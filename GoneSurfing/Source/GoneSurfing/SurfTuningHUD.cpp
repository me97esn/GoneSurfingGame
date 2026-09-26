// See SurfTuningHUD.h and specs/runtime-tuning.md.

#include "SurfTuningHUD.h"
#include "SurfLog.h"
#include "SurfTuningSubsystem.h"
#include "TraceShare.h"

#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "GameFramework/PlayerController.h"

#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SDPIScaler.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SNumericEntryBox.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Input/SSlider.h"
#include "Widgets/Text/STextBlock.h"
#include "Styling/AppStyle.h"

namespace
{
	// All UI state for a single installed HUD. Held by a TSharedPtr in the
	// per-world map below so Slate's shared-pointer lifetimes line up.
	struct FTuningHUDState
	{
		TWeakObjectPtr<UWorld> World;
		TWeakObjectPtr<USurfTuningSubsystem> Tuning;
		TSharedPtr<SWidget> RootWidget;
		TSharedPtr<SWidget> PanelWidget;
		bool bPanelOpen = false;
		// The SEND panel (TraceShare) shares the strip and the pause. Its content is rebuilt on
		// every open so the trace list is current; the holder is what stays in the tree.
		TSharedPtr<SBox> SharePanelHolder;
		bool bSharePanelOpen = false;

		// Live filter text from the search box at the top of the panel. Rows and
		// category headers read this each paint (via TAttribute) to show/hide
		// themselves; empty = show everything. Case-insensitive substring match.
		FString Filter;

		// Sliders / numeric entries use TAttribute lambdas that re-fetch each
		// paint, so no manual sync is required. Hook kept for future widgets
		// that cache instead of attributing.
		void RebuildSliderValues();
		TArray<TPair<FName, TSharedPtr<SSlider>>> Sliders;
	};

	// Per-world registry. PIE can spawn multiple worlds; production single-world
	// only ever has one entry. We key by raw UWorld* — entries are removed in
	// Uninstall, and GC of UWorld* is fine for raw-ptr keys because we never
	// dereference the key after the world is gone.
	TMap<UWorld*, TSharedPtr<FTuningHUDState>> GHUDStates;

	// Map an FFloatProperty value into the slider's 0..1 range. Uses the
	// subsystem's captured default × 4 as the "full" deflection so the slider
	// always sits roughly in the middle when current==default and at 1.0 at
	// 4×default. Zero-default falls back to clamping the raw value.
	float ValueToSliderUnits(float Value, float Default)
	{
		if (FMath::IsNearlyZero(Default))
		{
			return FMath::Clamp(Value, 0.0f, 1.0f);
		}
		return FMath::Clamp(Value / (4.0f * Default), 0.0f, 1.0f);
	}

	float SliderUnitsToValue(float SliderUnits, float Default)
	{
		if (FMath::IsNearlyZero(Default))
		{
			return FMath::Clamp(SliderUnits, 0.0f, 1.0f);
		}
		return SliderUnits * 4.0f * Default;
	}

	FString FormatFloat(float V)
	{
		// 4 significant figures, automatic between fixed and scientific based on
		// magnitude. The default tuning set spans 1e-5 to 1e+8.
		const float Abs = FMath::Abs(V);
		if (Abs > 0.0f && (Abs >= 1e5f || Abs < 1e-3f))
		{
			return FString::Printf(TEXT("%.3g"), V);
		}
		return FString::Printf(TEXT("%.4g"), V);
	}

	void SetGamePausedAndInputMode(UWorld* World, bool bPaused)
	{
		if (!World) return;
		UGameplayStatics::SetGamePaused(World, bPaused);
		APlayerController* PC = UGameplayStatics::GetPlayerController(World, 0);
		if (!PC) return;
		if (bPaused)
		{
			FInputModeUIOnly Mode;
			PC->SetInputMode(Mode);
			PC->SetShowMouseCursor(true);
		}
		else
		{
			// GameAndUI, not GameOnly: this HUD only exists when bShowTuningHUD is on, and
			// GameOnly's viewport capture would stop the gear button receiving further taps
			// — i.e. the panel would open exactly once per session and then go dead. Mirrors
			// ASurfboardPawn::ApplyGameplayInputMode's dev-build branch.
			FInputModeGameAndUI Mode;
			Mode.SetHideCursorDuringCapture(true);
			Mode.SetLockMouseToViewportBehavior(EMouseLockMode::LockOnCapture);
			PC->SetInputMode(Mode);
			PC->SetShowMouseCursor(false);
		}
	}

	TSharedRef<SWidget> BuildRow(
		const TSharedPtr<FTuningHUDState>& State,
		USurfTuningSubsystem* Tuning,
		FName PropertyName)
	{
		const float Default = Tuning->GetDefault(PropertyName);
		const float Current = Tuning->GetByName(PropertyName);

		TSharedPtr<SSlider> Slider;

		TWeakPtr<FTuningHUDState> WeakState = State;
		TWeakObjectPtr<USurfTuningSubsystem> WeakTuning = Tuning;

		auto OnSliderChanged = [WeakTuning, PropertyName, Default](float NewSliderValue)
		{
			USurfTuningSubsystem* T = WeakTuning.Get();
			if (!T) return;
			const float NewValue = SliderUnitsToValue(NewSliderValue, Default);
			T->SetByName(PropertyName, NewValue);
		};

		auto OnResetClicked = [WeakTuning, WeakState, PropertyName, Default]() -> FReply
		{
			USurfTuningSubsystem* T = WeakTuning.Get();
			if (!T) return FReply::Handled();
			T->ResetToDefault(PropertyName);
			TSharedPtr<FTuningHUDState> S = WeakState.Pin();
			if (S.IsValid())
			{
				S->RebuildSliderValues();
			}
			return FReply::Handled();
		};

		// Slider always reads its position via TAttribute so external setters
		// (numeric typing, JSON reload, future API) propagate without us pushing.
		auto SliderAttr = TAttribute<float>::CreateLambda([WeakTuning, PropertyName, Default]()
		{
			USurfTuningSubsystem* T = WeakTuning.Get();
			if (!T) return 0.0f;
			return ValueToSliderUnits(T->GetByName(PropertyName), Default);
		});

		// Numeric entry attribute — returns TOptional<float> so SNumericEntryBox
		// can show "—" if the subsystem is gone. The box accepts any float; the
		// slider stays in 0..1 (mapping to 0..4×default) but typing into the
		// numeric field bypasses that range — so values from 0 to 1000+ are fine
		// even when the default is 0.01.
		auto NumericAttr = TAttribute<TOptional<float>>::CreateLambda([WeakTuning, PropertyName]() -> TOptional<float>
		{
			USurfTuningSubsystem* T = WeakTuning.Get();
			if (!T) return TOptional<float>();
			return T->GetByName(PropertyName);
		});

		auto OnNumericCommitted = [WeakTuning, PropertyName](float NewValue, ETextCommit::Type)
		{
			USurfTuningSubsystem* T = WeakTuning.Get();
			if (T)
			{
				T->SetByName(PropertyName, NewValue);
			}
		};

		const FString PrettyName = PropertyName.ToString();

		// SHorizontalBox: [name | numeric input | slider | reset]
		TSharedRef<SWidget> Row = SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
				.FillWidth(0.40f)
				.VAlign(VAlign_Center)
				.Padding(4.0f, 2.0f)
				[
					SNew(STextBlock)
						.Text(FText::FromString(PrettyName))
						.ColorAndOpacity(FSlateColor(FLinearColor(0.85f, 0.85f, 0.85f)))
				]
			+ SHorizontalBox::Slot()
				.FillWidth(0.28f)
				.VAlign(VAlign_Center)
				.Padding(4.0f, 2.0f)
				[
					SNew(SNumericEntryBox<float>)
						.AllowSpin(false)
						.Value(NumericAttr)
						.OnValueCommitted_Lambda(OnNumericCommitted)
						.ToolTipText(FText::FromString(TEXT("Type any value — slider range is just for fine adjustment near the default")))
				]
			+ SHorizontalBox::Slot()
				.FillWidth(0.25f)
				.VAlign(VAlign_Center)
				.Padding(4.0f, 2.0f)
				[
					SAssignNew(Slider, SSlider)
						.MinValue(0.0f)
						.MaxValue(1.0f)
						.Value(SliderAttr)
						.OnValueChanged_Lambda(OnSliderChanged)
				]
			+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(4.0f, 2.0f)
				[
					SNew(SButton)
						.ContentPadding(FMargin(4.0f, 1.0f))
						.OnClicked_Lambda(OnResetClicked)
						.ToolTipText(FText::FromString(FString::Printf(TEXT("Reset to %s"), *FormatFloat(Default))))
						[
							SNew(STextBlock).Text(FText::FromString(TEXT("Reset")))
						]
				];

		State->Sliders.Emplace(PropertyName, Slider);
		return Row;
	}

	TSharedRef<SWidget> BuildPanel(
		const TSharedPtr<FTuningHUDState>& State,
		USurfTuningSubsystem* Tuning)
	{
		TSharedRef<SVerticalBox> Rows = SNew(SVerticalBox);

		TWeakPtr<FTuningHUDState> WeakState = State;

		// WHICH BOARD am I editing. Not decoration: edits are written to the active board's overlay
		// (Saved/BoardTuning/<id>.json), and a value looks identical on screen whichever of the four
		// layers set it. Without this it is entirely possible to dial in the foamie while believing
		// you are on the shortboard, and only discover it after switching back.
		{
			TWeakObjectPtr<USurfTuningSubsystem> WeakTuningForLabel = Tuning;
			Rows->AddSlot().AutoHeight().Padding(4.0f, 2.0f, 4.0f, 6.0f)
			[
				SNew(STextBlock)
				.ColorAndOpacity(FSlateColor(FLinearColor(1.0f, 0.78f, 0.34f)))
				.Text(TAttribute<FText>::CreateLambda([WeakTuningForLabel]()
				{
					const USurfTuningSubsystem* T = WeakTuningForLabel.Get();
					const FString Id = T ? T->GetActiveBoardId() : FString();
					return FText::FromString(Id.IsEmpty()
						? TEXT("editing: GLOBAL (no board installed)")
						: FString::Printf(TEXT("editing board: %s"), *Id));
				}))
			];
		}

		// Group rows by Category metadata. Sort categories for stable order;
		// within a category, preserve UPROPERTY declaration order (which is
		// the order TFieldIterator yields).
		TArray<FName> Names = Tuning->GetAllPropertyNames();
		TMap<FString, TArray<FName>> ByCategory;
		for (const FName& N : Names)
		{
			ByCategory.FindOrAdd(Tuning->GetCategoryForProperty(N)).Add(N);
		}
		TArray<FString> Categories;
		ByCategory.GetKeys(Categories);
		Categories.Sort();

		for (const FString& Cat : Categories)
		{
			// Category header hides itself when a filter is active and none of its
			// rows match, so filtering doesn't leave orphaned headers behind.
			const TArray<FName> CatNames = ByCategory[Cat];
			auto HeaderVis = TAttribute<EVisibility>::CreateLambda([WeakState, CatNames]() -> EVisibility
			{
				TSharedPtr<FTuningHUDState> S = WeakState.Pin();
				if (!S.IsValid() || S->Filter.IsEmpty()) return EVisibility::Visible;
				for (const FName& CN : CatNames)
				{
					if (CN.ToString().Contains(S->Filter)) return EVisibility::Visible;
				}
				return EVisibility::Collapsed;
			});

			Rows->AddSlot()
				.AutoHeight()
				.Padding(4.0f, 8.0f, 4.0f, 2.0f)
				[
					SNew(STextBlock)
						.Text(FText::FromString(Cat))
						.ColorAndOpacity(FSlateColor(FLinearColor(0.6f, 0.8f, 1.0f)))
						.Visibility(HeaderVis)
				];

			for (const FName& N : ByCategory[Cat])
			{
				// Each row re-evaluates its own visibility against the live filter
				// every paint (substring, case-insensitive) — no panel rebuild, so
				// the search box keeps focus while typing.
				auto RowVis = TAttribute<EVisibility>::CreateLambda([WeakState, N]() -> EVisibility
				{
					TSharedPtr<FTuningHUDState> S = WeakState.Pin();
					if (!S.IsValid() || S->Filter.IsEmpty()) return EVisibility::Visible;
					return N.ToString().Contains(S->Filter) ? EVisibility::Visible : EVisibility::Collapsed;
				});

				Rows->AddSlot().AutoHeight()
				[
					SNew(SBox)
						.Visibility(RowVis)
						[
							BuildRow(State, Tuning, N)
						]
				];
			}
		}

		// Live filter box, pinned above the scroll area so it stays put while the
		// list scrolls. Typing narrows rows to substring matches on the setting name
		// (e.g. "torque"); clearing it shows everything. FString::Contains defaults
		// to case-insensitive, so the match ignores case.
		TSharedRef<SWidget> SearchBox = SNew(SSearchBox)
			.HintText(FText::FromString(TEXT("Filter settings… (e.g. torque)")))
			.OnTextChanged_Lambda([WeakState](const FText& NewText)
			{
				if (TSharedPtr<FTuningHUDState> S = WeakState.Pin())
				{
					S->Filter = NewText.ToString();
				}
			});

		// Panel height capped at 540 logical units. On Android the SDPIScaler at the
		// root multiplies that by 2 → 1080 px, fitting a landscape phone screen
		// (typically 1080 px tall) without clipping.
		return SNew(SBox)
			.WidthOverride(560.0f)
			.HeightOverride(540.0f)
			[
				SNew(SBorder)
					.BorderImage(FAppStyle::GetBrush(TEXT("ToolPanel.GroupBorder")))
					.BorderBackgroundColor(FLinearColor(0.05f, 0.05f, 0.05f, 0.92f))
					.Padding(8.0f)
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot()
							.AutoHeight()
							.Padding(0.0f, 0.0f, 0.0f, 6.0f)
							[
								SearchBox
							]
						+ SVerticalBox::Slot()
							.FillHeight(1.0f)
							[
								SNew(SScrollBox) + SScrollBox::Slot() [ Rows ]
							]
					]
			];
	}

	TSharedRef<SWidget> BuildRoot(
		const TSharedPtr<FTuningHUDState>& State,
		USurfTuningSubsystem* Tuning)
	{
		TSharedRef<SBox> PanelHolder = SNew(SBox).Visibility(EVisibility::Collapsed);
		PanelHolder->SetContent(BuildPanel(State, Tuning));
		State->PanelWidget = PanelHolder;

		TWeakPtr<FTuningHUDState> WeakState = State;

		TSharedRef<SBox> ShareHolder = SNew(SBox).Visibility(EVisibility::Collapsed);
		State->SharePanelHolder = ShareHolder;

		// One panel at a time: both pause the world, and two open at once would stack in the strip.
		auto ShowPanels = [WeakState](bool bTune, bool bShare)
		{
			TSharedPtr<FTuningHUDState> S = WeakState.Pin();
			if (!S.IsValid()) return;
			S->bPanelOpen = bTune;
			S->bSharePanelOpen = bShare;
			if (S->PanelWidget.IsValid())
			{
				S->PanelWidget->SetVisibility(bTune ? EVisibility::Visible : EVisibility::Collapsed);
			}
			if (S->SharePanelHolder.IsValid())
			{
				if (bShare)
				{
					S->SharePanelHolder->SetContent(TraceShare::BuildPanel(S->World.Get()));
				}
				else
				{
					S->SharePanelHolder->SetContent(SNullWidget::NullWidget);   // drop the stale list
				}
				S->SharePanelHolder->SetVisibility(bShare ? EVisibility::Visible : EVisibility::Collapsed);
			}
			SetGamePausedAndInputMode(S->World.Get(), bTune || bShare);
		};

		auto OnToggle = [WeakState, ShowPanels]() -> FReply
		{
			TSharedPtr<FTuningHUDState> S = WeakState.Pin();
			if (!S.IsValid()) return FReply::Handled();
			ShowPanels(!S->bPanelOpen, false);
			return FReply::Handled();
		};

		auto OnShareToggle = [WeakState, ShowPanels]() -> FReply
		{
			TSharedPtr<FTuningHUDState> S = WeakState.Pin();
			if (!S.IsValid()) return FReply::Handled();
			ShowPanels(false, !S->bSharePanelOpen);
			return FReply::Handled();
		};

		// Phone screens have ~2× the pixel density of desktop monitors, so the
		// default Slate widget sizes render unreadably small. SDPIScaler wraps
		// the whole HUD and applies a render-time scale on Android — every
		// child (text, slider, numeric input, button, padding) doubles in
		// pixel size with no per-widget changes needed. PC keeps scale=1.0.
#if PLATFORM_ANDROID
		const float HUDScale = 2.0f;
#else
		const float HUDScale = 1.0f;
#endif

		// Top-CENTRE anchored stack: gear button on top, panel below it (hidden until clicked).
		// It was top-right until 2026-09-14, when the ride score moved into that corner (the
		// top-left went to the ride's Back button - specs/two-screen-navigation.md FR2). The top
		// centre is free now that the old UMG button bar is gone, and a dev badge this small can
		// sit on the screen's edge without landing on the surfer the way the score did there.
		//
		// SelfHitTestInvisible on the root, and it matters beyond this HUD. The root SOverlay is
		// allotted the WHOLE screen, and a panel left at the default Visible is hit-testable across
		// all of it — so at ZOrder 300 this blanketed every widget below and ate their clicks, while
		// its own buttons kept working. That is why the assist badge could be seen but never hovered.
		// SelfHitTestInvisible keeps the children (gear, panel) interactive while the empty expanse
		// around them passes input through. RideCueOverlay and ReplayOverlayHUD do the same thing
		// with plain HitTestInvisible because nothing in them is clickable.
		return SNew(SDPIScaler)
			.DPIScale(HUDScale)
			.Visibility(EVisibility::SelfHitTestInvisible)
			[
				SNew(SOverlay)
				.Visibility(EVisibility::SelfHitTestInvisible)
				+ SOverlay::Slot()
					.HAlign(HAlign_Center)
					.VAlign(VAlign_Top)
					.Padding(0.0f, 8.0f, 0.0f, 0.0f)
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot()
							.AutoHeight()
							.HAlign(HAlign_Center)
							[
								// TUNE and SEND side by side: two dev toggles in the one strip the
								// stamina bar already steps down from (StaminaBarOverlay).
								SNew(SHorizontalBox)
								+ SHorizontalBox::Slot().AutoWidth()
								[
									SNew(SBox)
										.WidthOverride(56.0f)
										.HeightOverride(40.0f)
										[
											SNew(SButton)
												.ContentPadding(FMargin(4.0f))
												.OnClicked_Lambda(OnToggle)
												.ToolTipText(FText::FromString(TEXT("Tuning panel — pauses game while open")))
												[
													SNew(STextBlock)
														.Justification(ETextJustify::Center)
														.Text(FText::FromString(TEXT("TUNE")))
												]
										]
								]
								+ SHorizontalBox::Slot().AutoWidth().Padding(6.0f, 0.0f, 0.0f, 0.0f)
								[
									SNew(SBox)
										.WidthOverride(56.0f)
										.HeightOverride(40.0f)
										[
											SNew(SButton)
												.ContentPadding(FMargin(4.0f))
												.OnClicked_Lambda(OnShareToggle)
												.ToolTipText(FText::FromString(TEXT("Send a ride trace to the PC — pauses game while open")))
												[
													SNew(STextBlock)
														.Justification(ETextJustify::Center)
														.Text(FText::FromString(TEXT("SEND")))
												]
										]
								]
							]
						+ SVerticalBox::Slot()
							.AutoHeight()
							.HAlign(HAlign_Center)
							.Padding(0.0f, 4.0f, 0.0f, 0.0f)
							[
								PanelHolder
							]
						+ SVerticalBox::Slot()
							.AutoHeight()
							.HAlign(HAlign_Center)
							.Padding(0.0f, 4.0f, 0.0f, 0.0f)
							[
								ShareHolder
							]
					]
			];
	}

	void FTuningHUDState::RebuildSliderValues()
	{
		// Sliders use TAttribute lambdas that re-fetch each tick, so explicit
		// rebuild isn't needed for the slider position. Kept as a hook in case
		// future widgets cache values instead of attributing them.
	}
}

namespace SurfTuningHUD
{
	void Install(UWorld* World)
	{
		// A second lock on the same door as ASurfboardPawn::IsTuningUIAvailable(). The pawn is the
		// only caller today; this is here so a future one cannot put TUNE and SEND in front of a
		// player by calling Install without knowing the rule. Written as a normal `if` on the
		// build macro rather than an #if block, so the builders below stay referenced in every
		// configuration and Shipping does not warn its way into an unused-function error.
		if (UE_BUILD_SHIPPING)
		{
			return;
		}
		if (!World)
		{
			return;
		}
		if (GHUDStates.Contains(World))
		{
			return; // already installed
		}
		UGameViewportClient* Viewport = World->GetGameViewport();
		if (!Viewport)
		{
			UE_LOG(LogSurf, Warning, TEXT("SurfTuningHUD::Install: no GameViewportClient"));
			return;
		}
		USurfTuningSubsystem* Tuning = SurfTuning::Get(World);
		if (!Tuning)
		{
			UE_LOG(LogSurf, Warning, TEXT("SurfTuningHUD::Install: SurfTuningSubsystem unavailable"));
			return;
		}

		TSharedPtr<FTuningHUDState> State = MakeShared<FTuningHUDState>();
		State->World = World;
		State->Tuning = Tuning;
		State->RootWidget = BuildRoot(State, Tuning);

		// ZOrder 300 — above every gameplay overlay: wave radar (90), replay HUD (100),
		// ride cue (150), start/tutorial (200). This is a developer HUD and must stay
		// reachable regardless of what else is on screen. It sat at 100 and was covered
		// by the start overlay, whose scrim slot is a full-screen hit-testable SBorder,
		// so taps on the gear went to the scrim instead. The clash went unnoticed because
		// bShowTuningHUD was off through all of the overlay work.
		Viewport->AddViewportWidgetContent(State->RootWidget.ToSharedRef(), /*ZOrder*/300);
		GHUDStates.Add(World, State);

		UE_LOG(LogSurf, Display, TEXT("SurfTuningHUD: Installed (%d tunables)."),
			Tuning->GetAllPropertyNames().Num());
	}

	bool IsInstalled(UWorld* World)
	{
		return World && GHUDStates.Contains(World);
	}

	void Uninstall(UWorld* World)
	{
		TSharedPtr<FTuningHUDState> State;
		if (!GHUDStates.RemoveAndCopyValue(World, State) || !State.IsValid())
		{
			return;
		}
		if (UGameViewportClient* Viewport = World ? World->GetGameViewport() : nullptr)
		{
			if (State->RootWidget.IsValid())
			{
				Viewport->RemoveViewportWidgetContent(State->RootWidget.ToSharedRef());
			}
		}
		// Clear input/pause carryover if the user left a panel open.
		if (State->bPanelOpen || State->bSharePanelOpen)
		{
			SetGamePausedAndInputMode(World, false);
		}
	}
}
