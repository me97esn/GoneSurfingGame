// The board rack: every board, side by side, at true relative size. See specs/board-selection.md D6.
//
// This replaced a single-board description panel, and the reason is worth keeping. The first design
// had one tap both SELECT a board and EXPLAIN it, which does too much at once: a player tapping an
// unfamiliar outline was switched onto a board they had not yet understood. Splitting it means the
// game screen carries one icon and no explanation, and the choosing happens here, where there is
// room to explain and nothing is at stake until you pick.
//
// The rack is the whole idea: five boards standing bottoms-aligned on ONE shared scale, so the fact
// that a foamie is half again the length of a shortboard is read straight off the drawing. No
// figures, no copy. That is why the glyphs take an explicit PixelsPerUnit rather than each fitting
// its own box - fitting would flatten exactly the difference the screen exists to show.
//
// Shape borrowed from AssistPanel: a namespaced Slate overlay, no UObject inside the widget, and it
// pauses the world only if nothing else already has.
#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"

class UWorld;
struct FSurfBoardProfile;

namespace BoardPanel
{
	/** Everything the rack needs from the game, so the widget itself stays free of actors. */
	struct FHooks
	{
		/** The board actually applied and being ridden. The rack marks it RIDING NOW. */
		TFunction<int32()> GetActiveIndex;

		/** Take the picked board. Only the RIDE THE <board> button calls this - a card click is
		 *  provisional, so a player can read every board without committing to any of them. The
		 *  rack opens from the hub, between waves, so a pick applies on the spot and nothing needs
		 *  restarting (specs/two-screen-navigation.md FR5). It used to restart the level. */
		TFunction<void(int32)> ChooseBoard;

		/** Fired when the rack closes, by any route. The caller restores whatever it stood down
		 *  while the rack was up; it cannot detect the close itself, because the rack pauses the
		 *  world and its owner stops ticking. */
		TFunction<void()> OnClosed;

		/** Best ride on a given board, already converted to the points the player sees. Negative
		 *  means never ridden, which the card shows as a dash rather than a zero. */
		TFunction<int32(int32)> GetBestScore;

		/** Display name of a board, for the commit button to say which board it takes. */
		TFunction<FString(int32)> GetBoardName;
	};

	/** Open (installing on first call) with the boards in the order given, which is rack order.
	 *  Empty = nothing to choose from, and the call is a no-op rather than an empty screen. */
	void Open(UWorld* World, const TArray<FSurfBoardProfile>& Profiles, const FHooks& Hooks);

	bool IsOpen(UWorld* World);
	void Close(UWorld* World);
	void Uninstall(UWorld* World);
}
