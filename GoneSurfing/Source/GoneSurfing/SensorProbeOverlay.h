// On-device sensor capture wizard. Walks the player through a scripted list of phone
// poses and motions, showing live sensor readouts on screen, and writes every motion
// value to a CSV under Saved/SensorProbe/ for offline analysis.
//
// Exists because the gyro-yaw fusion (specs/tilt-yaw-fusion.md) measured inert on device
// and the deploy-guess-redeploy loop is far too slow to debug blind. Ground truth first.
//
// Each step buffers the preceding ~2 s of samples, so a *motion* step ("swing the phone
// left, then press OK") captures the motion itself rather than the stillness after it.
//
// Install from ASurfboardPawn::BeginPlay when bRunSensorProbe is set; the pawn pushes one
// sample per Tick. See specs/sensor-probe.md.

#pragma once

#include "CoreMinimal.h"

class UWorld;

namespace SensorProbe
{
	/** Attach the wizard to the local player's viewport. No-op if already installed. */
	GONESURFING_API void Install(UWorld* World);

	/** Detach and free. Flushes any unwritten rows first. */
	GONESURFING_API void Uninstall(UWorld* World);

	/** True while the wizard is up — the pawn uses this to keep pushing samples. */
	GONESURFING_API bool IsActive(UWorld* World);

	/** Supply the pawn's calibrated tilt basis, used only to fill the derived pitch/roll
	 *  columns. Optional and best-effort: the pawn does not tick while the start screen has
	 *  the world paused, so this may never arrive and the columns then read zero. The raw
	 *  motion vectors — the ones that matter — do not depend on it.
	 *
	 *  Sampling itself is driven by the overlay's own Slate active timer, which keeps
	 *  running while the world is paused. It must NOT be driven from the pawn's Tick: the
	 *  first version was, and collected nothing at all because the probe is used on top of
	 *  the paused start screen. See specs/sensor-probe.md. */
	GONESURFING_API void SetTiltBasis(
		UWorld* World,
		const FVector& NeutralGravity,
		const FVector& TiltForwardAxis,
		const FVector& TiltRightAxis);
}
