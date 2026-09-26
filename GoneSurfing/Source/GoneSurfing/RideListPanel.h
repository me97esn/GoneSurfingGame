// The ride list: "which ride do you want to watch?", opened by the one Replay control on the ride
// bar. See specs/best-ride-replay.md D10 and FR6.
//
// This exists because the spec had arrived at two Replay buttons differing only by which ride they
// played - the last one, and the best one - which is one button too many wherever the second one is
// standing. So Replay stops meaning "the last one" and starts meaning "which one?".
//
// Shape borrowed from BoardPanel: a namespaced Slate overlay with no UObject inside the widget, at
// the same ZOrder 260 modal tier, pausing the world only if nothing else already has.
//
// NFR3, and it is the thing most likely to break this screen: the button bar underneath is UMG.
// A live UMG widget under a live Slate panel means one of the two silently stops responding to
// taps. The caller must set ASurfboardPawn::bRideUIBlocked - the flag every UMG widget binds its
// Visibility to - before opening, and clear it on every path out.
#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"

class UWorld;

/** One row. Built by the pawn from the stored records, so the widget never touches config or disk. */
struct FRideListEntry
{
	/** "LAST RIDE" or "BEST" - which ride this is, not a label the widget invents. */
	FString KindLabel;

	/** Board display name, e.g. "SHORTBOARD". */
	FString BoardName;

	/** Board id, for the presentational swap when this row is played (D1). */
	FString BoardId;

	/** Points as the player reads them, board multiplier already applied. */
	int32 Score = 0;

	/** Ride length, seconds. Shown as m:ss - a 4-second wipeout should be visibly a wipeout. */
	float DurationSeconds = 0.0f;

	/** Absolute path of the trace to play. Already checked to exist (FR2). */
	FString TracePath;

	/** True if this row is the ride the player just did. Drawn first and emphasised: "watch what I
	 *  just did" is the common case and must stay the shortest path. */
	bool bIsLastRide = false;

	/** True if this is the board the player is on right now - the row's "you are here". */
	bool bIsCurrentBoard = false;
};

namespace RideListPanel
{
	/** Everything the list needs from the game, so the widget itself stays free of actors. */
	struct FHooks
	{
		/** Play this row. The panel closes itself first, so the callee owns the screen. */
		TFunction<void(int32 /*EntryIndex*/)> PlayEntry;

		/** Fired when the list closes by any route. The caller clears bRideUIBlocked here - it
		 *  cannot detect the close itself, because the list pauses the world and its owner stops
		 *  ticking. bToPlay is true when the close is the first half of a row press and PlayEntry
		 *  follows at once: a caller that would otherwise tear the world down on close (a hub
		 *  replay returning to the hub) must not, or the pick is thrown away with it. */
		TFunction<void(bool /*bToPlay*/)> OnClosed;
	};

	/** Open with these rows, in this order. An empty list is a no-op rather than an empty screen:
	 *  FR6 says the control that opens it should not be drawn when there is nothing to watch. */
	void Open(UWorld* World, const TArray<FRideListEntry>& Entries, const FHooks& Hooks,
	          int32 HighlightIndex = INDEX_NONE);

	bool IsOpen(UWorld* World);
	void Close(UWorld* World);
	void Uninstall(UWorld* World);
}
