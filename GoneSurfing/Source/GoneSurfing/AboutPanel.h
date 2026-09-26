// The About screen: who made the game, what it was made with, and the notices those parts require.
// See specs/about-screen.md.
//
// Three cards on the rack's scrim - the game, the credits, the legal - plus two text pages behind
// it (third-party licences, privacy & terms). The words live in Content/Legal/*.txt, not here: a
// credit changes whenever an asset does and a contact address changes whenever a mailbox does,
// and neither should need a compile. What this file owns is the layout and the two lines the
// Unreal Engine EULA (section 7a) fixes verbatim.
//
// Shape borrowed from BoardPanel: a namespaced Slate overlay, no UObject inside the widget, and it
// pauses the world only if nothing else already has - it opens from the hub, which has.
#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"

class UWorld;

namespace AboutPanel
{
	/** Everything the panel needs from the game, so the widget itself stays free of actors. */
	struct FHooks
	{
		/** Fired when the panel closes, by any route. The caller restores whatever it stood down
		 *  while the panel was up; it cannot detect the close itself, because the panel keeps the
		 *  world paused and its owner does not tick. */
		TFunction<void()> OnClosed;
	};

	/** Open (installing on first call). No-op while already open. */
	void Open(UWorld* World, const FHooks& Hooks);

	bool IsOpen(UWorld* World);
	void Close(UWorld* World);
	void Uninstall(UWorld* World);
}
