// The wipeout card: a ride's ending. See specs/two-screen-navigation.md FR6.
//
// A fall used to change nothing the player could read: the rider ragdolled, the joystick
// vanished, the score froze, and the camera went on following the board as if the ride were still
// on. Testers could not tell it was over, let alone that Restart was the next thing to tap. This
// card is the ending - RIDE OVER (one title for every ending, owner 2026-09-22), the score, and the
// two things the player wants next: go again, or go back to the hub.
//
// Shape borrowed from RideListPanel: a namespaced Slate overlay on the modal tier (ZOrder 260),
// no UObject inside the widget. Unlike the rack and the list it does NOT pause the world: the
// surfer tumbling in the white water behind it IS the wipeout, and the card sits on top of that
// rather than freezing it.
#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"

class UWorld;

namespace WipeoutPanel
{
	struct FHooks
	{
		/** SURF AGAIN: straight into a fresh wave, hub skipped. */
		TFunction<void()> SurfAgain;

		/** BACK: end here and return to the hub. */
		TFunction<void()> Back;
	};

	/** Open the card. Title is RIDE OVER for every ending (owner, 2026-09-22; before that WIPEOUT or
	 *  LOST THE WAVE, each true only some of the time). Score is this ride's points; Best is the best
	 *  to beat (<= 0 hides the line). No-op if already open. */
	void Open(UWorld* World, const FText& Title, int32 Score, int32 Best, const FHooks& Hooks);

	bool IsOpen(UWorld* World);
	void Close(UWorld* World);
	void Uninstall(UWorld* World);
}
