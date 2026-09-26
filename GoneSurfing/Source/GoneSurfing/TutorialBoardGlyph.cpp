// See TutorialBoardGlyph.h and specs/tutorial-live-feedback.md.

#include "TutorialBoardGlyph.h"
#include "Rendering/DrawElements.h"

namespace
{
	// Outlines are authored in a normalised box: x and y both -1..1, origin at the board's centre,
	// +x forward (nose), +y down. OnPaint rotates, then maps that box onto the widget.
	const float GlyphHalfSpanX = 1.15f;   // a little margin so a rolled board doesn't clip
	const float GlyphHalfSpanY = 0.95f;   // room under the waterline for the direction markers

	// The two board outlines are hand-drawn in Inkscape and converted to point lists by
	// Tools/svg2glyph.py. Keeping them as paths rather than textures means they stay crisp at any
	// size, tint to the accent colour for the success flash, and need no asset import.
	#include "TutorialBoardGlyphShapes.inc"

	void BuildTailOn(TArray<TArray<FVector2f>>& OutStrokes) { BuildTailOnShape(OutStrokes); }
	void BuildSideOn(TArray<TArray<FVector2f>>& OutStrokes) { BuildSideOnShape(OutStrokes); }
}

void STutorialBoardGlyph::Construct(const FArguments& InArgs)
{
	View = InArgs._View;
	LineColor = InArgs._LineColor;
	SuccessColor = InArgs._SuccessColor;
	SetCanTick(false);
	// The pump board runs off the edge on purpose, so it has to be cut off at the edge rather than
	// drawn across the illustration beside it.
	SetClipping(EWidgetClipping::ClipToBounds);
}

void STutorialBoardGlyph::SetPose(float InRollDeg, float InPitchDeg, float InTravel, float InSpeed)
{
	RollDeg  = InRollDeg;
	PitchDeg = InPitchDeg;
	Travel   = InTravel;
	Speed    = FMath::Clamp(InSpeed, 0.0f, 1.0f);
}

void STutorialBoardGlyph::SetSuccessFlash(float InFlash)
{
	SuccessFlash = FMath::Clamp(InFlash, 0.0f, 1.0f);
}

void STutorialBoardGlyph::SetDirectionProgress(bool bInTwoWay, bool bInPosDone, bool bInNegDone)
{
	bTwoWay  = bInTwoWay;
	bPosDone = bInPosDone;
	bNegDone = bInNegDone;
}

int32 STutorialBoardGlyph::OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry,
	const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements, int32 LayerId,
	const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	const FVector2f Size = FVector2f(AllottedGeometry.GetLocalSize());
	if (Size.X <= 0.0f || Size.Y <= 0.0f)
	{
		return LayerId;
	}

	// The pose the board is drawn in. Only the axis this view can express is applied — the others
	// would be invisible anyway, and letting them through would make the glyph twitch to gestures
	// the card is not teaching.
	const bool bSideOn = (View == ETutorialBoardView::SideOn);
	const float AngleDeg = bSideOn ? PitchDeg : RollDeg;
	const float AngleRad = FMath::DegreesToRadians(AngleDeg);
	const float S = FMath::Sin(AngleRad), C = FMath::Cos(AngleRad);

	// Travel carries the board bodily along its length. The pump card sends it right off the edge.
	const float SurgeX = bSideOn ? Travel : 0.0f;

	// Fit the normalised box into the widget, preserving aspect so the board is never stretched.
	// The success flourish rides on top as a small scale pop and a colour flash — enough to feel
	// like an answer, small enough not to reflow anything.
	const float Pop = 1.0f + 0.11f * FMath::Sin(SuccessFlash * PI);
	const float Scale = FMath::Min(Size.X / (2.0f * GlyphHalfSpanX), Size.Y / (2.0f * GlyphHalfSpanY)) * Pop;
	const FVector2f Centre = Size * 0.5f;

	const FLinearColor DrawColor = FMath::Lerp(LineColor, SuccessColor, SuccessFlash);

	auto ToLocal = [&](const FVector2f& P) -> FVector2f
	{
		const FVector2f Moved(P.X + SurgeX, P.Y);
		const FVector2f Rotated(Moved.X * C - Moved.Y * S, Moved.X * S + Moved.Y * C);
		return Centre + Rotated * Scale;
	};

	const FSlateLayoutTransform Identity;
	const FPaintGeometry Paint = AllottedGeometry.ToPaintGeometry();
	const ESlateDrawEffect Effects = ESlateDrawEffect::None;

	// A still waterline behind the board. Without a fixed reference a rotating shape is oddly hard
	// to read as "leaning" — the line is what turns it into an angle. Drawn as short dashes and
	// well below the hull: a solid full-width rule read as a strikethrough across the board.
	{
		const float WaterY = Centre.Y + 0.62f * Scale;
		const float HalfW = FMath::Min(Size.X * 0.5f - 2.0f, GlyphHalfSpanX * Scale);
		const int32 Dashes = 7;
		const float Span = (HalfW * 2.0f) / static_cast<float>(Dashes);
		for (int32 i = 0; i < Dashes; ++i)
		{
			const float X0 = Centre.X - HalfW + i * Span;
			TArray<FVector2f> Dash;
			Dash.Add(FVector2f(X0, WaterY));
			Dash.Add(FVector2f(X0 + Span * 0.55f, WaterY));
			FSlateDrawElement::MakeLines(OutDrawElements, LayerId, Paint, Dash,
				Effects, DrawColor.CopyWithNewOpacity(DrawColor.A * 0.30f), /*bAntiAlias*/ true, 1.5f);
		}
	}

	TArray<TArray<FVector2f>> Strokes;
	if (bSideOn) { BuildSideOn(Strokes); } else { BuildTailOn(Strokes); }

	for (const TArray<FVector2f>& Stroke : Strokes)
	{
		TArray<FVector2f> Points;
		Points.Reserve(Stroke.Num());
		for (const FVector2f& P : Stroke)
		{
			Points.Add(ToLocal(P));
		}
		FSlateDrawElement::MakeLines(OutDrawElements, LayerId + 1, Paint, Points,
			Effects, DrawColor, /*bAntiAlias*/ true, 3.0f);
	}

	// Speed lines off the tail while surging. Only while surging: a permanent decoration would stop
	// meaning anything, and the point of this view is that the pump did something.
	//
	// Deliberately heavy — heavier than the board's own outline. The player judging this is holding
	// a phone they are pumping, so anything that needs a steady gaze is invisible to them; the first
	// pass used three thin short strokes and could not be told from nothing.
	if (bSideOn && Speed > 0.04f)
	{
		const float Alpha = FMath::Clamp(Speed, 0.0f, 1.0f);
		for (int32 i = 0; i < 4; ++i)
		{
			const float Y = Centre.Y + (-0.22f + 0.15f * i) * Scale;
			// Lines grow with the pump, so the gesture's strength has somewhere to show. They trail
			// the board, so they follow it out of frame with it.
			//
			// Uneven lengths and starts, longest through the middle: four identical stacked rules
			// read as a barcode rather than as motion.
			const float Centred = 1.0f - FMath::Abs(i - 1.5f) / 1.5f;   // 0 outer, 1 middle
			const float Len = (0.30f + 0.55f * Alpha) * (0.55f + 0.45f * Centred) * Scale;
			const float X0 = Centre.X + (SurgeX - 0.98f - 0.16f * (1.0f - Centred)) * Scale;
			TArray<FVector2f> Line;
			Line.Add(FVector2f(X0 - Len, Y));
			Line.Add(FVector2f(X0, Y));
			FSlateDrawElement::MakeLines(OutDrawElements, LayerId + 1, Paint, Line,
				Effects, DrawColor.CopyWithNewOpacity(DrawColor.A * FMath::Min(1.0f, Alpha * 1.4f)),
				true, 4.0f);
		}
	}

	// Two markers, one per direction, under the board. They are the answer to "I tilted right and
	// nothing told me that counted": the first half of a two-way gesture used to be entirely silent,
	// so the only feedback was the board moving, which happens for any wobble.
	if (bTwoWay)
	{
		const float Y = Centre.Y + 0.80f * Scale;
		const float DX = 0.52f * Scale;
		const float W = 0.13f * Scale;
		for (int32 Side = -1; Side <= 1; Side += 2)
		{
			const bool bDone = (Side < 0) ? bNegDone : bPosDone;
			const FLinearColor MarkColor = bDone
				? SuccessColor.CopyWithNewOpacity(SuccessColor.A * LineColor.A)
				: LineColor.CopyWithNewOpacity(LineColor.A * 0.28f);

			// A chevron pointing the way it stands for, so an unlit one still reads as an
			// instruction rather than as a dead dot.
			const float X = Centre.X + Side * DX;
			TArray<FVector2f> Chevron;
			Chevron.Add(FVector2f(X - Side * W * 0.5f, Y - W * 0.75f));
			Chevron.Add(FVector2f(X + Side * W * 0.5f, Y));
			Chevron.Add(FVector2f(X - Side * W * 0.5f, Y + W * 0.75f));
			FSlateDrawElement::MakeLines(OutDrawElements, LayerId + 1, Paint, Chevron,
				Effects, MarkColor, /*bAntiAlias*/ true, bDone ? 3.5f : 2.0f);
		}
	}

	return LayerId + 2;
}
