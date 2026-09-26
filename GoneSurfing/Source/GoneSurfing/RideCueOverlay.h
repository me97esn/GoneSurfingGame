// Pure-C++ Slate cue for the control handoff: a "…and surf!" flash that fades out the moment
// the player takes control (PlayGo self-installs the overlay). Fired by
// ASurfboardPawn::AnnounceHandoff - and since 2026-09-12 only where no touch controls are drawn.
// Where they are, the controls appearing on that same tick is the cue, and it is the better one:
// it shows where to put your thumbs. Words on top of that were redundant. The steady ShowWaiting phase ("Wait...") is no
// longer used in-game — the skip-paddle intro (specs/skip-paddle-intro.md) shrank the
// no-control window to a ~2s cobra→pop-up beat that needs no explanation; the API remains for
// debugging.
#pragma once

#include "CoreMinimal.h"

class UWorld;

namespace RideCue
{
	/** Show (installing on first call) the steady "waiting" cue with the given text. Idempotent. */
	GONESURFING_API void ShowWaiting(UWorld* World, const FText& Text);

	/** Switch to the emphasized handoff text and fade it out. One-shot — ignored once triggered. */
	GONESURFING_API void PlayGo(UWorld* World, const FText& Text);

	/** Remove the cue. Safe when not installed. */
	GONESURFING_API void Uninstall(UWorld* World);
}
