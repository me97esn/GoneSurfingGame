// Force-application gate for the scripted "rails" intro.
//
// During a rails intro the board is driven kinematically from a recorded pose track, but the force
// pipeline must keep TICKING so its derived state stays warm — planing, relative water velocity and
// the wave-mass smoothing filters all need a few hundred ms of coherent input before physics
// resumes. What must not happen is the pipeline actually pushing the board around: the pose track is
// authoritative, and impulses on a kinematic body are no-ops that log a warning per call.
//
// So this is the "compute, don't apply" switch. It is deliberately NOT the same thing as
// ASurfboardPawn::SetForcePipelineTicking(false), which stops those actors ticking entirely — that
// is right for the terminal ride replay (a viewing mode) and wrong here.
//
// See specs/deterministic-ride-handoff.md.

#pragma once

#include "CoreMinimal.h"

namespace SurfRails
{
	/** True while a rails intro is driving the board: every force-application site returns early,
	 *  every force-COMPUTATION site runs as normal. */
	GONESURFING_API bool AreForcesSuppressed();

	/** Set by the rails driver on entry/exit. Always paired — an early-out that leaves this true
	 *  would silently disable the whole force pipeline for the rest of the session. */
	GONESURFING_API void SetForcesSuppressed(bool bSuppressed);

	/** True when the board is NOT simulating yet still has a real velocity: the rails drive it via
	 *  Chaos kinematic targets, which derive V and W from the motion, and the velocity getters read
	 *  the particle with no simulating-physics check.
	 *
	 *  Code that guards a velocity read with IsSimulatingPhysics() must OR this in, or it will read
	 *  the board as stationary for the whole intro — silently feeding zero into every force
	 *  computation instead of merely logging about it. Backed by the same flag as
	 *  AreForcesSuppressed(); separate name because the call sites mean different things. */
	GONESURFING_API bool IsKinematicallyDriven();
}
