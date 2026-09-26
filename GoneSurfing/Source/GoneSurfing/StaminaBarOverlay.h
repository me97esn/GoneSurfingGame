// The stamina bar: a thin ghost-tier bar at the top-centre of the ride screen. See
// specs/stamina.md FR4/FR5.
//
// It exists so the ending is never a surprise, not so the player manages a meter: visible from
// the moment draining arms (a bar that appeared at 20 % would read as a bug), wordless, and quiet
// until the last stretch, where it turns warm - the one moment it is allowed to ask for attention.
// Top-centre because Back has the top-left, the score the top-right, and the camera keeps the
// surfer dead centre VERTICALLY - the strip along the top edge, inside the 18 % the touch zones
// leave free, is the one place still empty.
//
// Same shape as RideHudOverlay: pure-C++ Slate, root SelfHitTestInvisible so it never eats a tap,
// one widget per world, Show installs on first use and Hide never installs.
#pragma once

#include "CoreMinimal.h"

class UWorld;

namespace StaminaBar
{
	/** Draw the bar at Fraction (0..1) of full. bLow switches to the warm "spend it" state. Installs
	 *  the widget on first call. bBelowTuneButton drops it under the dev TUNE toggle, which owns
	 *  the same top-centre strip in development builds. */
	void Show(UWorld* World, float Fraction, bool bLow, bool bBelowTuneButton);

	/** Hide without uninstalling. Does not install - the common case (feature off, automated runs)
	 *  must not spawn a widget just to hide it. */
	void Hide(UWorld* World);

	/** Remove the widget. Safe when not installed. */
	void Uninstall(UWorld* World);
}
