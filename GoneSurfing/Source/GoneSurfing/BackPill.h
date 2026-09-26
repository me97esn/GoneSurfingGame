// The one BACK control. See specs/two-screen-navigation.md FR2 and FR2a.
//
// Every screen that has a way out draws THIS widget, in the SAME corner, at the SAME size. It is a
// single function rather than five hand-built buttons because the five drifted: the ride and the
// replay shared a pill in the top-left, while the ride list, the wipeout card and the board rack
// each centred a BACK of their own further down the screen. The ride list is the one that hurt -
// TickReplay opens it over a finished replay on a timer, so the target moved from the corner to
// screen centre at the exact moment a thumb was travelling towards the corner.
//
// The rule this file exists to enforce: BACK never moves. Anything paired with it (SURF AGAIN,
// RIDE THE SHORTBOARD) keeps the centre; the exit keeps the corner.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SWidget.h"

namespace BackPill
{
	/** Resting offset from the corner, logical px, and the invisible hit margin around the pill.
	 *  Exposed so a caller can reserve space beside the pill - not so it can re-anchor it. */
	extern const float kEdgePadding;
	extern const float kHitMargin;

	/** The pill, anchored top-left of whatever it is given.
	 *
	 *  Drop it in a slot that spans the full viewport at the caller's own DPI scale - an SOverlay
	 *  slot set Fill/Fill is the usual shape - and it lands exactly where every other screen's BACK
	 *  lands. The returned widget is SelfHitTestInvisible, so it covers the screen without eating a
	 *  single tap meant for what is underneath; only the pill itself is hit-testable.
	 *
	 *  All five callers pass the SAME DPI scale (2.0 on Android, 1.0 elsewhere), which is what makes
	 *  "the same corner" mean the same pixels rather than merely the same anchor. */
	TSharedRef<SWidget> MakeAnchored(TFunction<void()> OnPressed);
}
