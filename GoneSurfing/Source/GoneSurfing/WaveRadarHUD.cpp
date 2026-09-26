// See WaveRadarHUD.h and specs/wave-radar.md.

#include "WaveRadarHUD.h"
#include "SurfLog.h"

#include "Engine/GameViewportClient.h"
#include "Engine/World.h"

#include "Widgets/SLeafWidget.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SDPIScaler.h"
#include "Rendering/DrawElements.h"
#include "Styling/SlateBrush.h"
#include "Brushes/SlateColorBrush.h"

namespace
{
	// Custom-painted leaf widget: draws a circular radar (face shading + foam dots + surfer board)
	// from the latest snapshot, with a white border ring.
	class SWaveRadar : public SLeafWidget
	{
	public:
		SLATE_BEGIN_ARGS(SWaveRadar) : _SizePx(200.0f) {}
			SLATE_ARGUMENT(float, SizePx)
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs)
		{
			SizePx = InArgs._SizePx;
		}

		void SetSnapshot(const FWaveRadarSnapshot& In) { Snapshot = In; }

		virtual FVector2D ComputeDesiredSize(float) const override
		{
			return FVector2D(SizePx, SizePx);
		}

		virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
			FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override
		{
			static const FSlateColorBrush WhiteBrush(FLinearColor::White);
			const FVector2D Size = AllottedGeometry.GetLocalSize();
			const FVector2D Center(Size.X * 0.5f, Size.Y * 0.5f);
			const float Radius = FMath::Min(Size.X, Size.Y) * 0.5f;

			// Filled dark disc background. Slate has no circle primitive, so fill it with horizontal
			// scanline boxes (cheap, and gives a clean round silhouette without custom vertex buffers).
			{
				const FLinearColor BgCol(0.02f, 0.03f, 0.05f, 0.6f);
				const int32 Step = 2;
				for (int32 yy = 0; (float)yy <= 2.0f * Radius; yy += Step)
				{
					const float Dy = (float)yy - Radius;
					const float HalfW = FMath::Sqrt(FMath::Max(0.0f, Radius * Radius - Dy * Dy));
					if (HalfW <= 0.0f) { continue; }
					FSlateDrawElement::MakeBox(
						OutDrawElements, LayerId,
						AllottedGeometry.ToPaintGeometry(FVector2D(2.0f * HalfW, (float)Step + 1.0f),
							FSlateLayoutTransform(1.0f, FVector2D(Center.X - HalfW, Center.Y - Radius + (float)yy))),
						&WhiteBrush, ESlateDrawEffect::None, BgCol);
				}
			}

			// Face cells (height ramp) + foam dots, clipped to the circle. gy high = ahead -> top.
			if (Snapshot.bValid && Snapshot.GridN > 0 &&
				Snapshot.HeightT.Num() >= Snapshot.GridN * Snapshot.GridN &&
				Snapshot.Foam.Num() >= Snapshot.GridN * Snapshot.GridN)
			{
				const int32 N = Snapshot.GridN;
				const float CellW = Size.X / (float)N;
				const float CellH = Size.Y / (float)N;
				const FLinearColor Trough(0.05f, 0.12f, 0.30f, 0.9f);
				const FLinearColor Crest(0.20f, 0.65f, 0.95f, 0.9f);
				const FLinearColor Foam(0.95f, 0.97f, 1.0f, 0.95f);

				for (int32 gy = 0; gy < N; ++gy)
				{
					for (int32 gx = 0; gx < N; ++gx)
					{
						const float Px = gx * CellW;
						const float Py = (N - 1 - gy) * CellH; // ahead (high gy) at top, behind at bottom

						// Circle clip: skip cells whose center is outside the radar disc.
						const FVector2D CellCenter(Px + CellW * 0.5f, Py + CellH * 0.5f);
						if ((CellCenter - Center).Size() > Radius - FMath::Max(CellW, CellH) * 0.5f) { continue; }

						const int32 Idx = gy * N + gx;
						const FLinearColor C = FMath::Lerp(Trough, Crest, FMath::Clamp(Snapshot.HeightT[Idx], 0.0f, 1.0f));

						FSlateDrawElement::MakeBox(
							OutDrawElements, LayerId + 1,
							AllottedGeometry.ToPaintGeometry(FVector2D(CellW + 1.0f, CellH + 1.0f), FSlateLayoutTransform(1.0f, FVector2D(Px, Py))),
							&WhiteBrush, ESlateDrawEffect::None, C);

						// White water: a soft foam blob whose size and opacity scale with the (temporally
						// smoothed) foam intensity. Blobs are drawn a bit larger than a cell so neighbours
						// overlap and merge into a continuous foam mass rather than discrete dots.
						const float FoamI = Snapshot.Foam[Idx];
						if (FoamI > 0.04f)
						{
							const float MinDim = FMath::Min(CellW, CellH);
							const float BlobSize = FMath::Lerp(0.45f, 1.25f, FMath::Clamp(FoamI, 0.0f, 1.0f)) * MinDim;
							const float Bx = Px + (CellW - BlobSize) * 0.5f;
							const float By = Py + (CellH - BlobSize) * 0.5f;
							FLinearColor FoamCol = Foam;
							FoamCol.A = FMath::Clamp(FoamI * 0.85f, 0.0f, 0.9f);
							FSlateDrawElement::MakeBox(
								OutDrawElements, LayerId + 2,
								AllottedGeometry.ToPaintGeometry(FVector2D(BlobSize, BlobSize), FSlateLayoutTransform(1.0f, FVector2D(Bx, By))),
								&WhiteBrush, ESlateDrawEffect::None, FoamCol);
						}
					}
				}
			}

			// Surfer marker: a surfboard outline at center, rotated by the board heading (0 = up, 90 = right).
			const float R = FMath::Min(Size.X, Size.Y) * 0.06f;
			const float Ang = FMath::DegreesToRadians(Snapshot.SurferAngleDeg);
			const float S = FMath::Sin(Ang), Cdir = FMath::Cos(Ang);
			// Rotate a local offset (x = right, y = up-negative) so that angle 0 -> up, 90 -> right.
			auto Rot = [&](float lx, float ly) -> FVector2D
			{
				return Center + FVector2D(lx * Cdir - ly * S, lx * S + ly * Cdir);
			};
			// Surfboard silhouette: pointed nose at -L (forward), widest at center, rounded tail at +L.
			const float L = (Snapshot.BoardHalfLenFrac > 0.0f) ? (Snapshot.BoardHalfLenFrac * Size.X) : (R * 0.9f); // world-scaled half length
			const float W = (Snapshot.BoardHalfWidFrac > 0.0f) ? (Snapshot.BoardHalfWidFrac * Size.X) : (R * 0.25f); // world-scaled half width
			const FLinearColor BoardColor(1.0f, 0.9f, 0.2f, 1.0f);
			TArray<FVector2D> Board;
			Board.Add(Rot(0.0f,       -L));          // nose tip
			Board.Add(Rot(W * 0.55f,  -L * 0.55f));  // nose shoulder (right)
			Board.Add(Rot(W,          -L * 0.05f));  // widest (right)
			Board.Add(Rot(W * 0.7f,    L * 0.6f));   // hip (right)
			Board.Add(Rot(W * 0.4f,    L));          // tail corner (right)
			Board.Add(Rot(-W * 0.4f,   L));          // tail corner (left)
			Board.Add(Rot(-W * 0.7f,   L * 0.6f));   // hip (left)
			Board.Add(Rot(-W,         -L * 0.05f));  // widest (left)
			Board.Add(Rot(-W * 0.55f, -L * 0.55f));  // nose shoulder (left)
			Board.Add(Rot(0.0f,       -L));          // close at nose
			FSlateDrawElement::MakeLines(
				OutDrawElements, LayerId + 2, AllottedGeometry.ToPaintGeometry(),
				Board, ESlateDrawEffect::None, BoardColor, true, 2.0f);

			// Stringer (center line) for a clearer board read.
			TArray<FVector2D> Stringer;
			Stringer.Add(Rot(0.0f, -L));
			Stringer.Add(Rot(0.0f,  L));
			FSlateDrawElement::MakeLines(
				OutDrawElements, LayerId + 2, AllottedGeometry.ToPaintGeometry(),
				Stringer, ESlateDrawEffect::None, FLinearColor(1.0f, 0.9f, 0.2f, 0.55f), true, 1.0f);

			// White circular border ring.
			{
				const int32 Segs = 48;
				TArray<FVector2D> Ring;
				Ring.Reserve(Segs + 1);
				for (int32 i = 0; i <= Segs; ++i)
				{
					const float A = 2.0f * PI * (float)i / (float)Segs;
					Ring.Add(Center + FVector2D(FMath::Cos(A), FMath::Sin(A)) * Radius);
				}
				FSlateDrawElement::MakeLines(
					OutDrawElements, LayerId + 3, AllottedGeometry.ToPaintGeometry(),
					Ring, ESlateDrawEffect::None, FLinearColor::White, true, FMath::Max(7.0f, Radius * 0.08f));
			}

			return LayerId + 4;
		}

	private:
		float SizePx = 200.0f;
		FWaveRadarSnapshot Snapshot;
	};

	struct FWaveRadarState
	{
		TSharedPtr<SWidget> RootWidget;
		TSharedPtr<SWaveRadar> Radar;
	};

	TMap<UWorld*, TSharedPtr<FWaveRadarState>> GRadarStates;
}

namespace WaveRadar
{
	void Install(UWorld* World, float RadarSizePx)
	{
		if (!World || GRadarStates.Contains(World))
		{
			return;
		}
		UGameViewportClient* Viewport = World->GetGameViewport();
		if (!Viewport)
		{
			return;
		}

#if PLATFORM_ANDROID
		const float Scale = 2.0f; // match SurfTuningHUD: phones have ~2x pixel density
#else
		const float Scale = 1.0f;
#endif

		TSharedPtr<FWaveRadarState> State = MakeShared<FWaveRadarState>();
		TSharedPtr<SWaveRadar> Radar;

		TSharedRef<SWidget> Root = SNew(SDPIScaler)
			.DPIScale(Scale)
			[
				SNew(SOverlay)
				+ SOverlay::Slot()
					.HAlign(HAlign_Left)
					.VAlign(VAlign_Bottom)
					.Padding(8.0f)
					[
						SNew(SBox)
							.WidthOverride(RadarSizePx)
							.HeightOverride(RadarSizePx)
							[
								SAssignNew(Radar, SWaveRadar).SizePx(RadarSizePx)
							]
					]
			];

		State->RootWidget = Root;
		State->Radar = Radar;
		Viewport->AddViewportWidgetContent(Root, /*ZOrder*/ 90);
		GRadarStates.Add(World, State);

		UE_LOG(LogSurf, Display, TEXT("WaveRadar: Installed."));
	}

	void Uninstall(UWorld* World)
	{
		TSharedPtr<FWaveRadarState> State;
		if (!GRadarStates.RemoveAndCopyValue(World, State) || !State.IsValid())
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

	void UpdateData(UWorld* World, const FWaveRadarSnapshot& Snapshot)
	{
		if (TSharedPtr<FWaveRadarState>* Found = GRadarStates.Find(World))
		{
			if ((*Found)->Radar.IsValid())
			{
				(*Found)->Radar->SetSnapshot(Snapshot);
			}
		}
	}
}
