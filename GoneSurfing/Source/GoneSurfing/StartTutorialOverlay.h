// Pure-C++ Slate pre-wave "Start" screen + control tutorial. Shown before the ride begins:
// the pawn pauses the world, installs this overlay, and unpauses when the player taps Start
// (from the menu or the last tutorial card). No UMG asset. Mirrors the Install/Uninstall-by-
// UWorld lifecycle of WaveRadarHUD / ReplayOverlayHUD. See specs/start-screen-tutorial.md.
#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"

class UWorld;

namespace StartTutorial
{
	/** Everything the hub needs from the game, so the widget stays free of actors. Mirrors the
	 *  BoardPanel / RideListPanel shape. See specs/two-screen-navigation.md FR1. */
	struct FHooks
	{
		/** Start pressed (menu button, or Start on the final tutorial card). */
		TFunction<void()> OnStart;

		/** REPLAY pressed: open the ride list over the hub. The button is drawn only while
		 *  HasRides() is true (best-ride-replay.md FR6: never a dead control). */
		TFunction<void()> OnReplay;
		TFunction<bool()> HasRides;

		/** CHANGE BOARD pressed: open the rack over the hub. The button carries the current board's
		 *  name so the choice is visible without opening the rack; an empty BoardLabel() means no
		 *  boards are installed and the button is not drawn. */
		TFunction<void()> OnChangeBoard;
		TFunction<FString()> BoardLabel;

		/** ABOUT pressed: open the credits / licences screen over the hub (specs/about-screen.md).
		 *  Drawn only when bound; unlike the two above it needs nothing to exist first. */
		TFunction<void()> OnAbout;

		/** The view switched: true when the instruction cards come up over the menu, false when the
		 *  menu is back. The pawn parks the resting touch controls it draws over the hub while a
		 *  card is up (specs/two-screen-navigation.md FR1a). Fires on the initial view too, from
		 *  inside Install, when -ShowInstructions opens straight on the cards. */
		TFunction<void(bool)> OnInstructionsView;
	};

	/** Add the start/tutorial overlay to the world's game viewport (fills the screen, ZOrder 200).
	 *  Hooks are invoked on the game thread. No-op if already installed. */
	GONESURFING_API void Install(UWorld* World, FHooks Hooks);

	/** Start-only form, for callers with no hub actions to offer. */
	GONESURFING_API void Install(UWorld* World, TFunction<void()> OnStart);

	/** Remove the overlay. Safe to call when not installed. */
	GONESURFING_API void Uninstall(UWorld* World);

	/** True while the overlay is installed for this world. */
	GONESURFING_API bool IsInstalled(UWorld* World);
}
