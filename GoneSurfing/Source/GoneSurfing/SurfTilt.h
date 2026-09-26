// The device-motion → control-input mapping, as pure functions.
//
// Extracted from ASurfboardPawn so the start-screen tutorial can show a board reacting to the real
// gesture without re-deriving the maths. A copy would drift the first time either side was tuned,
// and a tutorial that teaches a subtly different mapping is worse than one that teaches nothing.
//
// Two rules this file keeps:
//   - No UObject, no world, no platform gating. Compiled everywhere so a desktop editor build
//     type-checks it; the Android toolchain is not always available on a dev box.
//   - All mutable state is a parameter, never a global or a member. Each consumer (the pawn, the
//     tutorial preview) owns its own FBasis and FYawState, so neither can disturb the other.
//
// See specs/tilt-yaw-fusion.md, specs/tilt-axis-swap-bug.md, specs/sensor-probe.md,
// specs/pumping.md and specs/tutorial-live-feedback.md.
#pragma once

#include "CoreMinimal.h"

namespace SurfTilt
{
	/** Device-frame axes derived from one neutral-pose gravity sample. */
	struct FBasis
	{
		/** Gravity at the neutral pose — the "down" the live sample is measured against. */
		FVector NeutralGravity = FVector::ZeroVector;

		/** Points "away from the player" at neutral. Tilting forward grows G's component along it. */
		FVector ForwardAxis = FVector::ZeroVector;

		/** Points "right of the player" at neutral. = ForwardAxis x NeutralGravity. */
		FVector RightAxis = FVector::ZeroVector;

		/** True once a non-degenerate gravity sample has been taken. */
		bool bCalibrated = false;

		/** How well-conditioned the axes are: |Fwd| before normalising, so 0 = the phone was flat
		 *  and the axes are meaningless, 1 = upright and they are exact. Because g.y is ~0 in every
		 *  measured pose this is ~|g.z|, the same quantity the grip swap and the yaw fusion use.
		 *  Below ~0.2 the axes should not be trusted — see Calibrate. */
		float Conditioning = 0.0f;

		bool IsUsable() const { return bCalibrated && !ForwardAxis.IsNearlyZero() && !RightAxis.IsNearlyZero(); }
	};

	/** Deflection knobs. The pawn fills these from USurfTuningSubsystem; the tutorial copies them,
	 *  so the preview deflects exactly as far as the ride does for the same gesture. */
	struct FTuning
	{
		float PitchDegreesForFull  = 25.0f;
		float RollDegreesForFull   = 25.0f;
		float DeadzoneDegrees      = 2.0f;
		bool  bInvertPitch         = true;
		bool  bInvertRoll          = false;
		float YawGain              = 1.0f;
		float YawSign              = 1.0f;   // +1 or -1
		float YawLeakSeconds       = 2.0f;
		float YawBiasSampleSeconds = 1.0f;
	};

	/** Gyro-integrator state. Mutated by ComputeWeight; reset whenever the basis is recalibrated. */
	struct FYawState
	{
		float AngleDeg    = 0.0f;
		float RateBias    = 0.0f;
		float BiasAccum   = 0.0f;
		float BiasElapsed = 0.0f;
		bool  bBiasReady  = false;

		void Reset() { *this = FYawState(); }
	};

	/** The offset, plus every intermediate the debug log and the trace recorder want. Returning
	 *  them beats recomputing them at the call site and risking the two drifting apart. */
	struct FResult
	{
		/** X = sideways, Y = fore/aft. Both -1..1, deadzoned and scaled — what the pawn writes
		 *  to CurrentWeightOffset. */
		FVector2D Offset = FVector2D::ZeroVector;

		float PitchDeg       = 0.0f;  // raw signed, pre-deadzone
		float RollDeg        = 0.0f;  // fused (gravity + gyro), pre-deadzone
		float GravityRollDeg = 0.0f;  // the gravity channel alone
		float YawWeight      = 0.0f;  // how much authority the gyro channel currently has
		float RawYawRate     = 0.0f;  // gyro about gravity, rad/s, pre-bias-removal
	};

	/** Build the axes from a neutral gravity sample. Degenerate when the phone is flat: see the
	 *  comment on the implementation, and FBasis::Conditioning for how to detect it. */
	FBasis Calibrate(const FVector& Gravity);

	/** Soft deadzone and scale for one axis: within DZ → 0; past DZ → (angle-DZ)/(Full-DZ), signed.
	 *  Exposed separately because trace replay starts from recorded *angles* rather than from
	 *  gravity, and used to keep its own verbatim copy of this. */
	float AxisOffset(float DeltaDeg, float FullDeg, float DeadzoneDeg, bool bInvert);

	/** One tick of the mapping. Mutates InOutYaw (the gyro integrator). */
	FResult ComputeWeight(
		const FBasis& Basis,
		const FTuning& Tuning,
		FYawState& InOutYaw,
		const FVector& Gravity,
		const FVector& RotationRate,
		float DeltaTime);

	struct FPumpTuning
	{
		float LowPassHz    = 10.0f;
		float Deadzone     = 1.5f;
		float AccelForFull = 7.0f;
	};

	/** Pump signal, 0..1. Needs no basis — it reads gravity's direction and the acceleration along
	 *  it — so unlike ComputeWeight it stays correct in every grip, flat included. */
	float ComputePump(
		const FVector& Gravity,
		const FVector& Acceleration,
		const FPumpTuning& Tuning,
		float& InOutLowPass,
		float DeltaTime);
}
