// See WipeoutPanel.h and specs/two-screen-navigation.md FR6.

#include "WipeoutPanel.h"

#include "BackPill.h"
#include "SurfLog.h"

#include "Engine/GameViewportClient.h"
#include "Engine/World.h"

#include "Widgets/SCompoundWidget.h"
#include "Widgets/SOverlay.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SDPIScaler.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SButton.h"
#include "Brushes/SlateColorBrush.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Styling/SlateTypes.h"
#include "Styling/CoreStyle.h"

namespace
{
	// Wipeout_ prefix: duplicate anonymous-namespace names across .cpp files break the Android
	// unity build, which desktop builds never catch.
	//
	// Lighter scrim than the rack and the list (0.90): those pause the wave and want it gone; this
	// one wants the tumble behind it visible, just dimmed enough for white text to hold on white
	// water.
	const FLinearColor Wipeout_Scrim(0.02f, 0.05f, 0.09f, 0.62f);
	const FLinearColor Wipeout_Ink  (1.00f, 1.00f, 1.00f, 0.95f);
	const FLinearColor Wipeout_Dim  (1.00f, 1.00f, 1.00f, 0.62f);

	// The start screen's primary tier: coral fill, dark ink. SURF AGAIN is the action that moves
	// the player forward, exactly what START is on the hub, so it wears the same colour.
	const FLinearColor Wipeout_Coral     (FColor(0xFF, 0x6B, 0x4A));
	const FLinearColor Wipeout_CoralHover(FColor(0xFF, 0x85, 0x68));
	const FLinearColor Wipeout_CoralPress(FColor(0xD9, 0x55, 0x38));
	const FLinearColor Wipeout_CoralInk  (FColor(0x2A, 0x0D, 0x05));

	FSlateFontInfo Wipeout_Bold(int32 Size) { return FCoreStyle::GetDefaultFontStyle("Bold", Size); }

	const FSlateBrush* Wipeout_ScrimBrush()
	{
		static const FSlateColorBrush B(FLinearColor::White);
		return &B;
	}

	const FButtonStyle& Wipeout_PrimaryStyle()
	{
		static const FButtonStyle Style = FButtonStyle()
			.SetNormal (FSlateRoundedBoxBrush(Wipeout_Coral,      34.0f))
			.SetHovered(FSlateRoundedBoxBrush(Wipeout_CoralHover, 34.0f))
			.SetPressed(FSlateRoundedBoxBrush(Wipeout_CoralPress, 34.0f))
			.SetNormalPadding(FMargin(0))
			.SetPressedPadding(FMargin(0));
		return Style;
	}

}

class SWipeoutPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SWipeoutPanel) {}
		SLATE_ARGUMENT(FText, Title)
		SLATE_ARGUMENT(int32, Score)
		SLATE_ARGUMENT(int32, Best)
		SLATE_ARGUMENT(WipeoutPanel::FHooks, Hooks)
		SLATE_EVENT(FSimpleDelegate, OnClosed)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		Hooks = InArgs._Hooks;
		OnClosed = InArgs._OnClosed;

		const bool bShowBest = InArgs._Best > 0;

		ChildSlot
		[
			SNew(SOverlay)
			// Full-screen scrim. Hit-testable on purpose: this is a modal, and a tap on the
			// tumbling surfer must not reach the stick zone underneath.
			+ SOverlay::Slot()
			[
				SNew(SBorder)
				.BorderImage(Wipeout_ScrimBrush())
				.BorderBackgroundColor(Wipeout_Scrim)
				.Padding(0)
			]
			+ SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Center)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
				[
					SNew(STextBlock)
					.Text(InArgs._Title)
					.Font(Wipeout_Bold(44))
					.ColorAndOpacity(FSlateColor(Wipeout_Ink))
					.ShadowOffset(FVector2D(2.0f, 3.0f))
					.ShadowColorAndOpacity(FLinearColor(0.0f, 0.0f, 0.0f, 0.55f))
				]
				// The score. The counter's ride-over surface, moved here: the number the player
				// was earning, and beneath it the best to beat - shown only when there is one, so
				// a first-ever ride is not told its best is zero.
				+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0.0f, 14.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text(FText::FromString(FString::FromInt(FMath::Max(0, InArgs._Score))))
					.Font(Wipeout_Bold(58))
					.ColorAndOpacity(FSlateColor(Wipeout_Ink))
					.ShadowOffset(FVector2D(2.0f, 2.0f))
					.ShadowColorAndOpacity(FLinearColor(0.0f, 0.0f, 0.0f, 0.8f))
				]
				+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0.0f, 2.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.Visibility(bShowBest ? EVisibility::Visible : EVisibility::Collapsed)
					.Text(FText::FromString(FString::Printf(TEXT("BEST  %d"), InArgs._Best)))
					.Font(Wipeout_Bold(22))
					.ColorAndOpacity(FSlateColor(Wipeout_Dim))
					.ShadowOffset(FVector2D(1.0f, 1.0f))
					.ShadowColorAndOpacity(FLinearColor(0.0f, 0.0f, 0.0f, 0.7f))
				]
				// SURF AGAIN alone, and centred. BACK used to sit beside it, dismissive-left, which
				// made this the fourth screen position the same control had - and the card drops
				// over a live ride the instant the player falls, so it moved the exit out from
				// under a thumb already travelling to the corner. The exit is in the corner now;
				// what stays here is the one thing the card is asking.
				+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0.0f, 30.0f, 0.0f, 0.0f)
				[
					SNew(SBox).HeightOverride(68.0f)
					[
						SNew(SButton)
						.ButtonStyle(&Wipeout_PrimaryStyle())
						.HAlign(HAlign_Center).VAlign(VAlign_Center)
						.ContentPadding(FMargin(34.0f, 0.0f))
						.OnClicked(this, &SWipeoutPanel::OnSurfAgainPressed)
						[
							SNew(STextBlock)
							.Font(Wipeout_Bold(20))
							.ColorAndOpacity(FSlateColor(Wipeout_CoralInk))
							.Text(NSLOCTEXT("GoneSurfing", "WipeoutSurfAgain", "SURF AGAIN"))
						]
					]
				]
			]

			// The exit, in the one corner it lives in on every screen. Last slot, over the scrim.
			+ SOverlay::Slot().HAlign(HAlign_Fill).VAlign(VAlign_Fill)
			[
				BackPill::MakeAnchored([this]() { OnBackPressed(); })
			]
		];
	}

private:
	WipeoutPanel::FHooks Hooks;
	FSimpleDelegate OnClosed;

	FReply OnSurfAgainPressed()
	{
		// Close first, so the world the hook reloads is never one with a card still on it.
		OnClosed.ExecuteIfBound();
		if (Hooks.SurfAgain) { Hooks.SurfAgain(); }
		return FReply::Handled();
	}

	void OnBackPressed()
	{
		OnClosed.ExecuteIfBound();
		if (Hooks.Back) { Hooks.Back(); }
	}
};

namespace
{
	struct FWipeoutState
	{
		TSharedPtr<SWidget> Root;
		TSharedPtr<SWipeoutPanel> Widget;
	};
	TMap<UWorld*, TSharedPtr<FWipeoutState>> GWipeoutStates;

	void Wipeout_Destroy(UWorld* World)
	{
		TSharedPtr<FWipeoutState> State;
		if (!GWipeoutStates.RemoveAndCopyValue(World, State) || !State.IsValid()) { return; }
		if (UGameViewportClient* Viewport = World ? World->GetGameViewport() : nullptr)
		{
			if (State->Root.IsValid())
			{
				Viewport->RemoveViewportWidgetContent(State->Root.ToSharedRef());
			}
		}
	}
}

namespace WipeoutPanel
{
	void Open(UWorld* World, const FText& Title, int32 Score, int32 Best, const FHooks& Hooks)
	{
		if (!World) { return; }

		// A stale entry can survive a PIE session (the map is keyed on a raw UWorld* and PIE reuses
		// those pointers); left alone, IsOpen() reads true for ever. Same guard as the ride list.
		if (TSharedPtr<FWipeoutState>* Existing = GWipeoutStates.Find(World))
		{
			if (Existing->IsValid() && (*Existing)->Widget.IsValid())
			{
				return;
			}
			GWipeoutStates.Remove(World);
		}

		UGameViewportClient* Viewport = World->GetGameViewport();
		if (!Viewport)
		{
			return;
		}

#if PLATFORM_ANDROID
		const float Scale = 2.0f;
#else
		const float Scale = 1.0f;
#endif

		TSharedPtr<FWipeoutState> State = MakeShared<FWipeoutState>();
		TSharedPtr<SWipeoutPanel> Widget;
		TSharedRef<SWidget> Root = SNew(SDPIScaler).DPIScale(Scale)
			[
				SAssignNew(Widget, SWipeoutPanel)
				.Title(Title)
				.Score(Score)
				.Best(Best)
				.Hooks(Hooks)
				.OnClosed(FSimpleDelegate::CreateLambda([World]() { Wipeout_Destroy(World); }))
			];
		State->Root = Root;
		State->Widget = Widget;

		// ZOrder 260: the modal tier. Deliberately no pause - see the header.
		Viewport->AddViewportWidgetContent(Root, /*ZOrder*/ 260);
		GWipeoutStates.Add(World, State);
		UE_LOG(LogSurf, Display, TEXT("WipeoutPanel: opened '%s' (score %d, best %d)"), *Title.ToString(), Score, Best);
	}

	bool IsOpen(UWorld* World)
	{
		if (!World) { return false; }
		const TSharedPtr<FWipeoutState>* Found = GWipeoutStates.Find(World);
		return Found && Found->IsValid() && (*Found)->Widget.IsValid();
	}

	void Close(UWorld* World)     { Wipeout_Destroy(World); }
	void Uninstall(UWorld* World) { Wipeout_Destroy(World); }
}
