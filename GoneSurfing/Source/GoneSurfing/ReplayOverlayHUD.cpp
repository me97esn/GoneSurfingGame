// See ReplayOverlayHUD.h and specs/replay-mode-clarity.md.

#include "ReplayOverlayHUD.h"
#include "SurfLog.h"

#include "Engine/GameViewportClient.h"
#include "Engine/World.h"

#include "Widgets/SLeafWidget.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Layout/SBox.h"
#include "Rendering/DrawElements.h"
#include "Brushes/SlateColorBrush.h"
#include "Styling/CoreStyle.h"
#include "Fonts/FontMeasure.h"
#include "Framework/Application/SlateApplication.h"

namespace
{
	FString FormatMMSS(float Seconds)
	{
		const int32 Total = FMath::Max(0, FMath::RoundToInt(Seconds));
		return FString::Printf(TEXT("%d:%02d"), Total / 60, Total % 60);
	}

	// Full-screen custom-painted leaf: letterbox bars + "REPLAY" badge + playback progress/timer,
	// plus a "REPLAY ENDED" end-card while holding on the final frame. HitTestInvisible so it never
	// eats touches meant for the on-screen controls underneath.
	class SReplayOverlay : public SLeafWidget
	{
	public:
		SLATE_BEGIN_ARGS(SReplayOverlay) {}
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs)
		{
			SetVisibility(EVisibility::HitTestInvisible);
			SetCanTick(true); // drive the badge-dot pulse
		}

		void SetSnapshot(const FReplayOverlaySnapshot& In) { Snapshot = In; }

		virtual FVector2D ComputeDesiredSize(float) const override { return FVector2D(100.0f, 100.0f); }

		// Accumulate real time for the badge-dot pulse (independent of the frozen playback cursor).
		virtual void Tick(const FGeometry&, const double InCurrentTime, const float InDeltaTime) override
		{
			AnimTime += InDeltaTime;
		}

		virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
			FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override
		{
			static const FSlateColorBrush WhiteBrush(FLinearColor::White);

			const FVector2D Size = AllottedGeometry.GetLocalSize();
			if (Size.X <= 0.0f || Size.Y <= 0.0f) { return LayerId; }

			// Resolution-independent UI scale (design baseline 1080p tall).
			const float Scale = FMath::Clamp(Size.Y / 1080.0f, 0.6f, 3.0f);
			const float Margin = 26.0f * Scale;
			const float BarH = FMath::Clamp(Size.Y * 0.065f, 24.0f, 150.0f); // letterbox bar height

			TSharedRef<FSlateFontMeasure> FontMeasure = FSlateApplication::Get().GetRenderer()->GetFontMeasureService();

			auto DrawBox = [&](const FVector2D& Pos, const FVector2D& Extent, const FLinearColor& Col, int32 Layer)
			{
				FSlateDrawElement::MakeBox(
					OutDrawElements, Layer,
					AllottedGeometry.ToPaintGeometry(Extent, FSlateLayoutTransform(1.0f, Pos)),
					&WhiteBrush, ESlateDrawEffect::None, Col);
			};
			auto DrawText = [&](const FString& Text, const FSlateFontInfo& Font, const FVector2D& Pos, const FLinearColor& Col, int32 Layer)
			{
				FSlateDrawElement::MakeText(
					OutDrawElements, Layer,
					AllottedGeometry.ToOffsetPaintGeometry(Pos),
					Text, Font, ESlateDrawEffect::None, Col);
			};

			int32 Layer = LayerId;

			// --- Letterbox bars (top + bottom): the primary "this is cinematic / a replay" signal.
			const FLinearColor Black(0.0f, 0.0f, 0.0f, 1.0f);
			DrawBox(FVector2D(0.0f, 0.0f), FVector2D(Size.X, BarH), Black, ++Layer);
			DrawBox(FVector2D(0.0f, Size.Y - BarH), FVector2D(Size.X, BarH), Black, Layer);

			// --- "REPLAY" badge, centred INSIDE the top letterbox bar. It used to sit top-left just
			// under the bar, which is exactly where the ride HUD's ‹ BACK pill lives (that overlay
			// draws at 2x DPI, this one at Size.Y/1080; the two never shared a coordinate space and
			// collided on the phone). The bar is black and empty; a badge in it is the film idiom
			// anyway, and it leaves the corner to the exit.
			{
				const float BadgePt = FMath::Min(34.0f * Scale, BarH * 0.55f);   // must fit the bar
				const FSlateFontInfo BadgeFont = FCoreStyle::GetDefaultFontStyle("Bold", FMath::RoundToInt(BadgePt));
				const float DotR = BadgePt * 0.32f;
				const float Gap = 10.0f * Scale;
				const FVector2D BadgeSize = FontMeasure->Measure(TEXT("REPLAY"), BadgeFont);
				const float RowW = DotR * 2.0f + Gap + BadgeSize.X;
				const float RowX = (Size.X - RowW) * 0.5f;
				const float RowCenterY = BarH * 0.5f;
				// Pulsing red dot, centred on the text's vertical midline.
				const float Pulse = 0.45f + 0.55f * (0.5f + 0.5f * FMath::Sin(AnimTime * 4.0f));
				const FLinearColor Dot(0.95f, 0.16f, 0.12f, Snapshot.bHolding ? 0.9f : Pulse);
				DrawBox(FVector2D(RowX, RowCenterY - DotR), FVector2D(DotR * 2.0f, DotR * 2.0f), Dot, ++Layer);
				DrawText(TEXT("REPLAY"), BadgeFont, FVector2D(RowX + DotR * 2.0f + Gap, RowCenterY - BadgeSize.Y * 0.5f),
					FLinearColor(1.0f, 1.0f, 1.0f, 0.95f), Layer);
			}

			// --- Playback progress bar + timer, along the bottom just above the bottom bar.
			{
				const float TrackH = 6.0f * Scale;
				const float TrackW = Size.X - 2.0f * Margin;
				const float TrackY = Size.Y - BarH - Margin * 0.6f - TrackH;
				const FLinearColor Track(1.0f, 1.0f, 1.0f, 0.16f);
				const FLinearColor Fill(0.55f, 0.85f, 1.0f, 0.95f);
				DrawBox(FVector2D(Margin, TrackY), FVector2D(TrackW, TrackH), Track, ++Layer);
				const float FillW = TrackW * FMath::Clamp(Snapshot.Progress, 0.0f, 1.0f);
				if (FillW > 0.0f)
				{
					DrawBox(FVector2D(Margin, TrackY), FVector2D(FillW, TrackH), Fill, ++Layer);
				}

				const FSlateFontInfo TimeFont = FCoreStyle::GetDefaultFontStyle("Bold", FMath::RoundToInt(24.0f * Scale));
				const FString TimeStr = FString::Printf(TEXT("%s / %s"),
					*FormatMMSS(Snapshot.CurrentSeconds), *FormatMMSS(Snapshot.TotalSeconds));
				const FVector2D TimeSize = FontMeasure->Measure(TimeStr, TimeFont);
				DrawText(TimeStr, TimeFont, FVector2D(Size.X - Margin - TimeSize.X, TrackY - TimeSize.Y - 4.0f * Scale),
					FLinearColor(1.0f, 1.0f, 1.0f, 0.85f), ++Layer);
			}

			// --- End-card: shown once playback holds on the final frame, so the freeze reads as
			//     intentional. Dim the scene and center a clear "REPLAY ENDED". No action hints:
			//     the ride list that opens over this is the actual set of choices.
			if (Snapshot.bHolding)
			{
				DrawBox(FVector2D(0.0f, 0.0f), Size, FLinearColor(0.0f, 0.0f, 0.0f, 0.45f), ++Layer);

				const FSlateFontInfo TitleFont = FCoreStyle::GetDefaultFontStyle("Bold", FMath::RoundToInt(64.0f * Scale));
				const FString Title = TEXT("REPLAY ENDED");
				const FVector2D TitleSize = FontMeasure->Measure(Title, TitleFont);
				DrawText(Title, TitleFont, FVector2D((Size.X - TitleSize.X) * 0.5f, (Size.Y - TitleSize.Y) * 0.5f),
					FLinearColor(1.0f, 1.0f, 1.0f, 0.98f), ++Layer);
			}

			return Layer + 1;
		}

	private:
		FReplayOverlaySnapshot Snapshot;
		float AnimTime = 0.0f;
	};

	struct FReplayOverlayState
	{
		TSharedPtr<SWidget> RootWidget;
		TSharedPtr<SReplayOverlay> Overlay;
	};

	TMap<UWorld*, TSharedPtr<FReplayOverlayState>> GReplayOverlayStates;
}

namespace ReplayOverlay
{
	void Install(UWorld* World)
	{
		if (!World || GReplayOverlayStates.Contains(World))
		{
			return;
		}
		UGameViewportClient* Viewport = World->GetGameViewport();
		if (!Viewport)
		{
			return;
		}

		TSharedPtr<FReplayOverlayState> State = MakeShared<FReplayOverlayState>();
		TSharedPtr<SReplayOverlay> Overlay;

		TSharedRef<SWidget> Root = SNew(SOverlay)
			.Visibility(EVisibility::HitTestInvisible)
			+ SOverlay::Slot()
				.HAlign(HAlign_Fill)
				.VAlign(VAlign_Fill)
				[
					SAssignNew(Overlay, SReplayOverlay)
				];

		State->RootWidget = Root;
		State->Overlay = Overlay;
		// ZOrder 100: above the scene and the wave radar; HitTestInvisible so the touch controls
		// (added by the level BP) still receive input regardless of stacking order.
		Viewport->AddViewportWidgetContent(Root, /*ZOrder*/ 100);
		GReplayOverlayStates.Add(World, State);

		UE_LOG(LogSurf, Display, TEXT("ReplayOverlay: Installed."));
	}

	void Uninstall(UWorld* World)
	{
		TSharedPtr<FReplayOverlayState> State;
		if (!GReplayOverlayStates.RemoveAndCopyValue(World, State) || !State.IsValid())
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
	}

	void UpdateData(UWorld* World, const FReplayOverlaySnapshot& Snapshot)
	{
		if (TSharedPtr<FReplayOverlayState>* Found = GReplayOverlayStates.Find(World))
		{
			if ((*Found)->Overlay.IsValid())
			{
				(*Found)->Overlay->SetSnapshot(Snapshot);
			}
		}
	}
}
