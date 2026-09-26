// See StaminaBarOverlay.h and specs/stamina.md.

#include "StaminaBarOverlay.h"
#include "SurfLog.h"

#include "Engine/GameViewportClient.h"
#include "Engine/World.h"

#include "Widgets/SLeafWidget.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SDPIScaler.h"
#include "Rendering/DrawElements.h"
#include "Brushes/SlateRoundedBoxBrush.h"

namespace
{
	// The ride HUD's ghost tier (RideHudOverlay): a translucent track with a faint keyline. The fill
	// is plain white - the same ink as the captions and the Back label - so the bar reads as part of
	// the HUD rather than as a meter from another game. Low = the SURF AGAIN coral, the warm colour
	// the card and the score's earning state already use; nothing on the ride screen is red.
	const FLinearColor kTrackFill   (1.0f, 1.0f, 1.0f, 0.18f);
	const FLinearColor kTrackOutline(1.0f, 1.0f, 1.0f, 0.45f);
	const FLinearColor kFill        (1.0f, 1.0f, 1.0f, 0.85f);
	const FLinearColor kFillLow     (0.94f, 0.38f, 0.24f, 0.95f);

	// Logical px (2x on the phone, like every other overlay). 6 + 8 = 14 px from the top edge,
	// 8 px tall: well inside the 97 px the touch zones leave free, and shorter than the Back pill
	// so it does not compete with it on the same row.
	const float kTopPadding = 14.0f;
	// Under the dev TUNE toggle (SurfTuningHUD: 40 px tall at 8 px from the top): 8 + 40 + 6.
	// Still inside the touch zones' free strip. Shipping builds have no toggle and use kTopPadding.
	const float kTopPaddingBelowTune = 54.0f;
	const float kWidth      = 200.0f;
	const float kHeight     = 8.0f;

	const FSlateBrush* TrackBrush()
	{
		static const FSlateRoundedBoxBrush B = []
		{
			FSlateRoundedBoxBrush Brush(FLinearColor::White, kHeight * 0.5f);
			// The radius lives in the outline settings; set both together or the corners go square.
			Brush.OutlineSettings = FSlateBrushOutlineSettings(kHeight * 0.5f, kTrackOutline, 1.0f);
			return Brush;
		}();
		return &B;
	}

	const FSlateBrush* FillBrush()
	{
		static const FSlateRoundedBoxBrush B(FLinearColor::White, kHeight * 0.5f);
		return &B;
	}

	class SStaminaBar : public SLeafWidget
	{
	public:
		SLATE_BEGIN_ARGS(SStaminaBar) {}
		SLATE_END_ARGS()

		void Construct(const FArguments&)
		{
			SetVisibility(EVisibility::HitTestInvisible);
		}

		void Set(float InFraction, bool bInLow)
		{
			const float NewFraction = FMath::Clamp(InFraction, 0.0f, 1.0f);
			if (NewFraction == Fraction && bInLow == bLow)
			{
				return;
			}
			Fraction = NewFraction;
			bLow = bInLow;
			// Slate caches a leaf's paint until told otherwise; without this the bar stays at
			// whatever it first drew (the score counter's burst went missing the same way).
			Invalidate(EInvalidateWidgetReason::Paint);
		}

		virtual FVector2D ComputeDesiredSize(float) const override { return FVector2D(kWidth, kHeight); }

		virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& Geometry, const FSlateRect& Clip,
			FSlateWindowElementList& Out, int32 LayerId, const FWidgetStyle& Style,
			bool bParentEnabled) const override
		{
			const FVector2D Size = Geometry.GetLocalSize();
			// MakeBox paints with the tint passed here and ignores the brush's own colour (that is
			// SImage's job) - passing White drew the track solid and hid the fill under it.
			FSlateDrawElement::MakeBox(Out, LayerId, Geometry.ToPaintGeometry(), TrackBrush(),
				ESlateDrawEffect::None, kTrackFill);

			// The empty part grows from the right: what is left sits against the left edge, where
			// the eye reads "how much" first. Never narrower than its own end caps.
			const float FillW = FMath::Max(Fraction > 0.0f ? Size.Y : 0.0f, Size.X * Fraction);
			if (FillW > 0.0f)
			{
				FSlateDrawElement::MakeBox(Out, LayerId + 1,
					Geometry.ToPaintGeometry(FVector2f((float)FillW, (float)Size.Y), FSlateLayoutTransform()),
					FillBrush(), ESlateDrawEffect::None, bLow ? kFillLow : kFill);
			}
			return LayerId + 1;
		}

	private:
		float Fraction = 1.0f;
		bool  bLow = false;
	};

	struct FBarState
	{
		TSharedPtr<SWidget> Root;
		TSharedPtr<SBox> Slot;         // toggled Visible / Collapsed by Show / Hide
		TSharedPtr<SStaminaBar> Bar;
		SVerticalBox::FSlot* TopSlot = nullptr;   // its padding moves the bar under the TUNE toggle
		bool bBelowTune = false;
	};
	TMap<UWorld*, TSharedPtr<FBarState>> GBarStates;

	FBarState* Bar_Find(UWorld* World)
	{
		TSharedPtr<FBarState>* Found = World ? GBarStates.Find(World) : nullptr;
		return (Found && Found->IsValid()) ? Found->Get() : nullptr;
	}

	FBarState* Bar_Ensure(UWorld* World)
	{
		if (FBarState* Existing = Bar_Find(World))
		{
			// A stale entry can survive a PIE session (keyed on a raw UWorld* that PIE reuses). The
			// widget itself is the tell: gone means the viewport it lived in is gone.
			if (Existing->Bar.IsValid())
			{
				return Existing;
			}
			GBarStates.Remove(World);
		}
		UGameViewportClient* Viewport = World ? World->GetGameViewport() : nullptr;
		if (!Viewport)
		{
			return nullptr;
		}

#if PLATFORM_ANDROID
		const float Scale = 2.0f;
#else
		const float Scale = 1.0f;
#endif

		TSharedPtr<FBarState> State = MakeShared<FBarState>();
		TSharedRef<SWidget> Root = SNew(SDPIScaler).DPIScale(Scale)
			.Visibility(EVisibility::SelfHitTestInvisible)
			[
				SNew(SVerticalBox)
				.Visibility(EVisibility::SelfHitTestInvisible)
				+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0.0f, kTopPadding, 0.0f, 0.0f)
					.Expose(State->TopSlot)
				[
					SAssignNew(State->Slot, SBox)
					.Visibility(EVisibility::HitTestInvisible)
					.WidthOverride(kWidth)
					.HeightOverride(kHeight)
					[
						SAssignNew(State->Bar, SStaminaBar)
					]
				]
			];
		State->Root = Root;

		// ZOrder 250 with the Back pill and the touch controls: below the modal tier (260), so the
		// end card covers it.
		Viewport->AddViewportWidgetContent(Root, /*ZOrder*/ 250);
		GBarStates.Add(World, State);
		UE_LOG(LogSurf, Display, TEXT("StaminaBar: installed."));
		return State.Get();
	}
}

namespace StaminaBar
{
	void Show(UWorld* World, float Fraction, bool bLow, bool bBelowTuneButton)
	{
		if (FBarState* S = Bar_Ensure(World))
		{
			S->Slot->SetVisibility(EVisibility::HitTestInvisible);
			if (S->TopSlot && bBelowTuneButton != S->bBelowTune)
			{
				S->bBelowTune = bBelowTuneButton;
				S->TopSlot->SetPadding(FMargin(0.0f, bBelowTuneButton ? kTopPaddingBelowTune : kTopPadding, 0.0f, 0.0f));
			}
			S->Bar->Set(Fraction, bLow);
		}
	}

	void Hide(UWorld* World)
	{
		if (FBarState* S = Bar_Find(World))
		{
			if (S->Slot.IsValid())
			{
				S->Slot->SetVisibility(EVisibility::Collapsed);
			}
		}
	}

	void Uninstall(UWorld* World)
	{
		TSharedPtr<FBarState> State;
		if (!GBarStates.RemoveAndCopyValue(World, State) || !State.IsValid())
		{
			return;
		}
		if (UGameViewportClient* Viewport = World ? World->GetGameViewport() : nullptr)
		{
			if (State->Root.IsValid())
			{
				Viewport->RemoveViewportWidgetContent(State->Root.ToSharedRef());
			}
		}
	}
}
