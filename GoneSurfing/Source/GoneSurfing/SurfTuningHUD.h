// Pure-C++ Slate HUD for the runtime tuning subsystem. A gear button anchored
// top-right of the viewport toggles a panel containing one row per
// USurfTuningSubsystem float property (slider + numeric readout + reset).
// No UMG asset required — enumerates the subsystem via reflection and builds
// the row tree at install time.
//
// Install from ASurfboardPawn::BeginPlay. The HUD pauses the game while open
// and switches input to UIOnly so touch / mouse events go to the panel rather
// than the surfboard pawn. See specs/runtime-tuning.md Phase 1.

#pragma once

#include "CoreMinimal.h"

class UWorld;

namespace SurfTuningHUD
{
	/** Attach the gear toggle and (hidden) panel to the local player's viewport
	 *  for the given world. Safe to call multiple times — second + later calls
	 *  no-op if the HUD is already installed for that world. */
	GONESURFING_API void Install(UWorld* World);

	/** Detach and free the HUD. Called from ASurfboardPawn::EndPlay. */
	GONESURFING_API void Uninstall(UWorld* World);

	/** True while the TUNE toggle is on screen. The stamina bar shares its top-centre strip and
	 *  steps down out from under it while it is (dev builds only; shipping has no toggle). */
	GONESURFING_API bool IsInstalled(UWorld* World);
}
