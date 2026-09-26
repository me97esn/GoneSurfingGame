// Fill out your copyright notice in the Description page of Project Settings.

#define _USE_MATH_DEFINES
#include "FluidDynamics.h"
#include "SurfLog.h"
#include <math.h>
#include <cmath>
#include "Kismet/GameplayStatics.h"
#include "DrawDebugHelpers.h"
#include "Components/PrimitiveComponent.h"
#include "Kismet/KismetMathLibrary.h"
#include "SurfDebug.h"
#include "SurfRails.h"
#include "SurfTuningSubsystem.h"
#include "WeightDistribution.h"

// Sets default values

AFluidDynamics::AFluidDynamics()
{
	// Set this actor to call Tick() every frame.  You can turn this off to improve performance if you don't need it.
	PrimaryActorTick.bCanEverTick = true;
}

// Called when the game starts or when spawned
void AFluidDynamics::RefreshFromTuningSubsystem()
{
	// Copy shared-by-design coefficients from the subsystem onto this actor's
	// scratch UPROPERTYs each tick. The level-designer-set per-actor variants
	// (lateralTurnMultiplier, side, debug flags) are intentionally NOT touched
	// here — those still come from the actor's BP-instance values. See
	// specs/runtime-tuning.md and project_lateral_turn_back_counter_carve memory.
	if (!Tuning)
	{
		return;
	}
	thrustMagnitude                 = Tuning->thrustMagnitude;
	lateralTurnCoefficient          = Tuning->lateralTurnCoefficient;
	lateralTurnSpeedCap             = Tuning->lateralTurnSpeedCap;
	lateralTurnRollDeadzone         = Tuning->lateralTurnRollDeadzone;
	lateralTurnHardCarveBoost       = Tuning->lateralTurnHardCarveBoost;
	lateralTurnHardCarveStart       = Tuning->lateralTurnHardCarveStart;
	lateralTurnHardCarveFull        = Tuning->lateralTurnHardCarveFull;
	forwardsThrustCoefficient       = Tuning->forwardsThrustCoefficient;
	actorForwardsThrustCoefficient  = Tuning->actorForwardsThrustCoefficient;
	upwardsThrustPitchSensitivity   = Tuning->upwardsThrustPitchSensitivity;
	yawHydrofoilCoefficient         = Tuning->yawHydrofoilCoefficient;
	yawThrustAttenStart             = Tuning->yawThrustAttenStart;
	yawThrustAttenEnd               = Tuning->yawThrustAttenEnd;
	bottomForwardDragMultiplier     = Tuning->bottomForwardDragMultiplier;
	forwardDragWettingGate          = Tuning->forwardDragWettingGate;
	airborneForceScale              = Tuning->airborneForceScale;
	airborneFadeDistance            = Tuning->airborneFadeDistance;
	yawFwdSlopeGateMin              = Tuning->yawFwdSlopeGateMin;
	yawFwdSlopeGateWidth            = Tuning->yawFwdSlopeGateWidth;
	yawFwdOffFaceFloor              = Tuning->yawFwdOffFaceFloor;
	yawFwdWithWaveGateOff           = Tuning->yawFwdWithWaveGateOff;
	yawFwdWithWaveGateOn            = Tuning->yawFwdWithWaveGateOn;
	yawFwdWithWaveGateSpeedMax      = Tuning->yawFwdWithWaveGateSpeedMax;
	brokenDriveCut                  = Tuning->BrokenDriveCut;
	antiSlipForceScale              = Tuning->AntiSlipForceScale;
	waveSlopeGravityCoefficient     = Tuning->waveSlopeGravityCoefficient;
	maxSupplementForce              = Tuning->maxSupplementForce;
	waveMassThrustCoefficient       = Tuning->waveMassThrustCoefficient;
	waveMassFlowDragCoefficient     = Tuning->waveMassFlowDragCoefficient;
	waveMassMinSlopeSin             = Tuning->waveMassMinSlopeSin;
	waveMassRollDecouple            = Tuning->waveMassRollDecouple;
	waveMassFlowYawDecouple         = Tuning->waveMassFlowYawDecouple;
	wavePenetrationYawDecouple      = Tuning->wavePenetrationYawDecouple;
	waveMassSmoothingTau            = Tuning->waveMassSmoothingTau;
	perActorWaterVelocityBlend      = Tuning->PerActorWaterVelocityBlend;
	lipImpactCoefficient            = Tuning->LipImpactCoefficient;
	lipImpactBandOffset             = Tuning->LipImpactBandOffset;
	lipImpactMinJetSpeed            = Tuning->LipImpactMinJetSpeed;
	lipImpactMinSlopeSin            = Tuning->LipImpactMinSlopeSin;
	lipImpactCrestRange             = Tuning->LipImpactCrestRange;
	lipImpactMaxForce               = Tuning->LipImpactMaxForce;
	railLiftRollDecouple            = Tuning->railLiftRollDecouple;
	wavePenetrationCoefficient      = Tuning->wavePenetrationCoefficient;
	wavePenetrationThreshold        = Tuning->wavePenetrationThreshold;
	slopeThrustCoefficient          = Tuning->slopeThrustCoefficient;
	slopeThrustMinSlopeSin          = Tuning->slopeThrustMinSlopeSin;
	slopeThrustFinRedirect          = Tuning->slopeThrustFinRedirect;
	slopeThrustFrontFaceDir         = Tuning->slopeThrustFrontFaceDir;
	baseHeight                      = Tuning->baseHeight;
	wettedTransitionDistance        = Tuning->wettedTransitionDistance;
	wettedForceCompensation         = Tuning->wettedForceCompensation;
	slopeHeight                     = Tuning->slopeHeight;
	maxEffectiveWaterHeight         = Tuning->maxEffectiveWaterHeight;
	maxHydrofoilForceAmount         = Tuning->maxHydrofoilForceAmount;
}

void AFluidDynamics::BeginPlay()
{
	Super::BeginPlay();
	// waveMassFlowDragCoefficient and waveMassThrustCoefficient are intentionally not overridden
	// here — their header defaults let per-actor editor tuning take effect.

	Tuning = SurfTuning::Get(this);
	RefreshFromTuningSubsystem();

	// One-time diagnostic: log this actor's startup world position so the per-tick torque
	// budget can be cross-referenced against where the actor physically sits on the board.
	// Useful for resolving questions about whether labeled "front" actors are actually
	// forward of CoM. Dev-only diagnostics; stripped from Shipping builds.
#if !UE_BUILD_SHIPPING
#if WITH_EDITOR
	const FVector loc = GetActorLocation();
	UE_LOG(LogSurf, Warning, TEXT("ActorStartPos [%s] worldPos=(%.2f, %.2f, %.2f)"),
		*GetActorLabel(), loc.X, loc.Y, loc.Z);
	const FString lbl = GetActorLabel();
#else
	const FString lbl = GetName();
#endif
	// One-shot dump of tunable defaults; see specs/runtime-tuning.md. Per-actor
	// because some fields (lateralTurnMultiplier, waveSlopeGravityCoefficient,
	// side, etc.) intentionally vary per actor in the level.
	UE_LOG(LogSurf, Warning, TEXT("TuningDefaults: FluidDynamics [%s] side=%d thrustMagnitude=%g lateralTurnCoefficient=%g lateralTurnMultiplier=%g forwardsThrustCoefficient=%g actorForwardsThrustCoefficient=%g upwardsThrustPitchSensitivity=%g yawHydrofoilCoefficient=%g"),
		*lbl, (int)side, thrustMagnitude, lateralTurnCoefficient, lateralTurnMultiplier, forwardsThrustCoefficient, actorForwardsThrustCoefficient, upwardsThrustPitchSensitivity, yawHydrofoilCoefficient);
	UE_LOG(LogSurf, Warning, TEXT("TuningDefaults: FluidDynamics [%s] waveSlopeGravityCoefficient=%g maxSupplementForce=%g waveMassThrustCoefficient=%g waveMassFlowDragCoefficient=%g baseHeight=%g slopeHeight=%g maxEffectiveWaterHeight=%g maxHydrofoilForceAmount=%g"),
		*lbl, waveSlopeGravityCoefficient, maxSupplementForce, waveMassThrustCoefficient, waveMassFlowDragCoefficient, baseHeight, slopeHeight, maxEffectiveWaterHeight, maxHydrofoilForceAmount);
#endif // !UE_BUILD_SHIPPING
}

double AFluidDynamics::unwindRadians(double angle)
{
	if (angle > M_PI)
	{
		angle -= 2 * M_PI;
	}
	else if (angle < -M_PI)
	{
		angle += 2 * M_PI;
	}
	return angle;
}

bool AFluidDynamics::roughlyEqual(float a, float b)
{
	return fabs(a - b) < 0.001;
}

float AFluidDynamics::calcCosAngleOfAttack(FVector _relativeWaterVelocity, FVector normal, FVector compareTo){

	FVector relativeWaterDirection = _relativeWaterVelocity.GetSafeNormal();
	// VectorPlaneProject assumes a unit normal (it uses ProjectOnToNormal internally,
	// which does NOT divide by |N|^2). The SC/FD basis vectors carry the actor's transform
	// scale, so they must be normalized before being passed as plane normals -- otherwise
	// the parallel component is only partially removed, scaled by |N|^2.
	FVector waterVelocityUpDown2dDirection= FVector::VectorPlaneProject(relativeWaterDirection, normal.GetSafeNormal());

	waterVelocityUpDown2dDirection.Normalize();


	// Dot product of two normalized vectors is the cos of the angle between them
	return compareTo.GetSafeNormal() | waterVelocityUpDown2dDirection;

}

FVector AFluidDynamics::calcDragForce()
{
	// Coefficients live solely in the tuning subsystem (single source of truth).
	// Falls back to the subsystem CDO — i.e. the header defaults — if no game
	// instance is up (e.g. editor-time calls).
	const USurfTuningSubsystem* T = Tuning ? Tuning : GetDefault<USurfTuningSubsystem>();
	const float bottomDragCoefficient = T->bottomDragCoefficient;
	const float maxDragAmount         = T->maxDragAmount;
	const float finDragCoefficient    = T->finDragCoefficient;
	const float railDragCoefficient   = T->railDragCoefficient;
	const float tailDragCoefficient   = T->tailDragCoefficient;
		const bool debugDragLog = (this->debugDrag || SurfDebug::ShouldDebug(this, TEXT("drag"))) && shouldDebugLog();
		FVector dragForce;
		// Wave-mass flow drag accumulates here (set in the bottom/rail branches) and is applied separately
		// at the CoM longitudinal axis by applyDragAsImpulse — option B, kills its roll torque while
		// keeping the front/back pitch. See specs/wave-mass-drag-torque-decoupling.md.
		this->PendingWaveMassForce = FVector::ZeroVector;
		this->PendingWavePenetrationForce = FVector::ZeroVector;

		switch (this->side)
		{
		case ESide::VE_Down:
		{
			// Bottom: split into two independent terms.
			//   (A) Forwards/backwards drag — skin friction. Pitch-gated, planing-attenuated.
			//   (B) Sideways drag — wall-pushback. No gates: the wetted bottom still feels
			//       perpendicular flow when the board is planing.
			const FVector boardForwards = this->sharedCalculations->forwards;
			const FVector boardUp       = this->sharedCalculations->up;
			const FVector relWaterVel   = this->sharedCalculations->relativeWaterVelocity;

			// (A) Forwards/backwards drag — existing formula.
			FVector forwardDrag = FVector::ZeroVector;
			if (this->sharedCalculations->pitchSinAngleOfAttack >= 0)
			{
				const float forwardComponent = FVector::DotProduct(relWaterVel, boardForwards);
				const FVector relVelAlongBoard = boardForwards * forwardComponent;
				const float relVelAlongMagnitude = relVelAlongBoard.Size();

				// Planing attenuation is LINEAR (not squared) — see specs/planing-drag-attenuation.md.
				// The multiplier was a hardcoded 10.0f (comment claimed 5×); it is now a tunable so the
				// biggest brake in the game can be A/B'd without a recompile. Default 10 = unchanged.
				//
				// WETTING: effectiveWaterHeight deliberately keeps a non-zero baseline when this actor is
				// NOT submerged, so on its own it let a v² drag keep braking a board that had left the
				// water — measured at 37% of the hardest braking ticks, mean 1074 cm/s. Scale by the
				// actor's real wetting instead. forwardDragWettingGate = 0 restores the old behaviour.
				const float dragWetting = FMath::Lerp(
					1.0f, this->actorWetted, FMath::Clamp(this->forwardDragWettingGate, 0.0f, 1.0f));
				const float forwardDragAmount = this->bottomForwardDragMultiplier * bottomDragCoefficient *
									   this->effectiveWaterHeight *
									   dragWetting *
									   this->sharedCalculations->pitchSinAngleOfAttack *
									   (1.0f - this->sharedCalculations->AmountPlaning) *
									   relVelAlongBoard.SizeSquared();
				if (debugDragLog)
				{
					UE_LOG(LogSurf, Warning, TEXT("=== DRAG DEBUG %s ==="), *this->GetName());
					UE_LOG(LogSurf, Warning, TEXT("  bottomDragCoef: %.2f, effectiveH: %.2f (col=%.2f slopeSin=%.2f), pitchSin: %.3f"), bottomDragCoefficient, this->effectiveWaterHeight, this->waterColumnAbove, this->slopeSin, this->sharedCalculations->pitchSinAngleOfAttack);
					UE_LOG(LogSurf, Warning, TEXT("  AmountPlaning: %.3f, (1-planing): %.3f"), this->sharedCalculations->AmountPlaning, (1 - this->sharedCalculations->AmountPlaning));
					UE_LOG(LogSurf, Warning, TEXT("  relativeWaterVelMag: %.2f, relVelAlongMag: %.2f"), this->sharedCalculations->relativeWaterVelocityMagnitude, relVelAlongMagnitude);
					UE_LOG(LogSurf, Warning, TEXT("  dragAmount: %.2f, maxDragAmount: %.2f"), forwardDragAmount, maxDragAmount);
				}
				const FVector dragDirection = relVelAlongBoard.GetSafeNormal();
				forwardDrag = dragDirection * FMath::Clamp(forwardDragAmount, 0.0f, maxDragAmount);
				if (debugDragLog)
				{
					UE_LOG(LogSurf, Warning, TEXT("  forwardDrag: (%.2f, %.2f, %.2f) magnitude: %.2f"),
						forwardDrag.X, forwardDrag.Y, forwardDrag.Z, forwardDrag.Length());
				}
			}

			// Wave's actual water velocity at this actor's SC position — used by sideways drag
			// (perpendicular component) and by the wave-mass flow drag (direction). The previous
			// per-actor waveMassDrag (forward-only, scale-leaked) was removed in favor of the
			// board-wide waveMassFlowDrag below. See specs/wave-mass-flow-drag.md.
			const FVector absWaterVel = this->sharedCalculations->absoluteWaterVelocity;

			// absSidewaysVel is the wave's lateral water flow component (perpendicular to board.forwards),
			// kept here because waveMassThrust below uses absSidewaysVel.SizeSquared() as its energy input.
			// The previous per-actor "sidewaysDrag" was removed — see specs/redundant-sideways-drag-removal.md.
			// Wave-mass momentum perpendicular to the board is now handled by waveMassFlowDrag (board-wide).
			const FVector absVelInBottomPlane = FVector::VectorPlaneProject(absWaterVel, boardUp.GetSafeNormal());
			const FVector absSidewaysVel     = FVector::VectorPlaneProject(absVelInBottomPlane, boardForwards.GetSafeNormal());

			// Wave-mass thrust — sideways wave flow converts to forward propulsion on a wave face.
			// Symmetric partner to the wave-mass drag above; reads the same absolute-velocity
			// sideways magnitude (absSidewaysVel²) but force direction is +board.forwards
			// instead of along the sideways flow.
			// IMPORTANT: this term uses *board-wide* slopeSin and waterColumnAbove (from SC),
			// not the per-actor values. The wave-mass thrust represents a board-wide phenomenon
			// (one body of water pushing the whole board); per-actor sampling of slopeSin would
			// inject discretization noise that becomes a spurious yaw torque, since the resulting
			// per-actor magnitudes would differ between symmetric L/R pairs. See
			// specs/per-actor-vs-board-wide-sampling.md for the principle, and
			// specs/wave-mass-thrust.md for this specific term.
			FVector waveMassThrust = FVector::ZeroVector;
			const float boardWideSlopeSin = this->sharedCalculations->boardWideSlopeSin;
			// Steep-face gate shared by every wave-mass force at this actor: no wave-mass push on
			// gentle faces, smooth ramp-in above the threshold. See specs/steep-face-takeoff.md.
			const float waveMassSlopeGate = FMath::SmoothStep(
				this->waveMassMinSlopeSin, this->waveMassMinSlopeSin + 0.06f, boardWideSlopeSin);
			if (this->waveMassThrustCoefficient > 0.0f && waveMassSlopeGate > 0.0f)
			{
				// Contact-gated like effectiveWaterHeight. This height is recomputed locally from the
				// BOARD-WIDE column rather than read off effectiveWaterHeight, so it does not inherit
				// that gate and has to take it explicitly -- the leak that let the wave-mass family
				// keep pushing a board clear of the water. Per-actor on purpose: the gate asks
				// whether THIS surface is in the water, so an airborne rail stops contributing while
				// a submerged one still does. airborneForceScale = 1 -> gate 1 -> unchanged.
				// See specs/airborne-force-gating.md.
				const float boardWideEffectiveH = this->waterContactGate * FMath::Min(
					this->baseHeight
						+ this->sharedCalculations->boardWideWaterColumnAbove
						+ boardWideSlopeSin * this->slopeHeight,
					this->maxEffectiveWaterHeight);
				const float thrustAmount = this->waveMassThrustCoefficient
				                         * boardWideEffectiveH
				                         * boardWideSlopeSin
				                         * waveMassSlopeGate
				                         * absSidewaysVel.SizeSquared();
				waveMassThrust = boardForwards.GetSafeNormal()
				               * FMath::Clamp(thrustAmount, 0.0f, maxDragAmount);
				// Propulsion budget (FR6). Report the RAW demand, apply the compressed force.
				// Reporting the already-scaled value would feed the governor its own output: the
				// total would land under the knee, the scale would relax to 1, and it would ring.
				this->sharedCalculations->RegisterForwardDrive(
					FVector::DotProduct(waveMassThrust, boardForwards.GetSafeNormal()));
				waveMassThrust *= this->sharedCalculations->GetPropulsionScale();
				if (debugDragLog)
				{
					UE_LOG(LogSurf, Warning, TEXT("  waveMassThrust (bottom, abs): coef=%.4f, boardWideSlopeSin=%.3f, boardWideEffH=%.2f, absSidewaysVel²=%.1f -> amount=%.2f, thrust=(%.2f, %.2f, %.2f) mag=%.2f"),
						this->waveMassThrustCoefficient, boardWideSlopeSin, boardWideEffectiveH, absSidewaysVel.SizeSquared(),
						thrustAmount, waveMassThrust.X, waveMassThrust.Y, waveMassThrust.Z, waveMassThrust.Length());
				}
			}

			// Wave-mass flow drag — pushes the board in the wave's actual flow direction.
			// Replaces waveMassDrag (forward, scale-leaked) and waveMassSidewaysDrag
			// (perpendicular, board-frame). Uses board-wide averaged absoluteWaterVelocity to
			// eliminate front/back per-actor sampling asymmetry. See specs/wave-mass-flow-drag.md.
			FVector waveMassFlowDrag = FVector::ZeroVector;
			if (this->waveMassFlowDragCoefficient > 0.0f && waveMassSlopeGate > 0.0f)
			{
				const FVector boardWideAbsWaterVel = (this->sharedCalculations
					&& this->sharedCalculations->otherHalfSharedCalculations)
					? 0.5f * (this->sharedCalculations->absoluteWaterVelocity
					          + this->sharedCalculations->otherHalfSharedCalculations->absoluteWaterVelocity)
					: this->sharedCalculations->absoluteWaterVelocity;
				// Per-actor blend: contact forces can read the water velocity at THIS actor instead
				// of the board-wide average, so the nose feels the lip's core (v² on a 2-3x faster
				// sample). 0 = legacy. See specs/per-actor-water-velocity.md.
				const FVector flowWaterVel = FMath::Lerp(boardWideAbsWaterVel,
					this->actorWaterVelocity, this->perActorWaterVelocityBlend);

				const float boardWideAbsWaterVelSq = flowWaterVel.SizeSquared();
				if (boardWideAbsWaterVelSq > 0.01f)
				{
					// Contact-gated -- see the note at the wave-mass thrust above; this height is
					// recomputed locally and does not inherit effectiveWaterHeight's gate.
					const float boardWideEffectiveH = this->waterContactGate * FMath::Min(
						this->baseHeight
							+ this->sharedCalculations->boardWideWaterColumnAbove
							+ boardWideSlopeSin * this->slopeHeight,
						this->maxEffectiveWaterHeight);
					const float flowDragAmount = this->waveMassFlowDragCoefficient
					                           * boardWideEffectiveH
					                           * boardWideSlopeSin
					                           * waveMassSlopeGate
					                           * boardWideAbsWaterVelSq;
					waveMassFlowDrag = flowWaterVel.GetSafeNormal()
					                 * FMath::Clamp(flowDragAmount, 0.0f, maxDragAmount);
					if (debugDragLog)
					{
						UE_LOG(LogSurf, Warning, TEXT("  waveMassFlowDrag (bottom, abs): coef=%.4f, boardWideSlopeSin=%.3f, slopeGate=%.3f, boardWideEffH=%.2f, |bwAbsWaterVel|²=%.1f -> amount=%.2f, drag=(%.2f, %.2f, %.2f) mag=%.2f"),
							this->waveMassFlowDragCoefficient, boardWideSlopeSin, waveMassSlopeGate, boardWideEffectiveH, boardWideAbsWaterVelSq,
							flowDragAmount, waveMassFlowDrag.X, waveMassFlowDrag.Y, waveMassFlowDrag.Z, waveMassFlowDrag.Length());
					}
				}
			}

			// Wave penetration resistance — a "wall" drag along the wave's HORIZONTAL normal that resists
			// the board crossing the face (in OR out), ungated by planing. Tangential (down-the-line)
			// motion has ~zero normal component → unaffected (NFR1); a forward charge into/over the crest
			// has a large normal component → resisted, so the board stays ON the face instead of launching
			// through to the back. Per-actor (front/back SC) so it also damps yaw/skid via local velocity.
			// See specs/wave-penetration-resistance.md.
			FVector wavePenetrationDrag = FVector::ZeroVector;
			if (this->wavePenetrationCoefficient > 0.0f && boardWideSlopeSin > 0.0f)
			{
				const FVector waveNormalHoriz = FVector(this->sharedCalculations->waveNormal.X,
				                                        this->sharedCalculations->waveNormal.Y, 0.0f).GetSafeNormal();
				if (!waveNormalHoriz.IsNearlyZero())
				{
					// Board velocity relative to water = -relWaterVel (relWaterVel = water - board). Its
					// component along the horizontal wave normal is the rate the board crosses the face.
					// Per-actor blend: local closing speed at THIS actor (the nose meeting the lip's core
					// reads a much higher closing speed than the board-midline SC sample). 0 = legacy.
					// See specs/per-actor-water-velocity.md.
					const FVector effRelWaterVel = FMath::Lerp(relWaterVel,
						this->actorWaterVelocity - this->sharedCalculations->componentVelocity,
						this->perActorWaterVelocityBlend);
					const float vAcrossFace = FVector::DotProduct(-effRelWaterVel, waveNormalHoriz);
					// Deadzone: only resist cross-face speed ABOVE a threshold. Normal riding/pumping has a
					// small, ever-present cross-face component (incl. the wave's own normal water flow) that
					// must NOT be braked (NFR1) — a single v² coefficient strong enough to stop a fast
					// punch-through also brakes the whole ride. Subtracting the threshold lets the coefficient
					// be strong where it matters (fast charge) while leaving steady-state riding untouched.
					const float vExcess = FMath::Max(0.0f, FMath::Abs(vAcrossFace) - this->wavePenetrationThreshold);
					const float wallEffH = FMath::Min(this->effectiveWaterHeight, this->maxEffectiveWaterHeight);
					const float wallAmount = this->wavePenetrationCoefficient * wallEffH * vExcess * vExcess;
					// Oppose the crossing direction, whichever way the board is crossing.
					wavePenetrationDrag = (-FMath::Sign(vAcrossFace)) * waveNormalHoriz
					                    * FMath::Clamp(wallAmount, 0.0f, maxDragAmount);
					if (debugDragLog)
					{
						UE_LOG(LogSurf, Warning, TEXT("  wavePenetrationDrag: coef=%.5f, thresh=%.1f, wallEffH=%.2f, waveNormalHoriz=(%.3f,%.3f,%.3f), vAcrossFace=%.2f, vExcess=%.2f -> amount=%.2f, drag=(%.2f, %.2f, %.2f) mag=%.2f"),
							this->wavePenetrationCoefficient, this->wavePenetrationThreshold, wallEffH, waveNormalHoriz.X, waveNormalHoriz.Y, waveNormalHoriz.Z,
							vAcrossFace, vExcess, wallAmount, wavePenetrationDrag.X, wavePenetrationDrag.Y, wavePenetrationDrag.Z, wavePenetrationDrag.Length());
					}
				}
			}

			// waveMassFlowDrag is the roll-producing term (its direction is off-axis). Relocate it to the
			// CoM longitudinal axis (option B) to kill roll while keeping the front/back pitch; the rest
			// (forwardDrag/waveMassThrust along board.forwards → 0 roll; wavePenetrationDrag per-actor
			// yaw/skid damping) stays at the actor. See specs/wave-mass-drag-torque-decoupling.md.
			this->PendingWaveMassForce += waveMassFlowDrag;
			// wavePenetration goes through the same option-C decouple path (its own knob) so its
			// into-the-wave yaw can be stripped while the linear "wall" (glide-through stop) survives.
			this->PendingWavePenetrationForce += wavePenetrationDrag;
			dragForce = forwardDrag + waveMassThrust;

			if (debugDragLog)
			{
#if WITH_EDITOR
				const FString LabelForLog = GetActorLabel();
#else
				const FString LabelForLog = TEXT("<no-label>");
#endif
				UE_LOG(LogSurf, Warning, TEXT("BOTTOMDRAG [%s] slopeSin=%.3f loc=(%.0f,%.0f,%.0f) total=(%.0f,%.0f,%.0f)|%.0f fwd=%.0f thrust=%.0f flow=%.0f penet=%.0f"),
					*LabelForLog, this->slopeSin, GetActorLocation().X, GetActorLocation().Y, GetActorLocation().Z,
					dragForce.X, dragForce.Y, dragForce.Z, dragForce.Size(),
					forwardDrag.Size(), waveMassThrust.Size(), waveMassFlowDrag.Size(), wavePenetrationDrag.Size());
			}

			// Register dragBottom sub-components separately so the torque budget can
			// distinguish per-actor-varying forwardDrag from board-wide waveMassThrust /
			// waveMassFlowDrag (which by symmetry shouldn't produce yaw torque from L/R
			// pairs but might from board-frame rotation when rolled).
			if (this->sharedCalculations)
			{
				const FVector actorPos = GetActorLocation();
				this->sharedCalculations->RegisterAppliedForce(TEXT("dragBottom_forwardDrag"),     actorPos, forwardDrag);
				this->sharedCalculations->RegisterAppliedForce(TEXT("dragBottom_waveMassThrust"), actorPos, waveMassThrust);
				this->sharedCalculations->RegisterAppliedForceWithTorque(TEXT("dragBottom_waveMassFlow"), waveMassFlowDrag, getWaveMassKeptTorque(waveMassFlowDrag, this->waveMassFlowYawDecouple));
				this->sharedCalculations->RegisterAppliedForceWithTorque(TEXT("dragBottom_wavePenetration"), wavePenetrationDrag, getWaveMassKeptTorque(wavePenetrationDrag, this->wavePenetrationYawDecouple));
			}
			// waveMassThrust rides in the RETURNED dragForce (plain apply path), unlike its siblings
			// on the pending wave-mass path — but it's the same wave-carry momentum, so exclude it
			// from spray. Measured 18k planar mid-turn, the largest counted driver before exclusion.
			this->appliedNonSprayForceThisTick += waveMassThrust;
			break;
		}
		case ESide::VE_Left:
		case ESide::VE_Right:
		{
			// Rail drag has two physical mechanisms:
			//   (A) Skin friction (planing-attenuated, relative velocity). Forward/backward
			//       viscous drag along the rail.
			//   (B) Wave-mass momentum push (no planing attenuation, absolute velocity).
			//       Scales with the wave's water mass × actual wave water flow squared.
			//       Direction follows the wave's actual flow, not the relative direction.
			// See specs/wave-mass-drag-absolute-direction.md for the principle.
			const FVector boardForwards = this->sharedCalculations->forwards.GetSafeNormal();
			const FVector boardLeft     = this->sharedCalculations->left.GetSafeNormal();
			const FVector boardUp       = this->sharedCalculations->up.GetSafeNormal();
			const FVector relWaterVel   = this->sharedCalculations->relativeWaterVelocity;
			const FVector absWaterVel   = this->sharedCalculations->absoluteWaterVelocity;
			const bool isLeftRail       = (this->side == ESide::VE_Left);
			const TCHAR* sideTag        = isLeftRail ? TEXT("LEFT") : TEXT("RIGHT");

			// (A) Forward skin friction — gated by the engaged-rail sign (only the rail facing
			// into the relative flow contributes; the other rail is presumed lifted out of the
			// water by board roll). Uses *relative* velocity.
			FVector forwardDrag = FVector::ZeroVector;
			const float cosYawLeft = this->sharedCalculations->cosYawAngleOfAttackLeft;
			const bool fwdEngaged = isLeftRail ? (cosYawLeft > 0) : (cosYawLeft < 0);
			float forwardDragAmount = 0.0f;
			float fwdPlaningAttenuation = 0.0f;
			if (fwdEngaged)
			{
				const float forwardComponent = FVector::DotProduct(relWaterVel, boardForwards);
				const FVector relVelAlongBoard = boardForwards * forwardComponent;
				// Planing attenuation is LINEAR (not squared) — see specs/planing-drag-attenuation.md.
				fwdPlaningAttenuation = 1.0f - this->sharedCalculations->AmountPlaning;
				// EXPERIMENTAL: 5× hardcoded multiplier while iterating on the right coefficient.
				const float kExperimentalRailDragMultiplier = 10.0f;
				forwardDragAmount = kExperimentalRailDragMultiplier * railDragCoefficient * this->effectiveWaterHeight * fwdPlaningAttenuation * relVelAlongBoard.SizeSquared();
				forwardDrag = relVelAlongBoard.GetSafeNormal() * FMath::Clamp(forwardDragAmount, 0.0f, maxDragAmount);

				// Wave-mass drag (forward-only, scale-leaked) was removed in favor of the board-wide
				// waveMassFlowDrag in the sidewaysEngaged block below. See specs/wave-mass-flow-drag.md.
			}

			// (B) Engagement gate + wave-mass flow drag — single-sided (one rail in water at a time).
			// The previous per-actor rail sideways drag was removed; wave-mass momentum into the
			// engaged rail is now handled exclusively by waveMassFlowDrag (board-wide direction).
			// See specs/redundant-sideways-drag-removal.md.
			FVector flowDrag = FVector::ZeroVector;
			const FVector absVelInBottomPlane = FVector::VectorPlaneProject(absWaterVel, boardUp);
			const FVector absSidewaysVel = FVector::VectorPlaneProject(absVelInBottomPlane, boardForwards);
			const float absSidewaysOnLeft = FVector::DotProduct(absSidewaysVel, boardLeft);
			const bool sidewaysEngaged = isLeftRail ? (absSidewaysOnLeft > 0) : (absSidewaysOnLeft < 0);
			float railFlowDragAmount = 0.0f;
			if (sidewaysEngaged)
			{
				// Rail wave-mass thrust intentionally skipped: single-sided engagement (only one
				// rail in the water at a time) means applying thrust at the engaged-rail position
				// creates a yaw torque ~20× bigger than the symmetric bottom thrust. The bottom
				// already provides plenty of forward propulsion via its symmetric left/right pairs.
				// See specs/wave-mass-thrust.md.

				// Wave-mass flow drag — pushes the engaged rail in the wave's actual flow direction.
				// Uses board-wide averaged absoluteWaterVelocity. See specs/wave-mass-flow-drag.md.
				const float boardWideSlopeSin = this->sharedCalculations->boardWideSlopeSin;
				// Steep-face gate: no wave-mass push on gentle faces. See specs/steep-face-takeoff.md.
				const float waveMassSlopeGate = FMath::SmoothStep(
					this->waveMassMinSlopeSin, this->waveMassMinSlopeSin + 0.06f, boardWideSlopeSin);
				if (this->waveMassFlowDragCoefficient > 0.0f && waveMassSlopeGate > 0.0f)
				{
					const FVector boardWideAbsWaterVel = (this->sharedCalculations
						&& this->sharedCalculations->otherHalfSharedCalculations)
						? 0.5f * (this->sharedCalculations->absoluteWaterVelocity
						          + this->sharedCalculations->otherHalfSharedCalculations->absoluteWaterVelocity)
						: this->sharedCalculations->absoluteWaterVelocity;

					// Per-actor blend (see specs/per-actor-water-velocity.md); 0 = legacy board-wide.
					const FVector railFlowWaterVel = FMath::Lerp(boardWideAbsWaterVel,
						this->actorWaterVelocity, this->perActorWaterVelocityBlend);
					const float boardWideAbsWaterVelSq = railFlowWaterVel.SizeSquared();
					if (boardWideAbsWaterVelSq > 0.01f)
					{
						// Contact-gated -- see the note at the wave-mass thrust above.
						const float boardWideEffectiveH = this->waterContactGate * FMath::Min(
							this->baseHeight
								+ this->sharedCalculations->boardWideWaterColumnAbove
								+ boardWideSlopeSin * this->slopeHeight,
							this->maxEffectiveWaterHeight);
						railFlowDragAmount = this->waveMassFlowDragCoefficient
						                   * boardWideEffectiveH
						                   * boardWideSlopeSin
						                   * waveMassSlopeGate
						                   * boardWideAbsWaterVelSq;
						flowDrag = railFlowWaterVel.GetSafeNormal()
						         * FMath::Clamp(railFlowDragAmount, 0.0f, maxDragAmount);
					}
				}
			}

			// flowDrag is single-sided (engaged rail only) → its off-axis force at the rail's lateral
			// offset is the dominant roll source. Relocate it to the CoM longitudinal axis (option B);
			// rail forwardDrag (skin friction along board.forwards → 0 roll) stays at the actor.
			// See specs/wave-mass-drag-torque-decoupling.md.
			this->PendingWaveMassForce += flowDrag;
			dragForce = forwardDrag;
			// Register the relocated rail flow drag at its CoM-axis application point so the torque budget
			// reflects the decoupling (roll should now read ~0). forwardDrag still registers as "dragRail".
			if (this->sharedCalculations && !flowDrag.IsNearlyZero())
			{
				this->sharedCalculations->RegisterAppliedForceWithTorque(TEXT("dragRailFlow"), flowDrag, getWaveMassKeptTorque(flowDrag, this->waveMassFlowYawDecouple));
			}
			if (debugDragLog)
			{
				UE_LOG(LogSurf, Warning, TEXT("=== RAIL DRAG (%s) %s === cosYawLeft: %.3f, planing: %.3f, effectiveH: %.2f (col=%.2f slopeSin=%.2f), fwdEngaged: %d fwdPlaningAtten: %.3f fwdAmount: %.2f forwardDrag mag: %.2f | absSidewaysVelMag: %.2f sidewaysEngaged: %d flowDragAmount: %.2f flowDrag: (%.2f, %.2f, %.2f) mag: %.2f"),
					sideTag, *this->GetName(),
					cosYawLeft, this->sharedCalculations->AmountPlaning, this->effectiveWaterHeight, this->waterColumnAbove, this->slopeSin,
					(int)fwdEngaged, fwdPlaningAttenuation, forwardDragAmount, forwardDrag.Length(),
					absSidewaysVel.Size(), (int)sidewaysEngaged, railFlowDragAmount,
					flowDrag.X, flowDrag.Y, flowDrag.Z, flowDrag.Length());
			}
			break;
		}
		case ESide::VE_Fin:
		{
			// Fins should have high drag when water flows perpendicular (sideways),
			// and very low drag when water flows parallel (forward/backward)
			// For fins: cosYawAngleOfAttack = surfboardLeft | relativeWaterVelocity
			//   cos = 1.0 when water flows LEFT (perpendicular to fins) -> maximum drag
			//   cos = 0.0 when water flows FORWARD/BACK (parallel to fins) -> minimal drag

			// Use cos directly as perpendicular factor
			float perpendicularFactor = FMath::Abs(this->cosYawAngleOfAttack);

			// cos⁴ falloff: parallel=0, perpendicular=1, but with a sharper drop. At cos=0.3 the
			// factor is ~0.008 (vs 0.09 with cos²), which lets finDragCoefficient be raised enough
			// to actually stop sideways slip without braking a straight run. Approximation of the
			// directional stability real fins provide via lift; if cos⁴ alone isn't enough, swap
			// fin drag for fin lift (force perpendicular to flow in the fin plane).
			const float p2 = perpendicularFactor * perpendicularFactor;
			float dragFactor = p2 * p2;

			// v² scaling so high-speed events (lip impact) hit much harder than steady carving.
			float dragAmount = finDragCoefficient * this->effectiveWaterHeight * dragFactor *
				this->sharedCalculations->relativeWaterVelocity.SizeSquared();

			dragForce = this->sharedCalculations->relativeWaterVelocityNormalized * FMath::Clamp(dragAmount, 0, maxDragAmount);
			if (debugDragLog)
			{
				UE_LOG(LogSurf, Warning, TEXT("=== FIN DRAG %s === perpFactor: %.3f, dragFactor: %.3f, effectiveH: %.2f (col=%.2f slopeSin=%.2f), relWaterVelMag: %.2f, dragAmount(uncapped): %.2f, dragForce: (%.2f, %.2f, %.2f) magnitude: %.2f"),
					*this->GetName(), perpendicularFactor, dragFactor, this->effectiveWaterHeight, this->waterColumnAbove, this->slopeSin,
					this->sharedCalculations->relativeWaterVelocityMagnitude, dragAmount,
					dragForce.X, dragForce.Y, dragForce.Z, dragForce.Length());
			}
			break;
		}
		case ESide::VE_Tail:
		{
			dragForce = FVector::ZeroVector;
			// Should only apply drag when the water moves towards the board from behind (same direction as forwards).
			float cosWaterForwards = this->sharedCalculations->relativeWaterVelocityDirectionProjectedUp | this->sharedCalculations->forwards;
			if (debugDragLog)
			{
				UE_LOG(LogSurf, Warning, TEXT("=== TAIL DRAG DEBUG %s ==="), *this->GetName());
				UE_LOG(LogSurf, Warning, TEXT("  cosWaterForwards: %.3f, effectiveH: %.2f (col=%.2f slopeSin=%.2f)"), cosWaterForwards, this->effectiveWaterHeight, this->waterColumnAbove, this->slopeSin);
				UE_LOG(LogSurf, Warning, TEXT("  relVelProjUp: (%.1f, %.1f, %.1f)"),
					this->sharedCalculations->relativeWaterVelocityDirectionProjectedUp.X,
					this->sharedCalculations->relativeWaterVelocityDirectionProjectedUp.Y,
					this->sharedCalculations->relativeWaterVelocityDirectionProjectedUp.Z);
				UE_LOG(LogSurf, Warning, TEXT("  forwards: (%.1f, %.1f, %.1f)"),
					this->sharedCalculations->forwards.X,
					this->sharedCalculations->forwards.Y,
					this->sharedCalculations->forwards.Z);
			}
			if (cosWaterForwards > 0)
			{
				// Skin friction tail drag: planing-attenuated, scales with relative velocity.
				// v² + planingAttenuation: when planing, the tail rides above the surface
				// for slow / parallel flow but stays submerged when water hits it head-on (high
				// cosWaterForwards). Same shape as the rail formula.
				const float planingAttenuation = 1.0f - this->sharedCalculations->AmountPlaning * (1.0f - cosWaterForwards);
				float dragAmount = cosWaterForwards * this->sharedCalculations->relativeWaterVelocity.SizeSquared() * tailDragCoefficient * 0.01 * planingAttenuation * this->effectiveWaterHeight;
				dragForce = this->sharedCalculations->forwards * FMath::Clamp(dragAmount, 0, maxDragAmount);
			}

			// Wave-mass tail drag — tail-specific variant of waveMassFlowDrag. Same coefficient
			// (waveMassFlowDragCoefficient), but the tail's force direction is constrained to
			// +board.forwards by the tail surface's geometry (perpendicular to board.forwards),
			// and the magnitude includes an extra cosAbsForwards gate (only fires when water is
			// flowing forward into the tail). See specs/wave-mass-flow-drag.md.
			// Steep-face gate on the tail's PER-ACTOR slopeSin (this site's existing sampling
			// semantics). See specs/steep-face-takeoff.md.
			const float waveMassSlopeGate = FMath::SmoothStep(
				this->waveMassMinSlopeSin, this->waveMassMinSlopeSin + 0.06f, this->slopeSin);
			if (this->waveMassFlowDragCoefficient > 0.0f && waveMassSlopeGate > 0.0f)
			{
				const FVector absWaterVel  = this->sharedCalculations->absoluteWaterVelocity;
				const FVector absWaterVelN = absWaterVel.GetSafeNormal();
				const float cosAbsWaterForwards = absWaterVelN | this->sharedCalculations->forwards;
				if (cosAbsWaterForwards > 0)
				{
					const float tailFlowDragAmount = this->waveMassFlowDragCoefficient
					                               * this->effectiveWaterHeight
					                               * this->slopeSin
					                               * waveMassSlopeGate
					                               * absWaterVel.SizeSquared()
					                               * cosAbsWaterForwards;
					dragForce += this->sharedCalculations->forwards
					           * FMath::Clamp(tailFlowDragAmount, 0.0f, maxDragAmount);
					if (debugDragLog)
					{
						UE_LOG(LogSurf, Warning, TEXT("  tail flowDragAmount(abs): %.2f, cosAbsFwd: %.3f, absVel²: %.1f, total tail dragForce: (%.2f, %.2f, %.2f) mag: %.2f"),
							tailFlowDragAmount, cosAbsWaterForwards, absWaterVel.SizeSquared(),
							dragForce.X, dragForce.Y, dragForce.Z, dragForce.Length());
					}
				}
			}
			break;
		}
		case ESide::VE_Nose:
		{
			// Synthesized lip impact: the airborne curl's momentum isn't in the column data at the
			// nose — it registers where the sheet LANDS, shoreward of the crest. When the nose is
			// under the curl (near-crest + steep breaking section) probe the landing band for the
			// jet's true velocity and synthesize F = coef × jetSpeed² along the jet's own direction
			// (shoreward+down). Stashed here; applied pitch-kept in applyDragAsImpulse.
			// See specs/lip-impact.md.
			this->PendingLipImpactForce = FVector::ZeroVector;
			if (SurfDebug::ShouldDebug(this, TEXT("lip")))
			{
				UE_LOG(LogSurf, Warning, TEXT("LIPGATE [%s] coef=%.3f sc=%d wave=%d distToCrest=%.0f slopeSin=%.3f backDir=(%.2f,%.2f)"),
					*GetName(), this->lipImpactCoefficient,
					this->sharedCalculations ? 1 : 0,
					(this->sharedCalculations && this->sharedCalculations->waveVelocity) ? 1 : 0,
					this->sharedCalculations ? this->sharedCalculations->signedDistanceToCrest : -99999.0f,
					this->sharedCalculations ? this->sharedCalculations->boardWideSlopeSin : -1.0f,
					this->sharedCalculations ? this->sharedCalculations->resolvedWaveBackDirection.X : 0.0,
					this->sharedCalculations ? this->sharedCalculations->resolvedWaveBackDirection.Y : 0.0);
			}
			if (this->lipImpactCoefficient > 0.0f && this->sharedCalculations
				&& this->sharedCalculations->waveVelocity)
			{
				const float distToCrest = this->sharedCalculations->signedDistanceToCrest;
				const float crestGate = 1.0f - FMath::Clamp(
					FMath::Abs(distToCrest) / FMath::Max(this->lipImpactCrestRange, 1.0f), 0.0f, 1.0f);
				const float slopeGate = FMath::SmoothStep(this->lipImpactMinSlopeSin,
					this->lipImpactMinSlopeSin + 0.08f, this->sharedCalculations->boardWideSlopeSin);
				const FVector backDir = this->sharedCalculations->resolvedWaveBackDirection;
				if (crestGate > 0.0f && slopeGate > 0.0f && !backDir.IsNearlyZero())
				{
					AWaveHeight* wave = this->sharedCalculations->waveVelocity;
					const FQuat waveRot = wave->GetActorQuat();
					const int32 frame = this->sharedCalculations->lastTileAdjustedFrame;
					const FVector nosePos = GetActorLocation();
					// Probe the landing band with a small FAN: {0.5, 1.0, 1.5}×offset shoreward ×
					// {-1, 0, +1}×offset along the crest. The wave PEELS — the landed/landing lip
					// water sits shoreward but often displaced ALONG the crest (up-line, where the
					// section already broke) relative to a nose pointing at the unbroken section, so
					// a straight-shoreward probe line misses it. Take the fastest sample — the
					// band's speed IS the evidence of a lip overhead.
					const FVector crestTangent = FVector(-backDir.Y, backDir.X, 0.0).GetSafeNormal();
					FVector jetVel = FVector::ZeroVector;
					float jetSpeedSq = 0.0f;
					for (int32 i = 0; i <= 3; ++i)
					{
						for (int32 j = -1; j <= 1; ++j)
						{
							const FVector p = nosePos
								- backDir * (this->lipImpactBandOffset * 0.5f * i)
								+ crestTangent * (this->lipImpactBandOffset * j);
							const FVector v = waveRot.RotateVector(wave->calculateWaveVelocity(p, frame));
							const float s = (float)v.SizeSquared();
							if (s > jetSpeedSq) { jetSpeedSq = s; jetVel = v; }
						}
					}
					const float jetSpeed = FMath::Sqrt(jetSpeedSq);
					const float speedGate = FMath::SmoothStep(this->lipImpactMinJetSpeed,
						this->lipImpactMinJetSpeed * 1.3f, jetSpeed);
					if (SurfDebug::ShouldDebug(this, TEXT("lip")))
					{
						UE_LOG(LogSurf, Warning, TEXT("LIPPROBE [%s] jetSpeed=%.0f jetVel=(%.0f,%.0f,%.0f) crestGate=%.2f slopeGate=%.2f speedGate=%.2f"),
							*GetName(), jetSpeed, jetVel.X, jetVel.Y, jetVel.Z, crestGate, slopeGate, speedGate);
					}
					if (speedGate > 0.0f)
					{
						// CONTACT GATE. Every gate above asks about the WAVE — how near the crest, how
						// steep the face, how fast the jet — and none asks whether the lip is touching
						// the board. So the moment after the lip throws the board clear, all three read
						// full (crest ~0 cm away, slopeSin 0.55, fast jet) and this keeps shoving the
						// airborne board along the jet direction at the cap. Measured as the DOMINANT
						// term in the force budget across a 40 cm-high window: ~200 kN, against which
						// every other family was already ~0. Water cannot slam a hull it is not
						// touching. See specs/airborne-force-gating.md.
						const float amount = FMath::Min(
							this->lipImpactCoefficient * jetSpeedSq * crestGate * slopeGate * speedGate
								* this->waterContactGate,
							this->lipImpactMaxForce);
						this->PendingLipImpactForce = jetVel.GetSafeNormal() * amount;
						if (SurfDebug::ShouldDebug(this, TEXT("lip")))
						{
							UE_LOG(LogSurf, Warning, TEXT("LIPIMPACT [%s] jetSpeed=%.0f gates(crest=%.2f slope=%.2f speed=%.2f) distToCrest=%.0f -> F=(%.0f, %.0f, %.0f) mag=%.0f"),
								*GetName(), jetSpeed, crestGate, slopeGate, speedGate, distToCrest,
								this->PendingLipImpactForce.X, this->PendingLipImpactForce.Y,
								this->PendingLipImpactForce.Z, this->PendingLipImpactForce.Size());
						}
					}
				}
			}
			dragForce = FVector::ZeroVector;
			break;
		}
		default:
		{
			dragForce = FVector::ZeroVector;
			break;
		}
		}

		// Debug visualization - draw for any non-zero drag force
		if ((this->debugDrawForces || SurfDebug::ShouldDebug(this, TEXT("forces"))) && !dragForce.IsNearlyZero())
		{
			// Different colors for different drag types
			FColor dragColor;
			switch (this->side)
			{
			case ESide::VE_Down:
				dragColor = FColor::Yellow;  // Bottom drag - Yellow
				break;
			case ESide::VE_Left:
			case ESide::VE_Right:
				dragColor = FColor::Orange;  // Rail drag - Orange
				break;
			case ESide::VE_Fin:
				dragColor = FColor::Cyan;    // Fin drag - Cyan
				break;
			case ESide::VE_Tail:
				dragColor = FColor::White; // Tail drag 
				break;
			default:
				dragColor = FColor::White;   // Default - White
				break;
			}

			FVector actorLocation = GetActorLocation();
			FVector arrowEnd = actorLocation + dragForce * debugForceDrawScale;
			DrawDebugDirectionalArrow(
				GetWorld(),
				actorLocation,
				arrowEnd,
				50.0f,
				dragColor,
				false,
				DebugDrawDurationTime,
				0,
				2.0f
			);
		}

		// Register drag (per-side) with SharedCalculations for the per-tick yaw torque budget log
		// (gated by SurfDebug "torque" flag, evaluated in SharedCalculations::calculateAll).
		if (this->sharedCalculations)
		{
			const TCHAR* dragCategory = TEXT("dragOther");
			switch (this->side)
			{
				case ESide::VE_Down:  dragCategory = TEXT("dragBottom"); break;
				case ESide::VE_Left:
				case ESide::VE_Right: dragCategory = TEXT("dragRail");   break;
				case ESide::VE_Fin:   dragCategory = TEXT("dragFin");    break;
				case ESide::VE_Tail:  dragCategory = TEXT("dragTail");   break;
				default: break;
			}
			this->sharedCalculations->RegisterAppliedForce(FString(dragCategory), GetActorLocation(), dragForce);
		}

		return dragForce;
}

float AFluidDynamics::calcRailLiftForceAmount(float liftMagnitude, float cosYawAngle)
{
	const bool debugLiftLog = (this->debugLift || SurfDebug::ShouldDebug(this, TEXT("lift"))) && shouldDebugLog();
	// Rail lift from two independent Bernoulli effects:
	// 1. Horizontal curvature (top-down view): water flowing front->back along curved rail
	// 2. Vertical curvature (front-on view): water flowing bottom->top along curved rail

	FVector relWaterVel = this->sharedCalculations->relativeWaterVelocity;

	// === HORIZONTAL CURVATURE LIFT (Front-to-Back Flow) ===
	// Project water velocity onto XY plane (remove vertical component)
	FVector waterFlowHorizontal = FVector(relWaterVel.X, relWaterVel.Y, 0.0f);
	float horizontalFlowMagnitude = waterFlowHorizontal.Size();

	float liftAmountHorizontal = 0.0f;
	if (horizontalFlowMagnitude > 0.01f) // Avoid division by zero
	{
		FVector waterFlowHorizontalNorm = waterFlowHorizontal / horizontalFlowMagnitude;
		// Compare to forward direction (front-to-back flow gives max lift)
		FVector forwardsHorizontal = FVector(this->forwards.X, this->forwards.Y, 0.0f).GetSafeNormal();
		float cosAngleHorizontal = FMath::Abs(waterFlowHorizontalNorm | forwardsHorizontal);

		if (cosAngleHorizontal > 0.87f)
		{
			float liftCoefficient = apxLiftCoefficient(cosAngleHorizontal);
			liftAmountHorizontal = liftCoefficient * liftMagnitude *
				horizontalFlowMagnitude * horizontalFlowMagnitude *
				this->effectiveWaterHeight;
		}
	}

	// === VERTICAL CURVATURE LIFT (Bottom-to-Top Flow) ===
	// Project water velocity onto XZ plane (remove Y component - sideways)
	FVector waterFlowVertical = FVector(relWaterVel.X, 0.0f, relWaterVel.Z);
	float verticalFlowMagnitude = waterFlowVertical.Size();

	float liftAmountVertical = 0.0f;
	if (verticalFlowMagnitude > 0.01f) // Avoid division by zero
	{
		FVector waterFlowVerticalNorm = waterFlowVertical / verticalFlowMagnitude;
		// Compare to up direction (bottom-to-top flow gives max lift)
		FVector upVertical = FVector(this->up.X, 0.0f, this->up.Z).GetSafeNormal();
		float cosAngleVertical = FMath::Abs(waterFlowVerticalNorm | upVertical);

		if (cosAngleVertical > 0.87f)
		{
			float liftCoefficient = apxLiftCoefficient(cosAngleVertical);
			liftAmountVertical = liftCoefficient * liftMagnitude *
				verticalFlowMagnitude * verticalFlowMagnitude *
				this->effectiveWaterHeight;
		}
	}

	if (debugLiftLog)
	{
		UE_LOG(LogSurf, Warning, TEXT("  Rail Lift (effectiveH=%.2f col=%.2f slopeSin=%.2f) - Horizontal: cos=%.3f lift=%.2f, Vertical: cos=%.3f lift=%.2f"),
			this->effectiveWaterHeight, this->waterColumnAbove, this->slopeSin,
			horizontalFlowMagnitude > 0.01f ? FMath::Abs((waterFlowHorizontal / horizontalFlowMagnitude) | FVector(this->forwards.X, this->forwards.Y, 0.0f).GetSafeNormal()) : 0.0f,
			liftAmountHorizontal,
			verticalFlowMagnitude > 0.01f ? FMath::Abs((waterFlowVertical / verticalFlowMagnitude) | FVector(this->up.X, 0.0f, this->up.Z).GetSafeNormal()) : 0.0f,
			liftAmountVertical);
		UE_LOG(LogSurf, Warning, TEXT("  relWaterVel: (%.1f, %.1f, %.1f), forwards: (%.2f, %.2f, %.2f), up: (%.2f, %.2f, %.2f)"),
			relWaterVel.X, relWaterVel.Y, relWaterVel.Z,
			this->forwards.X, this->forwards.Y, this->forwards.Z,
			this->up.X, this->up.Y, this->up.Z);
	}

	return liftAmountHorizontal + liftAmountVertical;
}

FVector AFluidDynamics::calcLiftForce()
{
	// Coefficients live solely in the tuning subsystem (single source of truth).
	const USurfTuningSubsystem* T = Tuning ? Tuning : GetDefault<USurfTuningSubsystem>();
	const float liftMagnitude     = T->bottomLiftMagnitude;
	const float railLiftMagnitude = T->railLiftMagnitude;
	const float finLiftMagnitude  = T->finLiftMagnitude;
	const bool debugLiftLog = (this->debugLift || SurfDebug::ShouldDebug(this, TEXT("lift"))) && shouldDebugLog();
	// the lift is depending on the yaw angle compared the relative water velocity.
	// Start by calculating the yaw angle, since the lift is less if the relative water velocity isn't perpendicular to the board
	FVector relativeWaterDirection = this->sharedCalculations->relativeWaterVelocityNormalized;
	float cosYawAngle = this->forwards | this->sharedCalculations->relativeWaterVelocityDirectionProjectedUp;
	float absCosPitchAngleOfAttack = abs(this->cosPitchAngleOfAttack);

	/** Calculate the lift from the rails and fins */
	FVector railLiftForce = FVector::ZeroVector;

	// Gate rail lift on the surfer's COMMANDED lateral weight shift (amountToTheRight), NOT the
	// board's measured roll. Rail Bernoulli lift is the weight-shift STEERING force — it pulls the
	// board toward the rail the surfer engages by leaning (see specs/bottom-yaw-hydrofoil.md:294).
	// The prior gate keyed on waveRelativeRollSin (the board's wave-relative roll), which carries a
	// ~4 Hz UNINTENTIONAL wobble; slaving the engaged rail to it made the force dither into/out of
	// the wave (flipping ~35x over a ride, netting OUT ~57% of firing ticks). The commanded lean
	// flips sign only ~2x over the same ride — it is the surfer's intent, wobble-free. This mirrors
	// the bottom-hydrofoil turn-gate, which already gates carve thrust on lateralShift for the same
	// reason ("wave pushing me sideways" vs "I'm actively carving"). See specs/rail-lift-weight-shift-steer.md.
	//
	// s = amountToTheRight - 0.5: >0 = leaning right (right rail dips), <0 = leaning left (left rail
	// dips). The per-side direction is UNCHANGED (VE_Left -> -left, VE_Right -> +left = "toward the
	// engaged rail"); only the signal that opens the gate changes, so no new sign mapping is introduced.
	// NOTE (steering-only, per specs/rail-lift-weight-shift-steer.md): with weight centered (trimming,
	// not leaning) the gate is CLOSED, so rail lift provides no passive into-wave grip — grip during
	// trim is expected to be carried by the engine velocity-redirect (carve-grip-via-redirect.md).
	// Whether a passive baseline is needed is a "decide after measuring" open question.
	bool shouldApplyRailLift = false;
	float intentGate = 0.0f;
	float commandedShift = 0.0f;
	if (this->sharedCalculations && this->sharedCalculations->weightDistribution)
	{
		commandedShift = this->sharedCalculations->weightDistribution->amountToTheRight - 0.5f;
		const float shiftThreshold = 0.05f; // below this = trimming, not steering: gate closed
		if (this->side == ESide::VE_Left && commandedShift < -shiftThreshold)
		{
			shouldApplyRailLift = true;
		}
		else if (this->side == ESide::VE_Right && commandedShift > shiftThreshold)
		{
			shouldApplyRailLift = true;
		}
		// SmoothStep ramp (same shape as the bottom-hydrofoil intentGate) so the force scales in
		// with commitment instead of snapping on at the threshold.
		intentGate = FMath::SmoothStep(0.05f, 0.20f, FMath::Abs(commandedShift));
	}

	if (shouldApplyRailLift)
	{
		float railLiftAmount = calcRailLiftForceAmount(railLiftMagnitude, cosYawAngle) * intentGate;

		if (debugLiftLog)
		{
			UE_LOG(LogSurf, Warning, TEXT("Rail Lift Debug - Side: %d, railLiftAmount: %.2f, commandedShift: %.3f, intentGate: %.3f"),
				(int)this->side, railLiftAmount, commandedShift, intentGate);
		}

		// Rail lift direction: perpendicular to the rail surface, pointing AWAY from the board
		// For left rail: points in -left direction (away from board center, which is to the right of left rail)
		// For right rail: points in +left direction (away from board center, which is to the left of right rail)
		FVector railLiftDirection = (this->side == ESide::VE_Left) ? -this->sharedCalculations->left : this->sharedCalculations->left;
		railLiftForce = railLiftDirection * railLiftAmount;
	}

	/** Now calculate the bottom-of-board hydrodynamic force (Bernoulli suction from rocker).
	 * Direction: world-down (-Z). Physically a perpendicular-to-surface force has a horizontal
	 * component on a tilted wave (= |force| × sin(slope)), but in reality that horizontal part
	 * is opposed by buoyancy of the displaced water — which this simulation doesn't model
	 * explicitly. Aligning the suction with gravity is the simulation-level shortcut that keeps
	 * the in-plane force budget owned by the supplement / drag and avoids spurious up-slope
	 * acceleration when the lift coefficient peaks near 20° angle of attack on a steep wave.
	 */
	FVector bottomLiftForce = FVector::ZeroVector;

	// Compute TRUE cosines locally just for the bottom-lift gate. The stored this->cosPitchAngleOfAttack and cosYawAngle are
	// scaled by the actor's transform scale (other formulas like along-thrust depend on that). For this gate we want the
	// real angle, independent of scale.
	FVector forwardsNorm = this->forwards.GetSafeNormal();
	float trueCosYaw = forwardsNorm | this->sharedCalculations->relativeWaterVelocityDirectionProjectedUp;
	// localLeftForBottom carries the actor's transform scale; VectorPlaneProject requires a
	// unit-length plane normal so normalize before passing it in.
	FVector localLeftForBottom = GetTransform().TransformVector(FVector(-1.0, 0.0, 0.0)).GetSafeNormal();
	FVector localRelWaterVelProjLeft = FVector::VectorPlaneProject(
		this->sharedCalculations->relativeWaterVelocityNormalized, localLeftForBottom).GetSafeNormal();
	float trueCosPitch = forwardsNorm | localRelWaterVelProjLeft;
	float absTrueCosPitch = abs(trueCosPitch);

	// Only calculate bottom lift for the bottom component
	if(this->side == ESide::VE_Down && absTrueCosPitch >= 0.87) // Only apply bottom lift if pitch angle is less than ~30 degrees
	{
		float liftCoefficient = apxLiftCoefficient(absTrueCosPitch);
		// Per-actor amountWetted (this actor's own depth, sharp smoothstep) so the deeper (submerged) rail
		// gets more lift than the raised one — the asymmetry the per-SC gates can't provide. See
		// specs/per-actor-wetting.md.
		float liftAmount = abs(this->sharedCalculations->relativeWaterVelocity.SizeSquared() * liftCoefficient * trueCosYaw * liftMagnitude * this->actorWetted * this->wettedForceCompensation);
		FVector liftDirection = FVector(0.0, 0.0, -1.0);
		bottomLiftForce = liftDirection * liftAmount;

		if (debugLiftLog)
		{
			UE_LOG(LogSurf, Warning, TEXT("Bottom Lift FIRED [%s] - absTrueCosPitch: %.3f, trueCosYaw: %.3f, liftCoef: %.3f, relWaterVelSq: %.1f, liftMag: %.2f, actorWetted: %.3f -> liftAmount: %.2f, force: (%.1f, %.1f, %.1f)"),
				*GetName(), absTrueCosPitch, trueCosYaw, liftCoefficient,
				this->sharedCalculations->relativeWaterVelocity.SizeSquared(), liftMagnitude, this->actorWetted, liftAmount,
				bottomLiftForce.X, bottomLiftForce.Y, bottomLiftForce.Z);
		}
	}
	else if (this->side == ESide::VE_Down && debugLiftLog)
	{
		UE_LOG(LogSurf, Warning, TEXT("Bottom Lift GATED OFF [%s] - absTrueCosPitch: %.3f (need >= 0.87), trueCosYaw: %.3f"),
			*GetName(), absTrueCosPitch, trueCosYaw);
	}

	// Wave-slope gravity supplement: extra downhill force along the wave-tangent plane, on top
	// of natural UE gravity. Direction = waveSlopeDownVec (magnitude = sin(slope)). Gates:
	//   - AmountPlaning > 0    — fires only while board is actively surfing (cuts trough-side drift)
	//   - smoothstep on slope  — fires only on real wave faces, not residual-ripple noise
	//                            during cruise (otherwise the board never decelerates).
	// Bottom side only to avoid double-counting from rails/fins/tail. See header for the
	// physical-correctness trade-off vs. the older board.forwards-aligned supplement, and
	// specs/wave-slope-gravity-supplement.md for the threshold rationale and calibration data.
	FVector waveSlopeGravityForce = FVector::ZeroVector;
	if (this->side == ESide::VE_Down && waveSlopeGravityCoefficient != 0.0f)
	{
		const FVector waveDown = this->sharedCalculations->waveSlopeDownVec;
		const float amountPlaning = this->sharedCalculations->AmountPlaning;
		const float waveDownMag = waveDown.Size();
		const float slopeGate = FMath::SmoothStep(0.10f, 0.20f, waveDownMag);
		if (amountPlaning > 0.0f && slopeGate > 0.0f)
		{
			// CONTACT GATE. This is the propulsion that drives planing, and until now nothing in it
			// knew whether the board was in the water: amountPlaning is pure board SPEED (it read a
			// flat 0.75 right through a 40 cm-high airborne window) and slopeGate is wave STEEPNESS,
			// which is at its maximum exactly where the lip throws the board clear. Measured airborne
			// horizontal acceleration was 13.4 m/s², with this term the dominant contributor.
			// The gate is exactly 1 for a submerged actor, so riding is untouched.
			// See specs/airborne-force-gating.md.
			waveSlopeGravityForce = waveSlopeGravityCoefficient * waveDown * amountPlaning * slopeGate
				* this->waterContactGate;

			// Per-actor cap (mirrors maxHydrofoilForceAmount pattern). Prevents large
			// `waveSlopeGravityCoefficient` values from producing 60-70 kN per-actor spikes
			// that, when the board tilts, leak into Y/Z and overwhelm sideways/vertical
			// damping — the catastrophic launch at the wave-catch transition. With the cap,
			// the coefficient can be tuned for cruise speed without worrying about peak chaos.
			float supplementCappedRatio = 1.0f;
			if (maxSupplementForce > 0.0f)
			{
				const float rawMag = waveSlopeGravityForce.Size();
				if (rawMag > maxSupplementForce)
				{
					supplementCappedRatio = maxSupplementForce / rawMag;
					waveSlopeGravityForce *= supplementCappedRatio;
				}
			}

			// Propulsion budget (FR6). This force points down the wave slope, not along the board, so
			// only its FORWARD component is propulsion — decompose and compress that alone. Scaling
			// the whole vector would also weaken the down-slope pull the board needs to stay on the
			// face, which is not what this governor is for.
			if (this->sharedCalculations)
			{
				const FVector fwdUnit = this->sharedCalculations->forwards.GetSafeNormal();
				const float alongFwd = FVector::DotProduct(waveSlopeGravityForce, fwdUnit);
				if (alongFwd > 0.0f)
				{
					this->sharedCalculations->RegisterForwardDrive(alongFwd); // raw demand
					const float scale = this->sharedCalculations->GetPropulsionScale();
					waveSlopeGravityForce += fwdUnit * (alongFwd * (scale - 1.0f));
				}
			}

			if (debugLiftLog)
			{
				UE_LOG(LogSurf, Warning, TEXT("Wave-Slope Gravity [%s] - slopeSin: %.3f, slopeGate: %.3f, amountPlaning: %.3f, coef: %.2f, cappedRatio: %.3f, waveDown: (%.3f, %.3f, %.3f), force: (%.1f, %.1f, %.1f) magnitude: %.1f"),
					*GetName(), waveDownMag, slopeGate, amountPlaning, waveSlopeGravityCoefficient, supplementCappedRatio,
					waveDown.X, waveDown.Y, waveDown.Z,
					waveSlopeGravityForce.X, waveSlopeGravityForce.Y, waveSlopeGravityForce.Z, waveSlopeGravityForce.Size());
			}
		}
	}

// Lateral carving force (replaces the old alongThrust). Fires whenever the board is rolled
	// relative to the LOCAL WAVE SURFACE — independent of pitch, so it works during sustained
	// turns that the bottomLift / cosPitch gate would otherwise miss. Force points TOWARD the
	// rail dipped relative to the wave surface (i.e., toward the direction of intended turn).
	FVector lateralTurnForce = FVector::ZeroVector;
	if (this->side == ESide::VE_Down)
	{
		// Carve keys on WORLD-relative roll (lean vs horizontal), NOT wave-relative roll: a board
		// trimming level down the line has ~0 worldRelativeRollSin even on a steep face, so it goes
		// straight at neutral and only a deliberate lean carves. waveRelativeRollSin would be large at
		// neutral (board horizontal vs tilted face) and carve the nose into the wave on its own.
		// See specs/lateral-turn-world-relative-roll.md.
		float worldRollSin = this->sharedCalculations->worldRelativeRollSin;
		float absWorldRollSin = FMath::Abs(worldRollSin);
		float absSinPitch = FMath::Abs(this->sinPitchAngleOfAttack);

		// Small-roll deadzone: the down-the-line over-carve is SEEDED by only ~2-6deg of residual
		// worldRelativeRollSin (looks like "no roll") that, at planing speed, runs away into a hard
		// yaw into the wave even with centered weight. A smoothstep gate zeroes the carve below the
		// deadzone and ramps to FULL by 2x the deadzone — so sub-threshold roll can't seed the turn,
		// but a deliberate lean keeps full carve authority above the band (no magnitude loss, unlike a
		// subtractive deadzone). Pairs with lateralTurnSpeedCap: deadzone stops INITIATION, cap tames
		// the SPIKE. 0 disables the gate. See specs/carve-grip-via-redirect.md and small-weight-shift-sharp-turn.
		const float rollDeadzone = FMath::Max(0.0f, lateralTurnRollDeadzone);
		const float rollGate = (rollDeadzone > 0.0f)
			? FMath::SmoothStep(rollDeadzone, 2.0f * rollDeadzone, absWorldRollSin)
			: 1.0f;
		float gatedRollSin = absWorldRollSin * rollGate;

		if (gatedRollSin > 0.001f && absSinPitch > 0.001f && lateralTurnCoefficient > 0.0f)
		{
			// amountUnderWater intentionally NOT included — lateral turn models a board-wide
			// rotation coupling, not a per-actor surface-contact phenomenon. Including it
			// distorted the force's distribution along the board's length (nose contribution
			// collapsed when pitched up). See specs/lateral-turn-uniform-along-length.md.
			//
			// Saturating speed term: relWaterVelMag is ~150 in normal trim but spikes to ~1200+ in a
			// hard turn, so the raw linear factor turns a few-degree residual roll into a runaway carve
			// (~36k) that yaws the board into the wave under centered weight. vEff = v / sqrt(1+(v/cap)^2)
			// is ~linear below the cap (normal carving untouched) and asymptotes to the cap above it.
			// cap <= 0 disables (raw speed), for A/B. See specs/carve-grip-via-redirect.md.
			const float relWaterVel = this->sharedCalculations->relativeWaterVelocityMagnitude;
			float effectiveTurnSpeed = relWaterVel;
			if (lateralTurnSpeedCap > 0.0f)
			{
				const float r = relWaterVel / lateralTurnSpeedCap;
				effectiveTurnSpeed = relWaterVel / FMath::Sqrt(1.0f + r * r);
			}
			float lateralAmount = gatedRollSin *   // deadzone-subtracted roll: kills the sub-threshold runaway seed
				absSinPitch *                        // amplifies via rocker; nose actors have higher pitch
				effectiveTurnSpeed *
				this->sharedCalculations->AmountPlaning *
				lateralTurnCoefficient *
				lateralTurnMultiplier *              // per-actor 0..1 ramp; 0 on tail actors
				// CONTACT GATE. The note above rules out amountUnderWater because a PARTIAL per-actor
				// gate distorted the force's distribution along the board's length. That argument is
				// about which submerged actor contributes how much; it says nothing about a board with
				// no water under it at all, where every other gate here (roll, pitch, speed, planing)
				// still reads full. Ungated it carved an airborne board ~20 deg.
				// Exactly 1 for a submerged actor, so the uniform-along-length behaviour is unchanged
				// while the board is in the water. See specs/airborne-force-gating.md.
				this->waterContactGate;

			float rollSign = FMath::Sign(worldRollSin);
			FVector lateralDirection = -rollSign * this->sharedCalculations->left;
			lateralTurnForce = lateralDirection * lateralAmount;

			// Contribute to planing-force detection so AmountPlaning continues to track when this is the dominant lateral force.
			this->sharedCalculations->AddPlaningForce(lateralTurnForce);

			if (debugLiftLog)
			{
				UE_LOG(LogSurf, Warning, TEXT("Lateral Turn FIRED [%s] - worldRollSin: %.3f, gatedRoll: %.3f, absSinPitch: %.3f, relWaterVelMag: %.1f, effTurnSpeed: %.1f, lateralAmount: %.2f, force: (%.1f, %.1f, %.1f)"),
					*GetName(), worldRollSin, gatedRollSin, absSinPitch,
					relWaterVel, effectiveTurnSpeed,
					lateralAmount,
					lateralTurnForce.X, lateralTurnForce.Y, lateralTurnForce.Z);
			}
		}

		// ----- Commanded-lean hard-carve (additive; bypasses the roll-stiffness bottleneck) -----
		// The board is roll-stiff: on a hard lean it banks only ~2-3deg (worldRollSin ~0.04, right at
		// the deadzone), so the roll-gated carve above is ~0 during a hard turn and the board turns
		// barely faster than trim. This term injects carve authority from the COMMANDED lean directly
		// (amountToTheRight), so a deliberate hard commit turns even when the board won't bank. It is
		// ADDITIVE and only active above `start`, which the gentle-trim regime never reaches, so
		// down-the-line trim is untouched. Direction is driven by the command (toward the commanded
		// rail), robust to the tiny/noisy achieved-roll sign. Default boost 0 = off.
		// See specs/hard-carve-progressive-boost.md.
		if (lateralTurnHardCarveBoost > 0.0f && absSinPitch > 0.001f && lateralTurnCoefficient > 0.0f
			&& this->sharedCalculations->weightDistribution)
		{
			const float cShift = this->sharedCalculations->weightDistribution->amountToTheRight - 0.5f;
			const float commitRamp = FMath::SmoothStep(lateralTurnHardCarveStart, lateralTurnHardCarveFull, FMath::Abs(cShift));
			if (commitRamp > 0.0f)
			{
				// Same saturating speed term as the base carve (recomputed here: the base copy is
				// scoped inside the roll gate, which this block deliberately runs outside of).
				const float relWaterVelCmd = this->sharedCalculations->relativeWaterVelocityMagnitude;
				float effectiveTurnSpeedCmd = relWaterVelCmd;
				if (lateralTurnSpeedCap > 0.0f)
				{
					const float r = relWaterVelCmd / lateralTurnSpeedCap;
					effectiveTurnSpeedCmd = relWaterVelCmd / FMath::Sqrt(1.0f + r * r);
				}
				const float commandedCarveAmount = commitRamp *
					lateralTurnHardCarveBoost *
					absSinPitch *
					effectiveTurnSpeedCmd *
					this->sharedCalculations->AmountPlaning *
					lateralTurnCoefficient *
					lateralTurnMultiplier;
				// Toward the commanded rail. sign(cShift)*left matches the base carve's
				// -sign(worldRollSin)*left direction for the same command (verified via signed-yaw A/B).
				const FVector commandedDir = FMath::Sign(cShift) * this->sharedCalculations->left;
				const FVector commandedCarve = commandedDir * commandedCarveAmount;
				lateralTurnForce += commandedCarve;
				this->sharedCalculations->AddPlaningForce(commandedCarve);

				if (SurfDebug::ShouldDebug(this, TEXT("carve")) && shouldDebugLog())
				{
					UE_LOG(LogSurf, Warning, TEXT("HardCarveAdd [%s] cShift=%.3f ramp=%.2f amt=%.1f worldRollSin=%.3f"),
						*GetName(), cShift, commitRamp, commandedCarveAmount, worldRollSin);
				}
			}
		}

}

// Fin lift: airfoil-style side force that opposes sideways slip and provides directional
// stability. Acts perpendicular to flow in the fin plane, magnitude peaks near 45° angle
// of attack and falls to zero at parallel flow (no yaw) and pure-perpendicular flow (stall).
//
// cosYawAngleOfAttack on a fin actor = surfboardLeft · relativeWaterVelocityDirection. So:
//   value = 0   → flow exactly along board.forwards (no yaw, no lift)
//   value = ±1  → flow exactly along board.left (pure slip — stalled, no lift)
//   value mid-range → angled flow → lift peaks
// Treating the value as sin(yaw) (where yaw is angle between flow and board.forwards),
// lift ∝ |sinYaw| × cosYawFromForward × v² × coef × amountUnderWater.
// Direction = sign(sinYaw) × board.left so force opposes slip:
//   slip in +board.left → relWaterVel.left < 0 → cosYaw < 0 → force in -board.left ✓
// Hydrofoil lift perpendicular to flow, in the horizontal plane around board.up.
// Decomposed in board frame:
//   anti-slip component (along ∓board.left) = L · cos α — matches the prior fin-lift behavior
//   forward component   (along +board.forwards) = L · |sin α| — induced thrust (the "carve coupling")
// Direction is constructed by rotating the existing anti-slip unit vector toward +board.forwards
// by α; this reuses the codebase's well-tested anti-slip sign convention (the in-engine
// board.left points to the surfer's RIGHT, so +sign(sinSlip) * board.left opposes slip).
// See specs/fin-carve-coupling.md.
FVector finLiftForce = FVector::ZeroVector;
if (this->side == ESide::VE_Fin && finLiftMagnitude > 0.0f)
{
	const float sinSlip = this->cosYawAngleOfAttack;
	const float absSinSlip = FMath::Abs(sinSlip);
	const float sinSlipSq = sinSlip * sinSlip;
	const float cosSlip = FMath::Sqrt(FMath::Max(0.0f, 1.0f - sinSlipSq));
	if (absSinSlip > 0.001f)
	{
		// Fins are fully submerged by construction — no amountUnderWater / effectiveWaterHeight gating.
		// "By construction" holds for a board ON the wave, not one clear of it, and this force has a
		// FORWARD component (liftDir's absSinSlip term), so ungated it drove an airborne board — at
		// high slip, which is exactly the attitude a board is thrown off the lip in. The contact gate
		// is the only wetting signal this site has. See specs/airborne-force-gating.md.
		const float v2 = this->sharedCalculations->relativeWaterVelocity.SizeSquared();
		const float finLiftL = v2 * absSinSlip * finLiftMagnitude * this->waterContactGate;

		// Build liftDir from UNIT basis vectors: sharedCalculations->left/forwards carry the
		// actor's ~0.2 transform scale, which would otherwise shrink the force another ~5×
		// (on top of the sinSlip fix) and leave liftDir non-unit. With unit, orthogonal left
		// and forwards and cosSlip²+sinSlip²=1, liftDir is unit. See specs/fin-force-normalization.md.
		const FVector antiSlipDir = FMath::Sign(sinSlip) * this->sharedCalculations->left.GetSafeNormal();
		// antiSlipForceScale dials down the anti-slip side component (carve-grip redirect takes over).
		const FVector liftDir     = antiSlipDir * cosSlip * this->antiSlipForceScale
			+ this->sharedCalculations->forwards.GetSafeNormal() * absSinSlip;
		finLiftForce = liftDir * finLiftL;

		if (debugLiftLog)
		{
			const float antiSlipMag = finLiftL * cosSlip;
			const float forwardMag  = finLiftL * absSinSlip;
			UE_LOG(LogSurf, Warning, TEXT("Fin Lift FIRED [%s] - sinSlip: %.3f, cosSlip: %.3f, v²: %.1f, coef: %.3f -> L: %.2f (antiSlip=%.2f fwd=%.2f), force: (%.1f, %.1f, %.1f)"),
				*GetName(), sinSlip, cosSlip, v2, finLiftMagnitude,
				finLiftL, antiSlipMag, forwardMag,
				finLiftForce.X, finLiftForce.Y, finLiftForce.Z);
		}
		}
	}

	if(this->debugDrawForces || SurfDebug::ShouldDebug(this, TEXT("forces")))
	{
		// Draw rail lift
		if (!railLiftForce.IsNearlyZero())
		{
			DrawDebugCone(
				GetWorld(),
				GetActorLocation(),
				railLiftForce,
				railLiftForce.Size() * debugForceDrawScale,
				FMath::DegreesToRadians(1),
				FMath::DegreesToRadians(1),
				50,	// Thickness
				FColor::Turquoise, // Different color for rail lift (distinct from cyan fin drag)
				false,
				0.1f); // Duration
		}

		// Draw fin lift
		if (!finLiftForce.IsNearlyZero())
		{
			DrawDebugCone(
				GetWorld(),
				GetActorLocation(),
				finLiftForce,
				finLiftForce.Size() * debugForceDrawScale,
				FMath::DegreesToRadians(1),
				FMath::DegreesToRadians(1),
				50,
				FColor::Blue,
				false,
				0.1f);
		}

		// Draw bottom lift
		if (!bottomLiftForce.IsNearlyZero())
		{
			DrawDebugCone(
				GetWorld(),
				GetActorLocation(),
				bottomLiftForce,
				bottomLiftForce.Size() * debugForceDrawScale,
				FMath::DegreesToRadians(1),
				FMath::DegreesToRadians(1),
				50,	// Thickness
				this->debugLiftColor,
				false,
				0.1f); // Duration
		}

		// Draw lateral turn force
		if (!lateralTurnForce.IsNearlyZero())
		{
			DrawDebugCone(
				GetWorld(),
				GetActorLocation(),
				lateralTurnForce,
				lateralTurnForce.Size() * debugForceDrawScale,
				FMath::DegreesToRadians(1),
				FMath::DegreesToRadians(1),
				50,	// Thickness
				FColor::Magenta, // Distinct color for lateral turn force
				false,
				0.1f); // Duration
		}

		// Draw wave-slope gravity force
		if (!waveSlopeGravityForce.IsNearlyZero())
		{
			DrawDebugCone(
				GetWorld(),
				GetActorLocation(),
				waveSlopeGravityForce,
				waveSlopeGravityForce.Size() * debugForceDrawScale,
				FMath::DegreesToRadians(1),
				FMath::DegreesToRadians(1),
				50,	// Thickness
				FColor::Purple, // Distinct color for wave-slope gravity (distinct from yellow bottom drag)
				false,
				0.1f); // Duration
		}

	}

	// Register lift sub-components with SharedCalculations for the per-tick yaw torque budget log
	// (gated by SurfDebug "torque" flag, evaluated in SharedCalculations::calculateAll).
	if (this->sharedCalculations)
	{
		const FVector actorPos = GetActorLocation();
		this->sharedCalculations->RegisterAppliedForce(TEXT("bottomBernoulliLift"), actorPos, bottomLiftForce);
		this->sharedCalculations->RegisterAppliedForce(TEXT("railLift"),            getComHeightApplyPoint(), railLiftForce);
		this->sharedCalculations->RegisterAppliedForce(TEXT("lateralTurn"),         actorPos, lateralTurnForce);
		this->sharedCalculations->RegisterAppliedForce(TEXT("waveSlopeGravity"),    actorPos, waveSlopeGravityForce);
		this->sharedCalculations->RegisterAppliedForce(TEXT("finLift"),             actorPos, finLiftForce);

		// Per-position split of lateralTurn so the budget shows whether the per-actor force
		// magnitude × position-from-CoM yields nose-side or tail-side dominant yaw torque.
		// Suffix taken from the actor label's last underscore segment (e.g. "front",
		// "middle-back"). Editor-only; in -game mode launched from the editor binary this
		// is still valid (WITH_EDITOR == true).
#if WITH_EDITOR
		if (!lateralTurnForce.IsNearlyZero())
		{
			const FString fullLabel = GetActorLabel();
			int32 lastUnderscore = INDEX_NONE;
			FString posSuffix = TEXT("unk");
			if (fullLabel.FindLastChar(TCHAR('_'), lastUnderscore))
			{
				posSuffix = fullLabel.RightChop(lastUnderscore + 1);
			}
			this->sharedCalculations->RegisterAppliedForce(
				FString::Printf(TEXT("lateralTurn_%s"), *posSuffix),
				actorPos, lateralTurnForce);
		}
#endif
	}

	// railLift is a sideways (board-left) force applied below the CoM, so it makes a keel-roll
	// (-r_up·F_left) that's the dominant down-the-line destabiliser. Stash it out of the actor-applied
	// lift and apply it at CoM HEIGHT instead (zeroes the vertical moment arm → no roll), keeping the
	// sideways grip and the yaw (r_fwd·F_left). See specs/wave-mass-drag-torque-decoupling.md.
	this->PendingRailLiftForce = railLiftForce;
	// Stash the synthetic-gravity portion so spray consumers can subtract it from the tick's
	// force sum (it rides into applyForceAsImpulse inside this return value).
	this->appliedNonSprayForceThisTick += waveSlopeGravityForce;
	return bottomLiftForce + lateralTurnForce + waveSlopeGravityForce + finLiftForce;
}

	// The following is an aproximation of the lift coefficient, for a given cos of the angle of attack
	// Points used for the aproximation are:
	/**
	 * (1 0.5)
	 * (0.996, 0.95)
	 * (0.984, 1.35)
	 * (0.965, 1.66)
	 * (0.939, 1.8)
	 * (0.9, 1.53)
	 * (0.86, -0.35)
	 */
float AFluidDynamics::apxLiftCoefficient(float cosAngleOfAttack)
{
	return -354.0744 * pow(cosAngleOfAttack, 2) + 665.3898 * cosAngleOfAttack - 310.6660;
}

// Bottom hydrofoil thrust on the board's bottom surface, decomposed into two orthogonal axes:
//   - Pitch hydrofoil: projects flow onto forwards × up plane; produces upthrust + forward propulsion
//     at positive pitch AOA. See specs/bottom-hydrofoil-thrust.md.
//   - Yaw hydrofoil: projects flow onto forwards × left plane; produces anti-slip + forward propulsion
//     at non-zero yaw slip. The bottom acts as a hydrofoil for sideways flow, the same way the fin
//     does (see fin-carve-coupling.md), just with much larger surface area. See specs/bottom-yaw-hydrofoil.md.
// The two planes are orthogonal: pitch projects out the `left` component, yaw projects out the `up`
// component. They fire independently and sum without overlap.
//
// At negative pitch AOA the bottom Bernoulli "lift" path in calcLiftForce takes over (downward
// suction); the pitch hydrofoil returns zero there.
//
// alongThrustCoefficient is unused; kept in the signature for BP compatibility.
float AFluidDynamics::calcBrokenDriveScale() const
{
	if (!this->sharedCalculations)
	{
		return 1.0f;
	}
	// brokenGeo - the wave-geometry service's "the board is behind the peel, in the bore band" -
	// not the foam reads: the ahead-inclusive foam read cut the drive to zero for a whole hard turn
	// whenever the nose swung toward the lip's foam on a clean face (PC bisect 2026-09-17 - the
	// board bled 350 -> 79 cm/s mid-turn and skidded), and the surround read never fired. The
	// service reads 0 through the whole slalom and the pocket (wave-geometry AC2), so keeping
	// speed on the wave costs nothing here by construction.
	return 1.0f - FMath::Clamp(this->sharedCalculations->brokenGeo * this->brokenDriveCut, 0.0f, 1.0f);
}

float AFluidDynamics::calcWithWaveGate() const
{
	if (!this->sharedCalculations || this->yawFwdWithWaveGateOn <= this->yawFwdWithWaveGateOff)
	{
		return 1.0f;
	}
	// The wave's travel axis from the tile geometry (stable per tick; falls back to the placed
	// waveBackDirection), the same reference the crossing diagnostic and the wave-normal damping use.
	const FVector backDir = this->sharedCalculations->resolvedWaveBackDirection.IsZero()
		? this->sharedCalculations->waveBackDirection
		: this->sharedCalculations->resolvedWaveBackDirection;
	const FVector shoreward = FVector(-backDir.X, -backDir.Y, 0.0f).GetSafeNormal();
	const FVector fwdH = FVector(this->sharedCalculations->forwards.X, this->sharedCalculations->forwards.Y, 0.0f).GetSafeNormal();
	if (shoreward.IsNearlyZero() || fwdH.IsNearlyZero())
	{
		return 1.0f;
	}
	// Where the board is GOING, not where it points: a top turn and a punch-through share a nose
	// orientation but not a velocity. Blend the nose in below ~150 cm/s, where the velocity
	// direction is noise (a board at rest in front of an arriving crest is the case that matters
	// there, and its nose points upstream).
	const FVector vel = this->sharedCalculations->componentVelocity;
	const FVector velH = FVector(vel.X, vel.Y, 0.0f);
	const float speed = (float)velH.Size();
	const float dNose = FVector::DotProduct(fwdH, shoreward);
	const float dVel = speed > 1.0f ? FVector::DotProduct(velH / speed, shoreward) : dNose;
	const float velWeight = FMath::Clamp((speed - 50.0f) / 100.0f, 0.0f, 1.0f);
	const float d = FMath::Lerp(dNose, dVel, velWeight);
	const float gate = FMath::SmoothStep(this->yawFwdWithWaveGateOff, this->yawFwdWithWaveGateOn, d);
	// The gate is for a slow board being driven into an arriving crest. A board with speed
	// (a cutback runs the velocity up the face for a second) keeps its drive whatever its heading.
	const float fastWeight = FMath::SmoothStep(this->yawFwdWithWaveGateSpeedMax, this->yawFwdWithWaveGateSpeedMax + 100.0f, speed);
	return FMath::Lerp(gate, 1.0f, fastWeight);
}

FVector AFluidDynamics::calcThrustForce(FColor debugColor = FColor::Red)
{
	// Coefficients live solely in the tuning subsystem (single source of truth).
	const USurfTuningSubsystem* T = Tuning ? Tuning : GetDefault<USurfTuningSubsystem>();
	const float alongThrustCoefficient   = T->alongThrustCoefficient;
	const float upwardsThrustCoefficient = T->upwardsThrustCoefficient;
	if (this->side != ESide::VE_Down)
	{
		return FVector::ZeroVector;
	}

	const FVector relVel = this->sharedCalculations->relativeWaterVelocity;
	const float amountUnderWater = this->sharedCalculations->amountUnderWater;
	// The old per-SC amountWetted is dead (pegged at 1.0, and per-SC so it can't carry left/right). Use
	// this actor's PER-ACTOR amountWetted (sharp smoothstep on its own depth) so the up-thrust / yaw-
	// hydrofoil / slope-thrust get a real submerged-side asymmetry. Aliased here so the force terms and
	// logs below pick it up unchanged. See specs/per-actor-wetting.md.
	const float amountWetted = this->actorWetted;
	// Force-gate variant: raw actorWetted lifted by wettedForceCompensation so the actorWetted-gated force
	// MAGNITUDES (up-thrust / forward thrust / yaw hydrofoil / slope thrust) recover the strength lost when
	// the always-1.0 per-SC gate was replaced by the ~0.82-mean per-actor gate. Uniform scale → the L/R
	// asymmetry ratio (the righting signal) is unchanged; logs keep reporting raw amountWetted so the
	// submerged-side diagnostic stays honest. See specs/per-actor-wetting.md.
	const float wettedForce = amountWetted * this->wettedForceCompensation;
	const bool debugLogThrust = (this->debugThrust || SurfDebug::ShouldDebug(this, TEXT("thrust"))) && shouldDebugLog();
	const bool debugDrawAny   = this->debugDrawForces || SurfDebug::ShouldDebug(this, TEXT("forces"));

	// === PITCH HYDROFOIL ===
	// Project relative water velocity onto the board's forward-up plane (the pitch plane).
	FVector pitchForce = FVector::ZeroVector;
	if (upwardsThrustCoefficient > 0.0f || this->forwardsThrustCoefficient > 0.0f || this->actorForwardsThrustCoefficient > 0.0f)
	{
		const FVector relVelInPitchPlane = relVel - FVector::DotProduct(relVel, this->left) * this->left;
		const float v2InPitchPlane = relVelInPitchPlane.SizeSquared();
		if (v2InPitchPlane >= 1.0f)
		{
			const FVector flowDir = relVelInPitchPlane / FMath::Sqrt(v2InPitchPlane);
			const float sinAOA = FVector::DotProduct(flowDir, this->up);
			if (sinAOA >= 0.001f)
			{
				// Thrust perpendicular to flow, in the pitch plane.
				FVector thrustDir = FVector::CrossProduct(this->left, flowDir);
				if (FVector::DotProduct(thrustDir, this->up) < 0.0f)
				{
					thrustDir = -thrustDir;  // sign correction for unusual orientations
				}

				// Three "common mag" variants for the three thrust components:
				//   commonMagUp        — for upForce, NO effectiveWaterHeight. Real hydrofoil lift
				//                        (L = ½ρv²A·Cl(α)) doesn't scale with water-column-above; the
				//                        effH factor was miscarried from the wave-mass-energy theme.
				//                        See specs/bottom-hydrofoil-upthrust-decoupling.md.
				//   commonMagPerActor  — for actorFwdForce. Keeps per-actor effH because each rocker-
				//                        tilted actor has its own direction and represents wave-mass-
				//                        driven propulsion at its local water column.
				//   commonMagBoardWide — for boardFwdForce. Board-wide effH because boardFwd direction
				//                        is the same across all actors, so per-actor sampling noise
				//                        would only become spurious yaw torque.
				//                        See specs/per-actor-vs-board-wide-sampling.md.
				const float commonMagUp = v2InPitchPlane
				                        * sinAOA
				                        * wettedForce;

				const float commonMagPerActor = v2InPitchPlane
				                              * sinAOA
				                              * wettedForce
				                              * this->effectiveWaterHeight;

				const float boardWideEffH = FMath::Min(
					this->baseHeight
						+ this->sharedCalculations->boardWideWaterColumnAbove
						+ this->sharedCalculations->boardWideSlopeSin * this->slopeHeight,
					this->maxEffectiveWaterHeight);
				const float commonMagBoardWide = v2InPitchPlane
				                               * sinAOA
				                               * wettedForce
				                               * boardWideEffH;

				// Decompose thrustDir onto three independent unit axes — board.up (actor's own up),
				// board-shared forwards, and actor-local forwards — and scale each by its own coefficient.
				// The board-shared and actor-local forward axes coincide on the middle bottom actor; on
				// rocker-rotated actors (nose tilts up, tail tilts down) they diverge, so the actor-local
				// term adds rocker-aware thrust direction on top of the uniform board-forward propulsion.
				const FVector upUnit       = this->up.GetSafeNormal();
				const FVector boardFwdUnit = this->sharedCalculations->forwards.GetSafeNormal();
				const FVector actorFwdUnit = this->forwards.GetSafeNormal();
				const float upDot       = FVector::DotProduct(thrustDir, upUnit);
				const float boardFwdDot = FVector::DotProduct(thrustDir, boardFwdUnit);
				const float actorFwdDot = FVector::DotProduct(thrustDir, actorFwdUnit);

				// Turn-gate: bottom-hydrofoil forward thrust is intended to keep the board from
				// bleeding speed mid-carve (see specs/bottom-hydrofoil-thrust.md). But the formula
				// fires on any positive pitch AOA — including straight-line surf — which makes it
				// a dominant cruise propulsion that prevents the board from ever decelerating.
				//
				// Gate on either commanded lateral weight shift (lateralShift = |amountToTheRight - 0.5|)
				// OR active pumping (PumpInput from the rider's down-gesture). Both are explicit
				// commands the surfer issues to inject forward thrust — carving and pumping are
				// the two physical mechanisms that produce it in real surfing, and the gate's
				// purpose ("only fire when the surfer commands it") applies equally to both.
				//
				// Wave-catch / cruise: neither signal active, gate closed. Carving OR pumping:
				// gate opens. See specs/pumping.md.
				//
				// Why not also gate on observed slip: prior attempt used slipGate * intentGate
				// (AND), but the signals don't co-occur — slip is the lag in response, intent
				// is the command. They overlap < 1% of ticks, killing the gate. Intent alone is
				// the correct signal: it's what the surfer directly commands at the moment they
				// want forward thrust.
				const float absSinSlip = FMath::Abs(this->cosYawAngleOfAttack);
				const float slipGate   = FMath::SmoothStep(0.05f, 0.20f, absSinSlip); // logged only
				const float lateralShift = this->sharedCalculations
					? this->sharedCalculations->lateralShift : 0.0f;
				// PumpInputAttenuated, NOT PumpInput. The pawn rewrites PumpInput with the raw
				// signal every tick and AWeightDistribution attenuates it in place, so whichever of
				// the two actors ticks first decides what this reads. Measured 2026-09-09: with the
				// impulse attenuated to exactly zero on flat water (speedAtt 0.000, forceProxy 0),
				// this gate was still reading 0.443 and sitting wide open, so the hydrofoil poured
				// tens of thousands of newtons of forward thrust into a board that was supposed to
				// be getting nothing from its pump. The debug sphere said grey; the board
				// accelerated anyway.
				const float pumpInput = (this->sharedCalculations && this->sharedCalculations->weightDistribution)
					? this->sharedCalculations->weightDistribution->PumpInputAttenuated : 0.0f;
				// pumpGate scales with the actual pump force injection
				// (MaxPumpForce × pumpInput), not the signal alone. This couples the
				// gate to the rider's energy injection — setting MaxPumpForce = 0
				// closes the gate regardless of pumpInput, and lowering MaxPumpForce
				// linearly tapers the gate's openness. Otherwise the gate would
				// flood the hydrofoil with carve-equivalent forward thrust as soon
				// as the player tapped space, with no physical justification.
				const float maxPumpForce = (Tuning ? Tuning->MaxPumpForce : 0.0f);
				const float pumpForceProxy = maxPumpForce * pumpInput;
				const float intentGate = FMath::SmoothStep(0.05f, 0.20f, lateralShift);
				// Proportional gate: fully open only at a full-strength pump, so the gate IS the
				// pump input (kept as a force proxy so MaxPumpForce = 0 still closes it).
				//
				// History: this was SmoothStep(0, 5000, proxy), tuned 2026-06-11 when MaxPumpForce
				// was 20 000 so that mid-strength pumps saturated it. MaxPumpForce was then raised
				// 10x without rescaling the gate, which left it wide open at PumpInput 0.025 - every
				// pump, however small, was a full launch (measured 2026-09-12, PC: pumpGate=1.000 on
				// essentially every pump tick of a 30 s stationary-pirouette exploit).
				const float pumpGate   = FMath::SmoothStep(0.0f, FMath::Max(maxPumpForce, 1.0f), pumpForceProxy);
				// Carve (lateralShift / intentGate) was REMOVED from the gate: gating this raw forward
				// push on the steering weight-shift made carving behave like a throttle with a deadzone
				// edge (small weight change near lateralShift=0.05 → big velocity jump). Carve-driven
				// forward speed now comes from the slip-proportional bottom YAW hydrofoil
				// (yawHydrofoilCoefficient) — a perpendicular-to-flow lift that REDIRECTS velocity (smooth,
				// scales with sin²(slip), no deadzone), which is the physically-correct mechanism. The
				// pitch forwardsThrust stays gated on PUMP only (the active boost). intentGate kept for
				// the log. See specs/passive-slope-thrust.md / the carve-redirection notes.
				const float turnGate   = pumpGate;
				// Pitch-amplifying upthrust: linear coef plus a quadratic-in-sinAOA term scaled by
				// upwardsThrustPitchSensitivity. At cruise sinAOA the quadratic term is small; at
				// nose-dive sinAOA it's a substantial counter-torque without inflating cruise.
				const float upEffectiveCoef = upwardsThrustCoefficient + this->upwardsThrustPitchSensitivity * sinAOA;
				const FVector upForce       = upUnit       * (upEffectiveCoef                            * commonMagUp        * upDot);
				FVector boardFwdForce = boardFwdUnit * (turnGate * this->forwardsThrustCoefficient      * commonMagBoardWide * boardFwdDot);
				FVector actorFwdForce = actorFwdUnit * (turnGate * this->actorForwardsThrustCoefficient * commonMagPerActor  * actorFwdDot);
				// Pump-scaled ceiling on the forward pair. The raw v²·sinAOA term is 5-300x over the
				// general per-actor cap on every pump, so without this the launch is always
				// "cap x actor count" (~2 g) no matter how hard the pump. Ceiling scales with the
				// gate so a half pump is a half launch. See PumpForwardForceCap.
				const float pumpFwdCap = (Tuning ? Tuning->PumpForwardForceCap : 0.0f) * turnGate;
				if (pumpFwdCap > 0.0f)
				{
					const FVector fwdPair = boardFwdForce + actorFwdForce;
					const float fwdMag = fwdPair.Size();
					if (fwdMag > pumpFwdCap)
					{
						const float k = pumpFwdCap / fwdMag;
						boardFwdForce *= k;
						actorFwdForce *= k;
					}
				}
				pitchForce = upForce + boardFwdForce + actorFwdForce;

				// TEMPORARY DIAGNOSTIC — unconditional log when actively pumping
				// AND this is a bottom actor (where forward thrust originates).
				// Throttled per-actor via debugFrameCounter (set in setup()).
				// Strip these once pump tuning is dialed in on Android.
				if (this->side == ESide::VE_Down
					&& pumpInput > 0.01f
					&& (debugFrameCounter % 10 == 0))
				{
					UE_LOG(LogSurf, Warning,
						TEXT("PumpDiag-FD[%s]: sinAOA=%.4f v²InPlane=%.0f amtWet=%.3f effH=%.1f boardEffH=%.1f | pumpInput=%.3f pumpGate=%.3f intentGate=%.3f turnGate=%.3f | fwdCoef=%.4f actorFwdCoef=%.4f | upForce=%.0f boardFwd=%.0f actorFwd=%.0f"),
						*GetName(),
						sinAOA, v2InPitchPlane, amountWetted, this->effectiveWaterHeight, boardWideEffH,
						pumpInput, pumpGate, intentGate, turnGate,
						this->forwardsThrustCoefficient, this->actorForwardsThrustCoefficient,
						upForce.Size(), boardFwdForce.Size(), actorFwdForce.Size());
				}

				if (debugLogThrust)
				{
#if WITH_EDITOR
					const FString LabelForLog = GetActorLabel();
#else
					const FString LabelForLog = TEXT("<no-label>");
#endif
					UE_LOG(LogSurf, Warning, TEXT("Bottom Hydrofoil [name=%s label=%s] - sinAOA: %.3f, |sinSlip|: %.3f, slipGate: %.3f, lateralShift: %.3f, intentGate: %.3f, pumpInput: %.3f, pumpGate: %.3f, turnGate: %.3f, v²InPlane: %.1f, upCoef: %.3f, pitchSens: %.3f, upEffectiveCoef: %.3f, boardFwdCoef: %.3f, actorFwdCoef: %.3f, amountWetted: %.3f, amountUnderWater: %.3f, effectiveH: %.2f, boardWideEffH: %.2f -> commonMag(up/perActor/boardWide): %.2f/%.2f/%.2f, upForceMag: %.2f, boardFwdForceMag: %.2f, actorFwdForceMag: %.2f, force: (%.1f, %.1f, %.1f)"),
						*GetName(), *LabelForLog, sinAOA, absSinSlip, slipGate, lateralShift, intentGate, pumpInput, pumpGate, turnGate, v2InPitchPlane, upwardsThrustCoefficient, this->upwardsThrustPitchSensitivity, upEffectiveCoef, this->forwardsThrustCoefficient, this->actorForwardsThrustCoefficient, amountWetted, amountUnderWater, this->effectiveWaterHeight, boardWideEffH,
						commonMagUp, commonMagPerActor, commonMagBoardWide, upForce.Size(), boardFwdForce.Size(), actorFwdForce.Size(),
						pitchForce.X, pitchForce.Y, pitchForce.Z);
				}

				if (debugDrawAny && !pitchForce.IsNearlyZero())
				{
					DrawDebugCone(
						GetWorld(),
						GetActorLocation(),
						pitchForce,
						pitchForce.Size() * debugForceDrawScale,
						FMath::DegreesToRadians(1),
						FMath::DegreesToRadians(1),
						50,
						debugColor,
						false,
						0.1f);
				}
			}
		}
	}

	// === YAW HYDROFOIL ===
	// Project relative water velocity onto the board's bottom plane (forwards × left, normal = up).
	// At non-zero slip the bottom deflects sideways flow → perpendicular-to-flow lift with anti-slip
	// and forward components. Mirror of the fin's carve-coupling shape applied to the bottom surface.
	//
	// IMPORTANT: the slip computation uses a *board-wide* relative water velocity (averaged from
	// front and back SC), not each actor's per-SC value. The yaw hydrofoil's anti-slip direction
	// flips by 180° when sign(sinSlip) flips, so front/back SCs disagreeing on sign produced a
	// catastrophic front-back force opposition that yawed the board into the wave. The board's
	// slip past the water is physically a board-wide property — one slip angle for one rigid
	// board. See specs/yaw-hydrofoil-board-wide-slip.md and specs/per-actor-vs-board-wide-sampling.md.
	FVector yawForce = FVector::ZeroVector;
	if (this->yawHydrofoilCoefficient > 0.0f)
	{
		const FVector relVelBoardWide = (this->sharedCalculations && this->sharedCalculations->otherHalfSharedCalculations)
			? 0.5f * (this->sharedCalculations->relativeWaterVelocity
			          + this->sharedCalculations->otherHalfSharedCalculations->relativeWaterVelocity)
			: relVel;

		const FVector upUnit = this->up.GetSafeNormal();
		const FVector relVelInBottomPlane = relVelBoardWide - FVector::DotProduct(relVelBoardWide, upUnit) * upUnit;
		const float v2InBottomPlane = relVelInBottomPlane.SizeSquared();
		if (v2InBottomPlane >= 1.0f)
		{
			const FVector flowDirYaw = relVelInBottomPlane / FMath::Sqrt(v2InBottomPlane);
			const FVector leftUnit = this->left.GetSafeNormal();
			const float sinSlip = FVector::DotProduct(flowDirYaw, leftUnit);
			const float absSinSlip = FMath::Abs(sinSlip);
			if (absSinSlip > 0.001f)
			{
				const float cosSlip = FMath::Sqrt(FMath::Max(0.0f, 1.0f - sinSlip * sinSlip));
				const FVector forwardsUnit = this->forwards.GetSafeNormal();
				const FVector antiSlipDir  =  FMath::Sign(sinSlip) * leftUnit;
				// Speed attenuation on the FORWARD (carve-coupling) thrust only: it is coef·v²·sin²(slip)·...
				// and v² grows as the board pulls away from the slow water, so it's an unbounded speed
				// positive-feedback that surges the board then lets the v²-drags slam it back. Taper it to zero
				// over [start,end] on relWaterVelMag (same shape/signal as the pump speed attenuation) so it
				// drives at low speed but self-limits to a cruise. start>=end disables it.
				// See specs/yaw-thrust-speed-attenuation.md.
				const float relWaterVelMag = this->sharedCalculations->relativeWaterVelocityMagnitude;
				const float yawFwdSpeedAtten = (this->yawThrustAttenEnd > this->yawThrustAttenStart)
					? 1.0f - FMath::SmoothStep(this->yawThrustAttenStart, this->yawThrustAttenEnd, relWaterVelMag)
					: 1.0f;
				// FORWARD-drive gating (FR1, specs/flat-water-propulsion-audit.md). This half had no slope
				// gate and no front-face gate, unlike every other propulsion term, so it drove the board on
				// flat water, behind the crest and while inverted — measured at ~99% of net forward force off
				// the face. Gate it the same two ways passive slope thrust is gated, as a FLOOR rather than a
				// switch so a beginner board can keep some off-face drive (yawFwdOffFaceFloor, per board).
				//
				// Deliberately NOT applied to the anti-slip side component below: slip-into-turn redirection
				// is what this coefficient exists for, and it must keep working wherever the board is.
				float yawFwdGate = 1.0f;
				if (this->sharedCalculations)
				{
					const float slopeGateFwd = FMath::SmoothStep(
						this->yawFwdSlopeGateMin,
						this->yawFwdSlopeGateMin + FMath::Max(0.001f, this->yawFwdSlopeGateWidth),
						this->sharedCalculations->boardWideSlopeSin);

					// Same front-face test as slope thrust: the wave always travels one world direction, so the
					// rideable face's fall-line points a fixed way. Full drive on the front face, off behind the
					// crest. Zero vector disables the gate, matching slopeThrustFrontFaceDir's own convention.
					float frontGateFwd = 1.0f;
					if (!this->slopeThrustFrontFaceDir.IsNearlyZero())
					{
						const FVector slopeDownFwd = (this->sharedCalculations->otherHalfSharedCalculations)
							? 0.5f * (this->sharedCalculations->waveSlopeDownVec
							          + this->sharedCalculations->otherHalfSharedCalculations->waveSlopeDownVec)
							: this->sharedCalculations->waveSlopeDownVec;
						const FVector slopeDownHorizFwd = FVector(slopeDownFwd.X, slopeDownFwd.Y, 0.0f).GetSafeNormal();
						const float frontAlignFwd = FVector::DotProduct(
							slopeDownHorizFwd, this->slopeThrustFrontFaceDir.GetSafeNormal());
						frontGateFwd = FMath::SmoothStep(-0.2f, 0.2f, frontAlignFwd);
					}

					const float floorFwd = FMath::Clamp(this->yawFwdOffFaceFloor, 0.0f, 1.0f);
					yawFwdGate = floorFwd + (1.0f - floorFwd) * slopeGateFwd * frontGateFwd;
					// FR1 (specs/yaw-hydrofoil-flow-direction.md): no forward drive while the board moves
					// seaward into the wave. Multiplies the whole gate, floor included - the off-face floor
					// is a beginner assist for leaving the pocket, not for driving up through the crest.
					yawFwdGate *= calcWithWaveGate();
					// A2 (specs/broken-wave-no-consequences.md): no face drive in the whitewater. The
					// data shows a bore front as a 0.3 face; only the foam knows it is broken.
					yawFwdGate *= calcBrokenDriveScale();
				}
				// antiSlipForceScale dials down the anti-slip *side* component (the carve-grip redirect takes
				// over slip-resistance); the forward (carve-coupling) component gets the speed attenuation and
				// the FR1 face gate.
				const float yawThrustMag = this->yawHydrofoilCoefficient
				                         * v2InBottomPlane
				                         * absSinSlip
				                         * wettedForce
				                         * this->effectiveWaterHeight;
				// Propulsion budget (FR6) governs the FORWARD half only. The anti-slip side component
				// is the slip-into-turn redirect (R2's grip), not propulsion, and compressing it would
				// trade handling for a speed number — so it is left at full strength.
				const float yawFwdMag = yawThrustMag * absSinSlip * yawFwdSpeedAtten * yawFwdGate;
				float yawFwdScale = 1.0f;
				if (this->sharedCalculations)
				{
					this->sharedCalculations->RegisterForwardDrive(yawFwdMag); // raw demand, see waveMassThrust
					yawFwdScale = this->sharedCalculations->GetPropulsionScale();
				}
				const FVector yawThrustDir = antiSlipDir * cosSlip * this->antiSlipForceScale * yawThrustMag
				                           + forwardsUnit * yawFwdMag * yawFwdScale;
				yawForce = yawThrustDir;

				if (debugLogThrust)
				{
					const float antiSlipMag = yawThrustMag * cosSlip * this->antiSlipForceScale;
					const float fwdMag      = yawThrustMag * absSinSlip * yawFwdSpeedAtten;
#if WITH_EDITOR
					const FString LabelForLog = GetActorLabel();
#else
					const FString LabelForLog = TEXT("<no-label>");
#endif
					UE_LOG(LogSurf, Warning, TEXT("Yaw Hydrofoil [name=%s label=%s] - sinSlip(boardWide): %.3f, cosSlip: %.3f, v²InBottom(boardWide): %.1f, coef: %.3f, amountWetted: %.3f, amountUnderWater: %.3f, effectiveH: %.2f, relWaterVel: %.0f, speedAtten: %.2f, fwdGate: %.2f (withWave: %.2f) -> mag: %.2f (antiSlip=%.2f fwd=%.2f), force: (%.1f, %.1f, %.1f)"),
						*GetName(), *LabelForLog, sinSlip, cosSlip, v2InBottomPlane, this->yawHydrofoilCoefficient, amountWetted, amountUnderWater, this->effectiveWaterHeight, relWaterVelMag, yawFwdSpeedAtten, yawFwdGate, calcWithWaveGate(),
						yawThrustMag, antiSlipMag, fwdMag * yawFwdGate,
						yawForce.X, yawForce.Y, yawForce.Z);
				}

				if (debugDrawAny && !yawForce.IsNearlyZero())
				{
					DrawDebugCone(
						GetWorld(),
						GetActorLocation(),
						yawForce,
						yawForce.Size() * debugForceDrawScale,
						FMath::DegreesToRadians(1),
						FMath::DegreesToRadians(1),
						50,
						FColor::Purple,
						false,
						0.1f);
				}
			}
		}
	}

	// === PASSIVE SLOPE THRUST ===
	// Gravity-down-the-wave-face drive, redirected along board.forwards (the fins/rails convert the
	// down-slope pull into forward motion). Magnitude = projection of the BOARD-WIDE down-slope gravity
	// onto the forward axis, applied along that axis. Always-on (independent of turnGate) — the hydrofoil
	// forwardsThrust above stays as the intent/pump-gated booster. Restores the passive drive lost when
	// waveSlopeGravity was retired, so a neutrally-trimmed board holds speed down the line instead of
	// coasting to a stop and washing through. Board-wide slope avoids per-actor yaw noise; gated on
	// amountWetted (hull engaged), NOT the sigmoid-squashed amountUnderWater. A minimum-slope deadzone
	// (slopeThrustMinSlopeSin) keeps it OFF on near-flat water between waves / in the trough — without it
	// the strong coefficient flung a slow board around on any residual slope. On a real face the projection
	// self-handles direction: nose pointed up the face (dot < 0) → decelerates, never pushes up the wave.
	// See specs/passive-slope-thrust.md.
	FVector slopeThrustForce = FVector::ZeroVector;
	if (this->slopeThrustCoefficient > 0.0f && this->sharedCalculations
		&& this->sharedCalculations->boardWideSlopeSin > this->slopeThrustMinSlopeSin)
	{
		// Deadzone ramp: 0 below the min-slope threshold (flat/trough), smoothly to full over a small band.
		// This is what enforces "no drive on near-flat water between waves" — the bug from the un-gated term.
		const float slopeGate = FMath::SmoothStep(
			this->slopeThrustMinSlopeSin,
			this->slopeThrustMinSlopeSin + 0.06f,
			this->sharedCalculations->boardWideSlopeSin);
		const FVector boardWideSlopeDown = (this->sharedCalculations->otherHalfSharedCalculations)
			? 0.5f * (this->sharedCalculations->waveSlopeDownVec
			          + this->sharedCalculations->otherHalfSharedCalculations->waveSlopeDownVec)
			: this->sharedCalculations->waveSlopeDownVec;
		const FVector boardFwdUnit = this->sharedCalculations->forwards.GetSafeNormal();
		// Signed projection: sin(slope)·cos(angle between fall-line and board.forwards). Positive when the
		// nose points (partly) down the face → drive; negative when it points up → decelerate.
		const float slopeAlongFwd = FVector::DotProduct(boardWideSlopeDown, boardFwdUnit);
		// Drive-only: clamp off the negative (nose-up-face) side so this propulsion term can never
		// actively push the board *backwards*. At coef=40000 the negative branch produced ~60kN of
		// reverse thrust during a carve up the wave face, braking 1000→100 cm/s ("pushed backwards").
		// Let real gravity/drag handle the climb slowdown; the term only ever adds forward drive now.
		// NOTE: the signed/conservative variant (no clamp) was tried and PAUSED — it didn't fix the
		// (metastable) crossing and the energy work was set aside. See specs/conservative-slope-thrust.md.
		const float slopeAlongFwdDrive = FMath::Max(0.0f, slopeAlongFwd);
		// Fin-redirect coupling (passive-slope-thrust spec / barrel-glide-through-bug.md): the down-slope
		// gravity's LATERAL (board-left) component is the part the fins/rails resist; a working fin
		// redirects it into forward drive. Adding it lets the board gain speed traversing the face
		// (rail ~parallel to the wave) and removes the stall when the nose points up-face — there
		// slopeAlongFwdDrive is 0 but the lateral pull is large, so the board keeps driving forward.
		const FVector boardLeftUnit = this->sharedCalculations->left.GetSafeNormal();
		const float slopeAlongLeft = FVector::DotProduct(boardWideSlopeDown, boardLeftUnit);
		// Deliberately NOT gated by calcWithWaveGate(): this is the drive through a turn (50-115 kN
		// on a player's turn into the wave, measured 2026-09-17) and gating it braked the board hard.
		// It is ~10% of the upstream crest push, which the yaw-hydrofoil gate handles.
		const float redirectDrive = this->slopeThrustFinRedirect * FMath::Abs(slopeAlongLeft);
		// Front-face gate: suppress the drive on the BACK of the crest. The wave always travels the same
		// world direction, so the rideable front face's fall-line points a fixed way (slopeThrustFrontFaceDir).
		// Project the horizontal down-slope onto it: +1 on the front face → full drive, -1 on the back → off.
		// On the back the down-slope reverses, and without this the 40000 coefficient powers the board
		// over/through the wave (net +24k toward the far side). Zero vector disables the gate (legacy).
		// See specs/wave-crossing-deceleration.md.
		float frontGate = 1.0f;
		if (!this->slopeThrustFrontFaceDir.IsNearlyZero())
		{
			const FVector slopeDownHoriz = FVector(boardWideSlopeDown.X, boardWideSlopeDown.Y, 0.0f).GetSafeNormal();
			const float frontFaceAlign = FVector::DotProduct(slopeDownHoriz, this->slopeThrustFrontFaceDir.GetSafeNormal());
			frontGate = FMath::SmoothStep(-0.2f, 0.2f, frontFaceAlign);
		}
		// A2 (specs/broken-wave-no-consequences.md): a bore front is a 0.3 face to the height data
		// and this term drove a board turned back into the whitewater; the foam signal cuts it.
		const float brokenScale = calcBrokenDriveScale();
		slopeThrustForce = boardFwdUnit * (this->slopeThrustCoefficient * wettedForce * slopeGate * frontGate * brokenScale * (slopeAlongFwdDrive + redirectDrive));
		// Propulsion budget (FR6). Already purely along board.forwards, so scaling the vector and
		// scaling its forward component are the same thing. Raw demand reported, compressed force applied.
		if (this->sharedCalculations)
		{
			this->sharedCalculations->RegisterForwardDrive(FVector::DotProduct(slopeThrustForce, boardFwdUnit));
			slopeThrustForce *= this->sharedCalculations->GetPropulsionScale();
		}

		if (debugLogThrust)
		{
#if WITH_EDITOR
			const FString LabelForLog = GetActorLabel();
#else
			const FString LabelForLog = TEXT("<no-label>");
#endif
			UE_LOG(LogSurf, Warning, TEXT("Slope Thrust [name=%s label=%s] - coef: %.1f, amountWetted: %.3f, slopeSin: %.3f, slopeGate: %.3f, frontGate: %.3f, boardWideSlopeDown: (%.3f, %.3f, %.3f) |%.3f|, slopeAlongFwd: %.4f, slopeAlongLeft: %.4f, redirectDrive: %.4f (finRedirect=%.2f) -> force: (%.1f, %.1f, %.1f) mag: %.1f"),
				*GetName(), *LabelForLog, this->slopeThrustCoefficient, amountWetted, this->sharedCalculations->boardWideSlopeSin, slopeGate, frontGate,
				boardWideSlopeDown.X, boardWideSlopeDown.Y, boardWideSlopeDown.Z, boardWideSlopeDown.Size(), slopeAlongFwd, slopeAlongLeft, redirectDrive, this->slopeThrustFinRedirect,
				slopeThrustForce.X, slopeThrustForce.Y, slopeThrustForce.Z, slopeThrustForce.Size());
		}
	}

	// Per-actor saturation: clamp each force vector's magnitude independently. Stand-in for the
	// cavitation/stall behavior real hydrofoils exhibit at extreme AOA or velocity. Direction is
	// preserved; only the magnitude is reduced. See specs/hydrofoil-force-cap.md.
	// (slopeThrustForce is intentionally NOT capped here — it's a gravity drive already bounded by
	// the coefficient, |F| ≤ coef, and is not a v²-scaled hydrofoil term.)
	if (this->maxHydrofoilForceAmount > 0.0f)
	{
		const float pitchMag = pitchForce.Size();
		if (pitchMag > this->maxHydrofoilForceAmount)
		{
			if (debugLogThrust)
			{
				UE_LOG(LogSurf, Warning, TEXT("Pitch Hydrofoil CAPPED [%s] - raw mag: %.2f -> cap: %.2f"),
					*GetName(), pitchMag, this->maxHydrofoilForceAmount);
			}
			pitchForce *= (this->maxHydrofoilForceAmount / pitchMag);
		}
		const float yawMag = yawForce.Size();
		if (yawMag > this->maxHydrofoilForceAmount)
		{
			if (debugLogThrust)
			{
				UE_LOG(LogSurf, Warning, TEXT("Yaw Hydrofoil CAPPED [%s] - raw mag: %.2f -> cap: %.2f"),
					*GetName(), yawMag, this->maxHydrofoilForceAmount);
			}
			yawForce *= (this->maxHydrofoilForceAmount / yawMag);
		}
	}

	// Register sub-components with SharedCalculations for the per-tick yaw torque budget log
	// (gated by SurfDebug "torque" flag, evaluated in SharedCalculations::calculateAll).
	if (this->sharedCalculations)
	{
		const FVector actorPos = GetActorLocation();
		this->sharedCalculations->RegisterAppliedForce(TEXT("bottomHydrofoilPitch"), actorPos, pitchForce);
		this->sharedCalculations->RegisterAppliedForce(TEXT("bottomHydrofoilYaw"),   actorPos, yawForce);
		this->sharedCalculations->RegisterAppliedForce(TEXT("bottomSlopeThrust"),    actorPos, slopeThrustForce);
	}

	// Slope thrust is slope-geometry propulsion (waveSlopeGravity's sibling), not water thrown off
	// a rail — exclude from spray. Measured ~30k planar riding down the line (the dominant spray
	// driver before exclusion, out-spraying the actual carve).
	this->appliedNonSprayForceThisTick += slopeThrustForce;

	return pitchForce + yawForce + slopeThrustForce;
}

/**Calculating shared values, used by a variety of force calculations.
 * Should be called at the beginning of the tick function from BP.
 */

// Called every frame
void AFluidDynamics::Tick(float DeltaTime)
{
	// Tick is called from derived BP classes	
	Super::Tick(DeltaTime);
}

bool AFluidDynamics::waterIsFlowingTowardsSurface()
{
  return this->forwards.Z > -(this->sharedCalculations->relativeWaterVelocityNormalized).Z;
}

bool AFluidDynamics::waterIsFlowingForwards(){
	return (this->sharedCalculations->relativeWaterVelocityNormalized | this->forwards) > 0;
}

void AFluidDynamics::enqueForce(FVector forceToEnque, int number_of_chunks)
{
	ForceQueueManager::EnqueueForce(ForceQueue, forceToEnque, number_of_chunks);
}

TArray<FVector> AFluidDynamics::getEnquedForces()
{
	return ForceQueueManager::GetEnqueuedForces(ForceQueue);
}

void AFluidDynamics::applyForceAsImpulse(FVector force, float DeltaTime)
{
	if (!this->surfboardMesh || force.ContainsNaN() || force.IsNearlyZero())
	{
		return;
	}
	// Rails intro: the force was still COMPUTED (appliedForceThisTick and every derived value
	// upstream are live), it just doesn't move a kinematically-driven board.
	if (SurfRails::AreForcesSuppressed())
	{
		this->appliedForceThisTick += force;
		return;
	}
	// Clamp DeltaTime so a startup hitch (PIE-first-tick can span hundreds of ms)
	// can't multiply force into a launch-the-board impulse. 33 ms = 30 Hz floor;
	// normal-FPS gameplay is well under this so the clamp is inert in steady state.
	const float clampedDt = FMath::Min(DeltaTime, 0.033f);
	const FVector impulse = force * clampedDt;
	this->surfboardMesh->AddImpulseAtLocation(impulse, GetActorLocation(), NAME_None);
	this->appliedForceThisTick += force;
}

void AFluidDynamics::applyDragAsImpulse(float DeltaTime)
{
	const FVector dragForce = calcDragForce();
	applyForceAsImpulse(dragForce, DeltaTime);
	// Option B: apply the roll-producing wave-mass flow drag (stashed by calcDragForce) at the CoM
	// longitudinal axis instead of the actor — kills its roll torque while keeping the front/back pitch.
	// See specs/wave-mass-drag-torque-decoupling.md.
	// Low-pass the wave-mass FLOW push (waveMassSmoothingTau): EMA smooths its flickering spikes on top of the
	// yaw-thrust speed attenuation — both are needed for the smooth glide (see specs/yaw-thrust-speed-attenuation.md).
	// Persistent EMA state; tau <= 0 passes the raw force through.
	if (this->waveMassSmoothingTau > 0.0f)
	{
		const float alpha = FMath::Clamp(DeltaTime / this->waveMassSmoothingTau, 0.0f, 1.0f);
		this->SmoothedWaveMassForce = FMath::Lerp(this->SmoothedWaveMassForce, this->PendingWaveMassForce, alpha);
	}
	else
	{
		this->SmoothedWaveMassForce = this->PendingWaveMassForce;
	}
	applyWaveMassForceAsImpulse(this->SmoothedWaveMassForce, DeltaTime, this->waveMassFlowYawDecouple);
	applyWaveMassForceAsImpulse(this->PendingWavePenetrationForce, DeltaTime, this->wavePenetrationYawDecouple);
	// Synthesized lip impact (nose actor; stashed by calcDragForce). Same option-C path: linear at
	// the CoM-axis nose arm + pitch-kept torque, roll/yaw fully stripped — the falling lip slams the
	// nose down without injecting roll/yaw chaos. Unsmoothed on purpose: the impact IS impulsive.
	// See specs/lip-impact.md.
	if (!this->PendingLipImpactForce.IsNearlyZero())
	{
		applyWaveMassForceAsImpulse(this->PendingLipImpactForce, DeltaTime, 1.0f);
		if (this->sharedCalculations)
		{
			this->sharedCalculations->RegisterAppliedForceWithTorque(TEXT("lipImpact"),
				this->PendingLipImpactForce, getWaveMassKeptTorque(this->PendingLipImpactForce, 1.0f));
		}
	}
}

// World point where the wave-mass flow drag is applied: the actor position projected onto the board's
// forward axis through the centre of mass (zeroes the lateral AND vertical moment arms → no roll/keel),
// lerped from the actor by waveMassRollDecouple (0 = legacy at-actor, 1 = fully on the CoM axis).
FVector AFluidDynamics::getWaveMassApplyPoint() const
{
	const FVector actorPos = GetActorLocation();
	if (!this->surfboardMesh || !this->sharedCalculations)
	{
		return actorPos;
	}
	const FVector com = this->surfboardMesh->GetCenterOfMass();
	const FVector fwd = this->sharedCalculations->forwards.GetSafeNormal();
	if (fwd.IsNearlyZero())
	{
		return actorPos;
	}
	const FVector projected = com + FVector::DotProduct(actorPos - com, fwd) * fwd;
	return FMath::Lerp(actorPos, projected, FMath::Clamp(this->waveMassRollDecouple, 0.0f, 1.0f));
}

FVector AFluidDynamics::getWaveMassKeptTorque(const FVector& force, float yawDecouple) const
{
	if (!this->surfboardMesh || !this->sharedCalculations)
	{
		return FVector::ZeroVector;
	}
	const FVector com = this->surfboardMesh->GetCenterOfMass();
	// Roll is already removed by the apply point sitting on the CoM forward axis; this cross product carries
	// the surviving pitch + yaw (plus any residual roll when waveMassRollDecouple < 1).
	const FVector fullTorque = FVector::CrossProduct(getWaveMassApplyPoint() - com, force);
	const float yawDec = FMath::Clamp(yawDecouple, 0.0f, 1.0f);
	if (yawDec <= 0.0f)
	{
		return fullTorque;  // legacy option B: keep pitch + yaw
	}
	const FVector upN = this->sharedCalculations->up.GetSafeNormal();
	const FVector yawComponent = FVector::DotProduct(fullTorque, upN) * upN;
	return fullTorque - yawDec * yawComponent;  // option C: strip yaw, keep pitch
}

void AFluidDynamics::applyWaveMassForceAsImpulse(FVector force, float DeltaTime, float yawDecouple)
{
	if (!this->surfboardMesh || force.ContainsNaN() || force.IsNearlyZero())
	{
		return;
	}
	const float clampedDt = FMath::Min(DeltaTime, 0.033f);
	const FVector impulse = force * clampedDt;
	// Split AddImpulseAtLocation into its linear + angular parts so the angular part can drop the yaw axis
	// (spec option C). At yawDecouple = 0 this is exactly AddImpulseAtLocation(impulse, applyPoint).
	if (SurfRails::AreForcesSuppressed())
	{
		return;
	}
	this->surfboardMesh->AddImpulse(impulse);
	this->surfboardMesh->AddAngularImpulseInRadians(getWaveMassKeptTorque(force, yawDecouple) * clampedDt);
	this->appliedForceThisTick += force;
	// Wave-mass forces are bulk water-carry (the wave pushing the board), not sheet-producing
	// contact forces — spray consumers subtract them. See appliedNonSprayForceThisTick.
	this->appliedNonSprayForceThisTick += force;
}

void AFluidDynamics::applyLiftAsImpulse(float DeltaTime)
{
	const FVector liftForce = calcLiftForce();
	applyForceAsImpulse(liftForce, DeltaTime);
	// railLift (stashed by calcLiftForce) applied at CoM height — kills its keel-roll, keeps grip + yaw.
	applyRailLiftAsImpulse(this->PendingRailLiftForce, DeltaTime);
}

// Application point that removes the BOARD-UP component of the moment arm from CoM (the keel arm), so a
// sideways force here makes no roll torque, while keeping the board-forward/left arms (so its yaw
// survives). Must be board-up, NOT world-Z: once the board rolls, world-Z != board-up and a world-Z
// zeroing leaves the keel-roll intact. Lerped by railLiftRollDecouple (0 = at-actor, 1 = full decouple).
FVector AFluidDynamics::getComHeightApplyPoint() const
{
	const FVector actorPos = GetActorLocation();
	if (!this->surfboardMesh || !this->sharedCalculations)
	{
		return actorPos;
	}
	const FVector boardUp = this->sharedCalculations->up.GetSafeNormal();
	if (boardUp.IsNearlyZero())
	{
		return actorPos;
	}
	const FVector relPos = actorPos - this->surfboardMesh->GetCenterOfMass();
	const float k = FMath::Clamp(this->railLiftRollDecouple, 0.0f, 1.0f);
	return actorPos - k * FVector::DotProduct(relPos, boardUp) * boardUp;
}

void AFluidDynamics::applyRailLiftAsImpulse(FVector force, float DeltaTime)
{
	if (!this->surfboardMesh || force.ContainsNaN() || force.IsNearlyZero())
	{
		return;
	}
	if (SurfRails::AreForcesSuppressed())
	{
		this->appliedForceThisTick += force;
		return;
	}
	const float clampedDt = FMath::Min(DeltaTime, 0.033f);
	this->surfboardMesh->AddImpulseAtLocation(force * clampedDt, getComHeightApplyPoint(), NAME_None);
	this->appliedForceThisTick += force;
}

void AFluidDynamics::applyThrustAsImpulse(float DeltaTime, FColor debugColor)
{
	const FVector thrustForce = calcThrustForce(debugColor);
	applyForceAsImpulse(thrustForce, DeltaTime);
}

void AFluidDynamics::setup()
{
	// Refresh shared-by-design coefficients from the tuning subsystem each tick
	// so live edits in the in-game tuner take effect on the next physics step.
	RefreshFromTuningSubsystem();

	// New tick, new force sum: the spray accumulators cover exactly one setup()-to-setup() span.
	this->appliedForceThisTick = FVector::ZeroVector;
	this->appliedNonSprayForceThisTick = FVector::ZeroVector;

	++debugFrameCounter; // drives shouldDebugLog() throttling

	auto upLocal = FVector(0.0, 0.0, 1.0);
	this->up = GetTransform().TransformVector(upLocal);

	this->left = this->sharedCalculations->left;

	// calculate this to include the extra cos, otherwise there is no rocker and the board will not turn.
	auto forwardsLocal = FVector(0.0, 1.0, 0.0);
	this->forwards = GetTransform().TransformVector(forwardsLocal);

	// Use the TRUE-cosine variant: the fin drag perpFactor and fin lift sinSlip both read
	// this->cosYawAngleOfAttack and need a real angle of attack, not the ~0.2× actor-scaled
	// cosYawAngleOfAttackLeft. (slipGate at the bottom-thrust block reads this too, but it is
	// logged only.) See specs/fin-force-normalization.md.
	this->cosYawAngleOfAttack = this->sharedCalculations->cosYawAngleOfAttackLeftN;
	if (this->side == ESide::VE_Down)
	{
		// Per-actor projection: use this surface's own left vector instead of the shared one,
		// so rocker curvature is reflected in the angle of attack.
		// Normalize localLeft before using it as a plane normal -- VectorPlaneProject calls
		// ProjectOnToNormal internally and assumes a unit-length normal.
		FVector localLeft = GetTransform().TransformVector(FVector(-1.0, 0.0, 0.0)).GetSafeNormal();
		FVector localRelWaterVelProjLeft = FVector::VectorPlaneProject(
			this->sharedCalculations->relativeWaterVelocityNormalized, localLeft).GetSafeNormal();

		this->cosPitchAngleOfAttack = this->forwards | localRelWaterVelProjLeft;
		this->sinPitchAngleOfAttack = this->up | localRelWaterVelProjLeft;

		if ((debugThrust || SurfDebug::ShouldDebug(this, TEXT("thrust"))) && shouldDebugLog())
		{
			UE_LOG(LogSurf, Warning, TEXT("=== ANGLE CALCULATION %s ==="), *GetName());
			UE_LOG(LogSurf, Warning, TEXT("  this->up: (%.3f, %.3f, %.3f)"), this->up.X, this->up.Y, this->up.Z);
			UE_LOG(LogSurf, Warning, TEXT("  this->forwards: (%.3f, %.3f, %.3f)"), this->forwards.X, this->forwards.Y, this->forwards.Z);
			UE_LOG(LogSurf, Warning, TEXT("  localRelWaterVelProjLeft: (%.3f, %.3f, %.3f)"),
				localRelWaterVelProjLeft.X, localRelWaterVelProjLeft.Y, localRelWaterVelProjLeft.Z);
			UE_LOG(LogSurf, Warning, TEXT("  DOT PRODUCT sinPitchAngle = up · relWaterVel = %.4f"), this->sinPitchAngleOfAttack);
			UE_LOG(LogSurf, Warning, TEXT("  DOT PRODUCT cosPitchAngle = fwd · relWaterVel = %.4f"), this->cosPitchAngleOfAttack);
		}
	}
	else
	{
		// The pitch for the Rails and fins are the same as the roll for the surfboard
		// NOTE: If I also add FD:s to the tail, this has to be changed!
		this->cosPitchAngleOfAttack = this->sharedCalculations->rollCos;
		this->cosRollAngleOfAttack = this->sharedCalculations->cosPitchAngleOfAttack;
	}

	// Sample the wave surface (height + normal) at this actor's (X,Y). The combined
	// effectiveWaterHeight scales every per-surface drag/lift/thrust formula below.
	// calculateWaveLocationAndNormalAuto resolves the tile-adjusted frame internally
	// (do not call waveHeightAndNormal with WaterController->CurrentFrame directly).
	if (this->sharedCalculations && this->sharedCalculations->waveVelocity)
	{
		const FVector samplePos = GetActorLocation();
		const TArray<FVector> sample =
			this->sharedCalculations->waveVelocity->calculateWaveLocationAndNormalAuto(samplePos);
		const float waveZ = (float)sample[0].Z;
		this->waterColumnAbove = FMath::Max(0.0f, waveZ - (float)samplePos.Z);
		// Keep the SIGN the clamp above discards: how far this surface is CLEAR of the water. Without it
		// there is no way to tell a barely-submerged actor (which must keep its trough baseline) from an
		// airborne one (which must have no water forces at all). See specs/airborne-force-gating.md.
		this->surfaceClearance = (float)samplePos.Z - waveZ;

		// sin of the wave-surface slope: 0 on flat water, ~1 on a vertical wall.
		// Robust against numerical drift in the sampled normal.
		const float normalZ = (float)sample[1].Z;
		this->slopeSin = FMath::Sqrt(FMath::Max(0.0f, 1.0f - normalZ * normalZ));

		// Per-actor water VELOCITY at this actor's position (world). Same call pattern + transform
		// as the SC sampling (same tile-adjusted frame, data-space velocity rotated by the wave
		// actor's quat, same temporal smoothing) — blend=1 differs from the SC value only by WHERE
		// it samples. Consumed by the contact forces via perActorWaterVelocityBlend.
		// See specs/per-actor-water-velocity.md.
		FVector rawActorVel = this->sharedCalculations->waveVelocity->GetActorQuat().RotateVector(
			this->sharedCalculations->waveVelocity->calculateWaveVelocity(
				samplePos, this->sharedCalculations->lastTileAdjustedFrame));
		if (this->sharedCalculations->enableVelocitySmoothing && !this->previousActorWaterVelocity.IsZero())
		{
			rawActorVel = FMath::Lerp(rawActorVel, this->previousActorWaterVelocity,
				this->sharedCalculations->velocitySmoothingFactor);
		}
		this->actorWaterVelocity = rawActorVel;
		this->previousActorWaterVelocity = rawActorVel;
	}
	else
	{
		this->waterColumnAbove = 0.0f;
		this->slopeSin = 0.0f;
		this->actorWaterVelocity = FVector::ZeroVector;
		// No wave to sample: treat the surface as in contact, so this degenerate path keeps its
		// existing baseHeight-only behaviour rather than silently losing all its forces.
		this->surfaceClearance = 0.0f;
	}

	// CONTACT GATE. 1 while any of this surface is in the water, fading to airborneForceScale once it
	// is clear by airborneFadeDistance. Because a submerged actor has clearance <= 0 it gates to
	// exactly 1, so the trough baseline that baseHeight exists to provide is untouched (AC2) and
	// airborneForceScale = 1 reproduces the old behaviour bit for bit. Smoothstep, the same shape as
	// actorWetted below, so there is no force discontinuity at the waterline.
	// See specs/airborne-force-gating.md.
	{
		const float clearFrac = (this->airborneFadeDistance > 0.0f)
			? FMath::Clamp(this->surfaceClearance / this->airborneFadeDistance, 0.0f, 1.0f)
			: ((this->surfaceClearance > 0.0f) ? 1.0f : 0.0f);
		const float clearSmooth = clearFrac * clearFrac * (3.0f - 2.0f * clearFrac);
		this->waterContactGate = FMath::Lerp(
			1.0f, FMath::Clamp(this->airborneForceScale, 0.0f, 1.0f), clearSmooth);
	}

	// Gated at the source: 23 sites in this file multiply by effectiveWaterHeight, and every one of
	// them means "this much water is here to push on". One multiply keeps that one meaning true.
	this->effectiveWaterHeight = this->waterContactGate * FMath::Min(
		this->baseHeight + this->waterColumnAbove + this->slopeSin * this->slopeHeight,
		this->maxEffectiveWaterHeight);


	// PER-ACTOR wetting from this actor's own depth below the surface. Replaces the dead per-SC
	// amountWetted (pegged at 1.0; can't carry left/right). Smoothstep so it's sharp; the transition
	// distance must be wide enough not to peg on the bottom actors' resting depth. Because waterColumnAbove
	// is sampled at this actor's (X,Y,Z), it differs between the submerged and raised rail under roll —
	// the asymmetry that lets the bottom forces right the board. See specs/per-actor-wetting.md.
	if (this->wettedTransitionDistance > 0.0f)
	{
		const float t = FMath::Clamp(this->waterColumnAbove / this->wettedTransitionDistance, 0.0f, 1.0f);
		this->actorWetted = t * t * (3.0f - 2.0f * t);
	}
	else
	{
		this->actorWetted = (this->waterColumnAbove > 0.0f) ? 1.0f : 0.0f;
	}

	// surf.debug.flags=contact: per-surface contact state, so an airborne window can be read off the
	// log per ACTOR rather than inferred from the board-midline SC sample.
	// Flag-only (not label-gated): the boards spawned by -BoardInTests do not carry the hand-placed
	// actor labels surf.debug.actors matches on, so a label filter silently prints nothing.
	if (SurfDebug::IsFlagSet(TEXT("contact")) && shouldDebugLog())
	{
		UE_LOG(LogSurf, Warning,
			TEXT("CONTACT [%s] clearance=%+.1f gate=%.3f effH=%.2f (col=%.2f slopeSin=%.3f) wetted=%.3f"),
			*GetName(), this->surfaceClearance, this->waterContactGate, this->effectiveWaterHeight,
			this->waterColumnAbove, this->slopeSin, this->actorWetted);
	}
}

