// See SurfTilt.h. Moved out of ASurfboardPawn unchanged — the comments below are the record of two
// bugs that cost a lot to find, so they travel with the code they explain.

#include "SurfTilt.h"

namespace SurfTilt
{

FBasis Calibrate(const FVector& Gravity)
{
	FBasis Basis;
	Basis.NeutralGravity = Gravity;
	Basis.bCalibrated = !Gravity.IsNearlyZero();
	if (!Basis.bCalibrated)
	{
		return Basis;
	}

	// Derive the two device-frame axes the tilt deltas are measured against. Gram-Schmidt: project
	// a fixed seed onto the plane perpendicular to gravity.
	//
	// The only +Z-degenerate pose is screen normal parallel to gravity: phone laid flat, screen-up
	// or screen-down. Unreachable in surfing — the player can't see the screen. See
	// specs/tilt-axis-swap-bug.md.
	const FVector N = Basis.NeutralGravity.GetSafeNormal();

	// Seed is the SCREEN NORMAL, which is device -X. It is NOT device +Z: that is the phone's
	// in-plane vertical axis (screen top-to-bottom). Measured 2026-08-20 (specs/sensor-probe.md) —
	// |g.x| = 1.00 with the phone flat, |g.z| = 1.00 with it upright.
	//
	// The old +Z seed made |Fwd| = |cos(tilt-from-flat)|, i.e. it degenerated exactly at vertical —
	// the play pose — and flipped sign across it:
	//
	//     pose      |Fwd| with +Z   |Fwd| with +/-X
	//     flat           1.000          0.018
	//     upright        0.048          0.999   <- play pose
	//     reclined       0.390          0.921   <- play pose
	//
	// Near-degenerate, the stray g.y (~0.03) dominates what is left of Fwd, so Fwd swings toward
	// +/-Y and Right = Fwd x N swings toward -/+X: the pitch and roll axes effectively swap, and
	// pitching the phone drives sideways weight. That is the reported symptom.
	//
	// +X, not -X. With N = (-cos t, 0, sin t) for tilt-from-flat t:
	//     seed +Z  ->  Fwd = sign(cos t) * (sin t, 0, cos t)   flips at vertical
	//     seed +X  ->  Fwd =              (sin t, 0, cos t)   consistent
	//     seed -X  ->  Fwd =             -(sin t, 0, cos t)   consistent, inverted
	// +X reproduces the old BELOW-vertical direction at every angle, so bInvertPitch / bInvertRoll
	// keep their playtested defaults and the gyro's MeasuredYawPolarity stays as measured.
	//
	// -X was tried first and inverted pitch (playtest 2026-08-20). The mistake was checking
	// sign-preservation against the captured *reclined* pose only — which is past vertical,
	// precisely where the old seed had already flipped. That "verified" the broken sign instead of
	// the working one. Below vertical is the reference, because that is where the old behaviour was
	// correct.
	//
	// The remaining degenerate pose is phone-flat, which is what specs/tilt-axis-swap-bug.md
	// originally reasoned about and dismissed as unreachable while playing. That argument was
	// written for a screen-normal seed; it is only actually true now. It is also not true of the
	// *tutorial*, which is read in whatever grip the player likes — hence Conditioning, which the
	// tutorial uses to hide its feedback rather than show a wrong axis
	// (specs/tutorial-live-feedback.md).
	const FVector Seed(1.0f, 0.0f, 0.0f);
	const FVector Fwd = Seed - FVector::DotProduct(Seed, N) * N;

	Basis.Conditioning = Fwd.Size();
	Basis.ForwardAxis = Fwd.GetSafeNormal();
	Basis.RightAxis = FVector::CrossProduct(Basis.ForwardAxis, N).GetSafeNormal();
	return Basis;
}

/** Gyro yaw fusion (specs/tilt-yaw-fusion.md). Returns the fused roll angle in degrees. */
static float AdvanceYawFusion(
	const FVector& GravityUnit,
	const FVector& RotationRate,
	float GravityRollDeg,
	const FTuning& Tuning,
	FYawState& Yaw,
	float DeltaTime,
	FResult& OutResult)
{
	// GravityRollDeg measures "is one end of the phone lower than the other". A rotation *about the
	// gravity vector* leaves gravity unchanged in the device frame, so that whole gesture is
	// invisible to it: the sideways gesture's sensitivity falls off as cos(theta) as the phone tips
	// toward vertical (theta = tilt from flat), hitting exactly zero at vertical and inverting past
	// it. That is precisely the pose a reclining player holds.
	//
	// The gyro can see it. Because the yaw axis IS gravity's null space, the two channels are
	// orthogonal by construction and are summed rather than blended — lerping would discard the
	// gravity channel at vertical, which is where the steering-wheel *twist* gesture is at full
	// sensitivity.

	// Angular velocity about the live gravity axis. RotationRate and Gravity share a frame (engine
	// CVar AndroidUnifyMotionSpace defaults to 1, which reorients all four motion vectors into the
	// same screen-relative space).
	const float RawYawRate = FVector::DotProduct(RotationRate, GravityUnit);
	OutResult.RawYawRate = RawYawRate;

	// Average the gyro's zero-rate bias over a short window after calibration. Unremoved, a typical
	// 1-2 deg/s MEMS bias against the leak parks the input outside the deadzone — i.e. a
	// permanently stuck turn.
	if (!Yaw.bBiasReady)
	{
		Yaw.BiasAccum += RawYawRate * DeltaTime;
		Yaw.BiasElapsed += DeltaTime;
		if (Yaw.BiasElapsed >= Tuning.YawBiasSampleSeconds)
		{
			Yaw.RateBias = Yaw.BiasAccum / FMath::Max(Yaw.BiasElapsed, KINDA_SMALL_NUMBER);
			Yaw.bBiasReady = true;
		}
	}

	// Integrate unconditionally. The bias estimate refines the integrand once ready (RateBias is 0
	// until then) but must never gate the integration itself: as an else-branch it was a single
	// point of total failure — any path that left bBiasReady false pinned the angle at exactly 0,
	// which is indistinguishable from a dead sensor and immune to both gain and sign. Drift while
	// un-biased is bounded by the leak below.
	Yaw.AngleDeg += FMath::RadiansToDegrees(RawYawRate - Yaw.RateBias) * DeltaTime;

	// Leak back to neutral. Bounds residual bias and discards slow whole-body rotation; affordable
	// because centred is already this input's resting state.
	const float Leak = FMath::Max(Tuning.YawLeakSeconds, KINDA_SMALL_NUMBER);
	Yaw.AngleDeg -= Yaw.AngleDeg * FMath::Min(DeltaTime / Leak, 1.0f);

	// Device Z is the phone's IN-PLANE VERTICAL axis (screen top-to-bottom), which is the swing
	// gesture's pivot. Gravity is blind to rotation about an axis parallel to itself, so |g.z| IS
	// the blindness — and therefore the gyro's weight directly.
	//
	// Measured on device 2026-08-20 (specs/sensor-probe.md), gravity at the capture mark:
	//     flat      g = (-1.004, -0.018,  0.001)   |g.z| = 0.00
	//     upright   g = ( 0.043,  0.022,  1.004)   |g.z| = 1.00
	//     reclined  g = ( 0.393, -0.028,  0.930)   |g.z| = 0.92
	// and the motion steps confirm the axes: swing pivots about Z, board-pitch about Y (the long
	// axis), twist about X.
	//
	// This is the opposite of what this code originally assumed. It took Z for the screen normal
	// and used YawWeight = 1 - |g.z|, which scaled the gyro down to 0.08 in the reclined pose — the
	// exact pose the fusion exists to fix — making the term look inert on device and immune to both
	// its gain and its sign. Device X is the screen normal (|g.x| = 1.00 when flat), not Z.
	//
	// Robust across both landscape orientations: the engine's reorient gives out.Z = +in.X
	// (LandscapeLeft) or -in.X (LandscapeRight), so the absolute value selects the same physical
	// axis either way. Portrait maps a different axis, but the game is landscape-only.
	const float YawWeight = FMath::Clamp(FMath::Abs(GravityUnit.Z), 0.0f, 1.0f);
	OutResult.YawWeight = YawWeight;

	// Measured polarity: a swing-LEFT gesture yields a POSITIVE yaw rate (mean +0.61 across the
	// gesture, against -1.16 for swing-right), while leaning left needs a NEGATIVE sideways angle.
	// So the base mapping is negative; YawSign flips it if another device or grip disagrees.
	const float MeasuredYawPolarity = -1.0f;

	return GravityRollDeg
		+ YawWeight * Tuning.YawGain * Tuning.YawSign * MeasuredYawPolarity * Yaw.AngleDeg;
}

FResult ComputeWeight(
	const FBasis& Basis,
	const FTuning& Tuning,
	FYawState& InOutYaw,
	const FVector& Gravity,
	const FVector& RotationRate,
	float DeltaTime)
{
	FResult Result;
	if (Gravity.IsNearlyZero() || !Basis.IsUsable())
	{
		return Result;
	}

	// Project the live (normalized) gravity onto the device-frame axes derived at calibration. At
	// neutral both components are ~0; tipping the phone rotates gravity into the relevant axis.
	//   PitchSin > 0 → phone tipped forward (top of screen away from player)
	//   RollSin  > 0 → phone tipped right (right edge of screen toward floor)
	// asin gives the rotation angle directly (valid for the moderate tilts players actually
	// produce; clamps gracefully at ±90°).
	const FVector G = Gravity.GetSafeNormal();
	const float PitchSin = FMath::Clamp(FVector::DotProduct(G, Basis.ForwardAxis), -1.0f, 1.0f);
	const float RollSin  = FMath::Clamp(FVector::DotProduct(G, Basis.RightAxis),   -1.0f, 1.0f);

	Result.PitchDeg       = FMath::RadiansToDegrees(FMath::Asin(PitchSin));
	Result.GravityRollDeg = FMath::RadiansToDegrees(FMath::Asin(RollSin));
	Result.RollDeg = AdvanceYawFusion(G, RotationRate, Result.GravityRollDeg, Tuning, InOutYaw, DeltaTime, Result);

	Result.Offset.Y = AxisOffset(Result.PitchDeg, Tuning.PitchDegreesForFull, Tuning.DeadzoneDegrees, Tuning.bInvertPitch);
	Result.Offset.X = AxisOffset(Result.RollDeg,  Tuning.RollDegreesForFull,  Tuning.DeadzoneDegrees, Tuning.bInvertRoll);
	return Result;
}

float AxisOffset(float DeltaDeg, float FullDeg, float DeadzoneDeg, bool bInvert)
{
	// Soft deadzone: within DZ → 0; past DZ → scaled by (angle-DZ)/(Full-DZ).
	if (bInvert) DeltaDeg = -DeltaDeg;
	const float Abs = FMath::Abs(DeltaDeg);
	if (Abs <= DeadzoneDeg)
	{
		return 0.0f;
	}
	const float Range = FMath::Max(FullDeg - DeadzoneDeg, KINDA_SMALL_NUMBER);
	const float Scaled = (Abs - DeadzoneDeg) / Range;
	return FMath::Clamp(Scaled, 0.0f, 1.0f) * FMath::Sign(DeltaDeg);
}

float ComputePump(
	const FVector& Gravity,
	const FVector& Acceleration,
	const FPumpTuning& Tuning,
	float& InOutLowPass,
	float DeltaTime)
{
	const FVector gUnit = Gravity.GetSafeNormal();
	if (gUnit.IsNearlyZero())
	{
		return 0.0f;
	}

	// Project player-induced acceleration onto live gravity. Positive when the player pushes the
	// phone in the gravity direction = loading the rider's weight = pump signal. Negative during
	// the extension/recovery phase.
	const float A_alongGravity = FVector::DotProduct(Acceleration, gUnit);

	// Low-pass filter. The accelerometer is noisy at 30-50 Hz raw; cutoff at LowPassHz (default
	// 10 Hz) smooths to the human-perceptible range.
	const float dt    = FMath::Max(DeltaTime, KINDA_SMALL_NUMBER);
	const float alpha = FMath::Clamp(dt * Tuning.LowPassHz, 0.0f, 1.0f);
	InOutLowPass = FMath::Lerp(InOutLowPass, A_alongGravity, alpha);

	// Loading-only: only the compress phase pumps; extension is recovery and must not subtract from
	// accumulated pump force. Then deadzone-and-scale.
	const float loadOnly = FMath::Max(0.0f, InOutLowPass - Tuning.Deadzone);
	return FMath::Clamp(loadOnly / FMath::Max(Tuning.AccelForFull, KINDA_SMALL_NUMBER), 0.0f, 1.0f);
}

} // namespace SurfTilt
