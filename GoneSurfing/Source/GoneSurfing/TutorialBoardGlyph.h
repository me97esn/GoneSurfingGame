// The little board that reacts to the player's gesture on the tutorial cards.
//
// Line art rather than a filled silhouette, to sit beside the hand-drawn instruction sketches
// without looking like it came from a different program. Drawn rather than imported so it can be
// posed continuously — the whole point is that it moves with the phone.
//
// Which way the board faces is per-step, so the axis being taught always faces the player: tail-on
// for the sideways card so a lean is a roll, side-on for fore/aft so the nose visibly lifts, and
// side-on again for pump so it has somewhere to surge. See specs/tutorial-live-feedback.md FR1.
#pragma once

#include "CoreMinimal.h"
#include "Widgets/SLeafWidget.h"

/** Which way the drawn board faces. */
enum class ETutorialBoardView : uint8
{
	/** Seen from behind, tail-on. Rolls onto its edge. */
	TailOn,
	/** Seen from the side, nose to the right. Pitches, and surges forward. */
	SideOn,
};

class STutorialBoardGlyph : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(STutorialBoardGlyph)
		: _View(ETutorialBoardView::TailOn)
		, _LineColor(FLinearColor::White)
		, _SuccessColor(FLinearColor::White)
	{}
		SLATE_ARGUMENT(ETutorialBoardView, View)
		SLATE_ARGUMENT(FLinearColor, LineColor)
		/** What the board flashes to when the gesture lands. */
		SLATE_ARGUMENT(FLinearColor, SuccessColor)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** Pose the board. Angles in degrees — the *display* pose, exaggeration already applied.
	 *
	 *  Travel slides the board along its own length (SideOn only), in outline units: 0 is centred
	 *  and past about +-1.4 it is off the edge of the widget, which clips. The pump card drives this
	 *  so the board visibly launches out of frame and comes back round, rather than nudging forward
	 *  — a nudge is not something you can see on a phone you are busy pumping.
	 *
	 *  Speed is 0..1 and only sets how hard the speed lines read. */
	void SetPose(float InRollDeg, float InPitchDeg, float InTravel, float InSpeed);

	/** The success flourish (FR6). Flash is 1 at the moment the gesture lands and decays to 0; the
	 *  colour and a small scale pop ride on it. */
	void SetSuccessFlash(float InFlash);

	/** Which directions the player has reached so far, drawn as two markers under the board.
	 *  Without this the first half of a two-way gesture is completely silent, and the player has no
	 *  way to know the first tilt counted. bTwoWay off (the pump card) hides them entirely. */
	void SetDirectionProgress(bool bInTwoWay, bool bInPosDone, bool bInNegDone);

	virtual FVector2D ComputeDesiredSize(float) const override { return FVector2D(150.0f, 110.0f); }

	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry,
		const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements, int32 LayerId,
		const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;

private:
	ETutorialBoardView View = ETutorialBoardView::TailOn;
	FLinearColor LineColor = FLinearColor::White;
	FLinearColor SuccessColor = FLinearColor::White;
	float SuccessFlash = 0.0f;

	bool bTwoWay = false;
	bool bPosDone = false;
	bool bNegDone = false;

	float RollDeg = 0.0f;
	float PitchDeg = 0.0f;
	float Travel = 0.0f;
	float Speed = 0.0f;
};
