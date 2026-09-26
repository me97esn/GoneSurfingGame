// The SEND panel: post a ride trace from the phone to the dev PC, mid-run, no cable.
//
// The loop this serves (specs/share-trace-from-phone.md): the owner rides on the phone, something
// ends the ride when it should not have (or fails to), and the question "what did the game decide?"
// needs the trace on the PC where the analysis tools are - now, from a remote-control chat, not
// tonight over USB. A trace is a CSV of a few hundred KB, so the transport is one HTTP POST to a
// listener on the PC (Tools/TraceShareServer.py), reached over Tailscale from anywhere.
//
// Dev builds only: lives behind the same bShowTuningHUD gate as the TUNE toggle, which is where its
// button sits. Nothing here is reachable when that flag is off.

#pragma once

#include "CoreMinimal.h"

class UWorld;
class SWidget;

namespace TraceShare
{
	/** Build a fresh panel listing the newest traces on disk, newest first, each with a SEND
	 *  button. Rebuilt on every open so the list is current; a send still in flight when the panel
	 *  is rebuilt completes on its own and logs. The live ride, if one is recording, is flushed
	 *  first and marked as such. */
	GONESURFING_API TSharedRef<SWidget> BuildPanel(UWorld* World);

	/** Where traces are posted. Empty = not configured; the panel says so instead of a dead button.
	 *  Read from the surf.traceshare.url CVar (set in DefaultEngine.ini [SystemSettings]). */
	GONESURFING_API FString DestinationUrl();
}
