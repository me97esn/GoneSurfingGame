// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/KismetStringLibrary.h"
#include "WaveHeight.h"
#include "WaveGeometrySubsystem.h"
#include "GameFramework/Actor.h"
#include <cmath> // For sin() and M_PI
#define _USE_MATH_DEFINES
#include <math.h>
#include "SharedCalculations.generated.h"

class AWeightDistribution;
class USurfTuningSubsystem;

UCLASS()
class GONESURFING_API ASharedCalculations : public AActor
{
	GENERATED_BODY()

public:
	// Sets default values for this actor's properties
	ASharedCalculations();

	FName currentFrame = UKismetStringLibrary::Conv_StringToName("");

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FVector relativeWaterVelocity = FVector(0, 0, 0);

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FVector relativeWaterVelocityLocalSpace = FVector(0, 0, 0);

	// Board speed (cm/s) required to start planing. Overwritten at runtime by
	// Tuning->PlaningStartsVelocity; this is only the fallback when no tuning
	// subsystem is present, kept in sync with the tuning default (300).
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float PlaningStartsVelocity = 300.0f;

	// Hysteresis: once planing, board keeps planing until speed drops below this.
	// Overwritten at runtime by Tuning->PlaningStopsVelocity (default 200).
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float PlaningStopsVelocity = 200.0f;

	// Board speed (cm/s) at which planing reaches MaxPlaning. Overwritten at
	// runtime by Tuning->PlaningFullVelocity (default 400).
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float PlaningFullVelocity = 400.0f;

	// Ceiling for AmountPlaning (0..1). Overwritten at runtime by
	// Tuning->MaxPlaning (default 0.8).
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float MaxPlaning = 0.8f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float AmountPlaning = 0.0f;

	// Time in seconds for planing to decay from max to zero when forces drop below threshold
	// Higher values = longer gliding after thrust stops
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planing")
	float PlaningDecayTime = 2.0f;

	// Wave-face pitch alignment: restoring torque that pitches the board's bottom to follow the local
	// wave face (drives waveRelativePitchSin -> 0), so the nose rides up a steepening face instead of
	// driving the flat hull through it. Applied as an angular-acceleration torque about board.left in
	// calculateAll, split across the two SC actors so it isn't double-applied. 0 = disabled (legacy).
	// This is the P (proportional) term of the surface-tracking PD; the D term is the surface-relative
	// asymmetric engine damping. See specs/surface-relative-pitch-damping.md, specs/wave-face-pitch-alignment.md.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PitchAlign")
	float pitchAlignCoefficient = 10.0f;

	// Min board-wide slopeSin before pitch alignment engages (deadzone: off on near-flat water). ~0.12 ≈ 7°.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PitchAlign")
	float pitchAlignMinSlopeSin = 0.12f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	AWaveHeight* waveVelocity;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	AActor* Surfboard;

	/** The OTHER half-board's ASharedCalculations actor. Wired in ASurfboardUtils::BeginPlay
	 *  so each SC knows its sibling. Used by the yaw hydrofoil to compute a board-wide slip
	 *  signal rather than each half-board's local one — see
	 *  specs/yaw-hydrofoil-board-wide-slip.md and specs/per-actor-vs-board-wide-sampling.md. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly)
	ASharedCalculations* otherHalfSharedCalculations = nullptr;

	/** Board's AWeightDistribution actor. Wired in ASurfboardUtils::BeginPlay alongside
	 *  otherHalfSharedCalculations. Used by the bottom-hydrofoil turn-gate (in
	 *  AFluidDynamics::calcThrustForce) to read the surfer's *intent* — `amountToTheRight`
	 *  deviation from 0.5 — and require commanded weight-shift before the forward thrust
	 *  fires. This distinguishes "wave is pushing me sideways" (no weight shift, gate closed)
	 *  from "I'm actively carving" (commanded shift, gate opens). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly)
	AWeightDistribution* weightDistribution = nullptr;

	/** Magnitude of lateral weight shift from centered. 0 = centered, 0.5 = fully shifted
	 *  to one rail. Computed each tick from `weightDistribution->amountToTheRight`. Read by
	 *  the bottom-hydrofoil turn-gate. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly)
	float lateralShift = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	bool debug = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	bool debugDrawAbsoluteVelocity = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	bool debugDrawRelativeVelocity = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	bool debugShowVelocityText = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	float velocityDebugScale = 0.1f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	bool debugPlaning = false;

	/** Per-tick log of componentVelocityMagnitude, AmountPlaning, amountUnderWater. Frame-throttled by debugLogEveryNthTick. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	bool debugStateLog = false;

	/** Throttle for the debug UE_LOG calls — only log on every Nth tick. 1 = every tick, 30 ≈ 2 Hz, 60 = 1 Hz. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug", meta=(ClampMin = "1"))
	int32 debugLogEveryNthTick = 1;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float relativeWaterVelocityMagnitude = 0;

	float componentVelocityMagnitude = 0;

	FVector absoluteWaterVelocityNormalized = FVector(0, 0, 1.0);
	float absoluteWaterVelocityMagnitude = 0;
	FVector relativeWaterVelocityNormalized	= FVector(0, 0, 1.0);
	FVector relativeWaterVelocityDirectionProjectedLeft;
	FVector relativeWaterVelocityDirectionProjectedForwards;
	FVector relativeWaterVelocityDirectionProjectedUp;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FVector absoluteWaterVelocity = FVector(0, 0, 0);

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FVector componentVelocity = FVector(0, 0, 0);

	// Temporal velocity smoothing to reduce striping artifacts
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Velocity Smoothing")
	bool enableVelocitySmoothing = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Velocity Smoothing", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float velocitySmoothingFactor = 0.3f;  // 0 = no smoothing, 1 = full smoothing (very laggy)

private:
	FVector previousAbsoluteWaterVelocity = FVector(0, 0, 0);
	int32 debugFrameCounter = 0;

public:

	FVector up = FVector(0, 0, 1.0);
	FVector forwards = FVector(0, 1.0, 0);
	FVector left = FVector(-1.0, 0, 0); // matches leftLocal = (-1,0,0) used in calculateAll
	TArray<FVector> PlaningForces;

	/**
	 * 
	 * 90 degrees angle to the left: 1.0 (0 degrees pitch).
	 * 45 degrees angle to the left: 1.0
	 * 0 degrees angle: 1.0.
	 * 
	 * 
	 * 45 degrees tilted back: 0.71 (45 degrees angle).
	 * 45 degrees tiltet forwards: 0.71 (45 degrees angle).
	 * 
	 * Note that this value might be positive or negative regardless if the board is tilted anterior or posterior.
	 *  */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly)
	double cosPitchAngleOfAttack = 0.0f;

	/**
	 * 
	 * No pitch regardless of yaw, which is correct. TODO: Does roll effect the pitch?
	 * 
	 * 45 degrees tilted back: 0.71 (45 degrees angle).
	 * 45 degrees tiltet forwards: 0.71 (45 degrees angle).
	 * Note that this value might be positive or negative regardless if the board is tilted anterior or posterior.
	 * 
	 *  */	
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly)
	double pitchSinAngleOfAttack = 0.0f;

	/**
	 * This is only roll of the surfboard, regardless of direction of the relative water velocity.
	 * The water has no roll, only the board.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly)
	double rollCos = 0.0f;

	/**
	 * This is only roll of the surfboard, regardless of direction of the relative water velocity.
	 * The water has no roll, only the board.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly)
	double rollSin = 0.0f;

	/**
	 * Roll of the surfboard relative to the LOCAL WAVE SURFACE (not gravity).
	 * Zero when the board is parallel to the wave face. Used by hydrodynamic
	 * forces that depend on rail-depth differential into the water (e.g., the
	 * lateral carving force in FluidDynamics).
	 *
	 * cos = 1, sin = 0 means board is wave-aligned (no roll relative to surface).
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly)
	double waveRelativeRollCos = 1.0f;

	/** See waveRelativeRollCos. Sign of sin indicates which rail is dipped relative to the wave surface. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly)
	double waveRelativeRollSin = 0.0f;

	/** Pitch analog of waveRelativeRollSin: board pitch about board.left measured against the local wave
	 *  surface. 0 = bottom parallel to the face (following it); sign indicates nose-high vs nose-low
	 *  relative to the face. Drives the wave-face pitch-alignment torque. See specs/wave-face-pitch-alignment.md. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly)
	double waveRelativePitchSin = 0.0f;

	/** See waveRelativePitchSin. cos = 1, sin = 0 means the board's bottom is parallel to the wave face. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly)
	double waveRelativePitchCos = 1.0f;

	/**
	 * Board roll about its forward axis measured against the HORIZONTAL plane (world), not the wave.
	 * cos = 1, sin = 0 means the board is level in the world. Unlike waveRelativeRollSin, this is ~0
	 * for a board trimming horizontally down the line on a steep face (its natural neutral attitude),
	 * so it isolates the rider's deliberate lean from the wave's tilt. Drives the lateral-turn carve.
	 * See specs/lateral-turn-world-relative-roll.md.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly)
	double worldRelativeRollCos = 1.0f;

	/** See worldRelativeRollCos. Sign indicates the leaned-to side (about board.forwards, vs horizontal). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly)
	double worldRelativeRollSin = 0.0f;

	/**
	 * Wave-slope downhill vector: world-down projected onto the wave-tangent plane.
	 * Magnitude equals sin(slope_angle), direction is along the wave surface in the
	 * downhill (toward-trough) direction. Zero on flat water.
	 *
	 * Used by FluidDynamics' waveSlopeGravity supplement to apply a per-bottom-actor downhill
	 * force on top of natural UE gravity, and by StateTriggerAutoPilot as a slope-magnitude
	 * metric for state transitions.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly)
	FVector waveSlopeDownVec = FVector::ZeroVector;

	/**
	 * Outward-pointing wave-surface normal in world frame. Falls back to (0,0,1) on flat water.
	 * Backs the derivations of waveSlopeDownVec and waveRelativeRollSin/Cos and is exposed for
	 * any hydrodynamic force that needs the wave-surface orientation directly.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly)
	FVector waveNormal = FVector(0.0, 0.0, 1.0);

	/**
	 * Direction (world) toward the BACK of the wave — i.e. the wave-travel axis; the wave breaks the
	 * opposite way (toward the front face). Used to locate the crest and to sign signedDistanceToCrest.
	 * Default +X (this project's wave travels along +X and breaks toward -X, with +Y down the line). Set
	 * per level if a wave is oriented differently. See specs/wave-crossing-deceleration.md.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave")
	FVector waveBackDirection = FVector(1.0f, 0.0f, 0.0f);

	/**
	 * Cross-shore back axis actually used by the crest scan, resolved once at runtime from the
	 * InfiniteWaveManager tile geometry (perpendicular to the down-line tiling axis). Zero until
	 * resolved; falls back to waveBackDirection if no manager is present. Not user-facing.
	 */
	FVector resolvedWaveBackDirection = FVector::ZeroVector;

	/**
	 * Signed distance (cm) from this SC actor to the wave crest, measured along waveBackDirection.
	 * Positive  => this point is BEHIND the crest (on the back / far side of the wave);
	 * negative  => on the front-face side; ~0 => at the crest.
	 * Found by scanning the wave-surface height along waveBackDirection for the local peak. This is a
	 * front/back-of-crest signal RELATIVE TO THE MOVING WAVE — absolute world position cannot give it
	 * (the wave travels, so the board can be behind the crest while its world X is still decreasing).
	 * Front and back SC report their own value, so a board straddling the crest is visible directly.
	 * Reusable for crossing diagnostics, autopilot state triggers, etc. See specs/wave-crossing-deceleration.md.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly)
	float signedDistanceToCrest = 0.0f;

	/** Set by ASurfboardPawn on the ONE SharedCalculations the assist actually reads, so the assist
	 *  band overlay (AssistDrawBand) is drawn once rather than once per board half. Front and back
	 *  each hill-climb their own crest, and two overlays at two estimates reads as a bug in the band
	 *  rather than as two honest samples. See specs/gradual-control-handoff.md. */
	bool bAssistBandReference = false;

	/** World time the assist band was last drawn. The overlay redraws only as the previous copy is
	 *  about to expire, not every frame — see the draw site for why. */
	float lastAssistBandDrawTime = -1.0e9f;

	/** World position of the nearest wave crest along waveBackDirection (the height peak found by the scan). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly)
	FVector crestWorldPosition = FVector::ZeroVector;

	/**
	 * Board-wide sin of wave slope: sqrt(1 - waveNormal.Z²) sampled at the SC actor's position.
	 * 0 on flat water, ~1 on a vertical wave wall. Differs from per-actor FluidDynamics::slopeSin
	 * in that this is *one* sample for the whole board, used by board-wide-phenomenon forces.
	 * See specs/per-actor-vs-board-wide-sampling.md for when to use this vs per-actor sampling.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly)
	float boardWideSlopeSin = 0.0f;

	/**
	 * Board-wide vertical distance from the SC actor up to the wave surface at the same (X,Y),
	 * clamped to 0. The board-wide analog of FluidDynamics::waterColumnAbove — sampled once at
	 * the SC actor's position rather than per-actor.
	 * See specs/per-actor-vs-board-wide-sampling.md.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly)
	float boardWideWaterColumnAbove = 0.0f;

	/**
	 * 1: water  is flowing in parallell with the board
	 * 0: Water is flowing perpendicular to the board
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly)
	double cosYawAngleOfAttack = 0.0f;

	/**
	 * 
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly)
	double cosYawAngleOfAttackLeft = 0.0f;

	/**
	 * True cosine sibling of cosYawAngleOfAttackLeft: dots the NORMALIZED board-left
	 * with the (unit) projected relative-water-velocity, so this is a real cos(yaw-from-left)
	 * in [-1,1] rather than the ~0.2× actor-scaled cosYawAngleOfAttackLeft. Use this for the
	 * fin terms (fin drag perpFactor, fin lift sinSlip), which need a true angle of attack.
	 * See specs/fin-force-normalization.md.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly)
	double cosYawAngleOfAttackLeftN = 0.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly)
	float mass;

	float amountUnderWater;

	// (Per-SC amountWetted removed — it was pegged at 1.0 and couldn't carry left/right. FluidDynamics
	// now uses a per-actor AFluidDynamics::actorWetted from each actor's own depth. See specs/per-actor-wetting.md.)

	int32 lastTileAdjustedFrame = 0;

protected:
	// The service's read of this actor's position -> brokenGeo / waveZone / distBehindImpact.
	void UpdateBrokenGeo();
	// Whitewater controller (found once via TActorIterator) for the brokenAmount diagnostic.
	TWeakObjectPtr<class AParticleSystemsController> FoamController;
	void UpdateBrokenAmount(float DeltaTime);

	// Called when the game starts or when spawned
	virtual void BeginPlay() override;

	/** Cached pointer to USurfTuningSubsystem, resolved once in BeginPlay.
	 *  The planing-threshold UPROPERTYs are overwritten from it each tick
	 *  (RefreshFromTuningSubsystem) so HUD edits apply live — same pattern
	 *  as AFluidDynamics. */
	UPROPERTY(Transient)
	USurfTuningSubsystem* Tuning = nullptr;

	void RefreshFromTuningSubsystem();

public:	
	// Called every frame
	virtual void Tick(float DeltaTime) override;

	UFUNCTION(BlueprintCallable, Category="Surfing")
	void calculateAll(FString rowName,UPrimitiveComponent *BasePrimComp);
	void AddPlaningForce(FVector force);

	void calculateAndSetAmountUnderWater();
	void calculateAngleOfAttacks(FVector _relativeWaterVelocityNormalized);

	FVector calcRelativeWaterVelocity(UPrimitiveComponent *BasePrimComp, FVector aboluteWaterVelocity);
	FVector calcComponentVelocity(UPrimitiveComponent *BasePrimComp);
	void calculateAmountPlaning(FVector _relativeWaterVelocityLocalSpace);

	// Per-tick force/torque accumulator for the "torque" debug-flag budget log.
	// FluidDynamics actors call RegisterAppliedForce() as they compute each force
	// sub-component. At the start of the NEXT tick's calculateAll(), the previous
	// tick's accumulator is logged under SurfDebug flag "torque" and cleared.
	// Torque is computed around the surfboard mesh CoM (cached at start of tick).
	struct FTorqueContribution {
		FVector totalForce = FVector::ZeroVector;
		FVector totalTorque = FVector::ZeroVector;
		float yawTorque = 0.0f;
		int32 count = 0;
	};
	TMap<FString, FTorqueContribution> torqueBudget;
	FVector cachedCenterOfMass = FVector::ZeroVector;

	void RegisterAppliedForce(const FString& category, const FVector& worldPosition, const FVector& force);

	// === Propulsion budget (FR6, specs/wave-mass-thrust-closing-speed.md) ===
	//
	// Forward drive is limited on the SUM, not per term, because the terms refill each other's gaps
	// (M9: zeroing the largest moved the net by 9%). FluidDynamics actors report the
	// along-board.forwards component of each governed drive via RegisterForwardDrive() as they
	// compute it, and scale that drive by GetPropulsionScale() before applying it.
	//
	// One tick of lag by construction: the total is only known once every actor has run, so the
	// scale served this tick is computed from last tick's total — the same previous-tick pattern the
	// torque budget uses. At 60-90 Hz this is invisible, and it avoids restructuring the force
	// pipeline around a two-pass sum.
	/** Half-step (cm) for deriving the wave normal from the height field instead of the stored
	 *  normals. 0 = use the stored normals. Mirrored from the tuning subsystem each tick. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Waves")
	float waveNormalHeightDelta = 0.0f;

	/** 0..1: the board is in collapsed water — the wave-geometry service's Broken at this actor
	 *  (specs/wave-geometry.md FR4: 0 on the face, in the pocket and over the back; ramping to 1 over
	 *  the first 4 m behind the pocket inside the aging bore band). THE broken-water signal every
	 *  consequence keys on (specs/broken-wave-no-consequences.md A4): the A2 drive cut, the A1 shove,
	 *  the A3 control cut, the score. Comes from the loop clock and the peel model, not from anything
	 *  sampled under the board — slope, water column, water speed and foam density all read a bore
	 *  like a face (M9-M13). 0 when the level has no model. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Waves")
	float brokenGeo = 0.0f;

	/** The service's zone at this actor this tick (Unknown when the level has no model). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Waves")
	EWaveZone waveZone = EWaveZone::Unknown;

	/** cm along the line behind the nearest wave's impact point (+ = up the line of it, on the
	 *  broken side) and cross-shore from it (+ = toward the back). 0 without a model. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Waves")
	float distBehindImpact = 0.0f;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Waves")
	float crossFromImpact = 0.0f;

	/** 0..1: DIAGNOSTIC ONLY, superseded by brokenGeo (specs/broken-wave-no-consequences.md M13: the
	 *  foam point cloud says "broken water NEAR the board", never "the board is IN it" — a board
	 *  carving up into the lip on a clean face has more foam round it than one sitting in the
	 *  whitewater). The sim's foam points within FoamBrokenRadius, thresholded and smoothed (M11).
	 *  Computed only while the `foam` debug flag is set; 0 otherwise, and nothing reads it for
	 *  behaviour. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Waves")
	float brokenAmount = 0.0f;

	/** Raw counts behind brokenAmount this tick (debug / device re-calibration): all round within
	 *  FoamBrokenRadius, and in the half-disc ahead of the motion within FoamAheadRadius. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Waves")
	int32 foamPointsNearby = 0;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Waves")
	int32 foamPointsAhead = 0;

	/** 0..1: how much the board is SURROUNDED by whitewater — foam within FoamBrokenRadius in every
	 *  one of four 90-degree sectors around the direction of motion (4 x the thinnest sector,
	 *  through the FoamBrokenCount knee, smoothed like brokenAmount). Foam on one side only — the
	 *  lip's foam beside the pocket, the broken section a hard turn points the nose at — reads ~0.
	 *  DIAGNOSTIC ONLY like brokenAmount: the A2 drive cut keyed on this until 2026-09-21 and never
	 *  fired (the foam is a thin band; the surround read peaked at 0.12 on a shortboard ride); it
	 *  keys on brokenGeo now. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Waves")
	float brokenSurroundAmount = 0.0f;

	/** Raw per-sector counts behind brokenSurroundAmount (ahead, behind, left, right of the motion),
	 *  within FoamBrokenRadius and within half of it, and the thinnest of the four. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Waves")
	int32 foamPointsThinnestSector = 0;
	int32 foamSectorCounts[4] = { 0, 0, 0, 0 };
	int32 foamSectorCountsNear[4] = { 0, 0, 0, 0 };

	void RegisterForwardDrive(float alongForwards);

	/** Multiplier the governed drive terms apply to themselves this tick. 1.0 = untouched. */
	float GetPropulsionScale() const { return PropulsionScale; }

	/** Recomputes PropulsionScale from the previous tick's board-wide total, then resets the
	 *  accumulator for this tick. Called once per tick per SC from calculateAll(). */
	void RollOverPropulsionBudget(UPrimitiveComponent* BasePrimComp);

	/** Accumulating this tick; snapshotted into PreviousForwardDrive at roll-over. */
	float PendingForwardDrive = 0.0f;
	/** Last tick's total for THIS half of the board. Board-wide = this + otherHalf's. */
	float PreviousForwardDrive = 0.0f;
	/** Board-wide governed drive last tick, in board weights — what the scale was computed from.
	 *  Exposed for the "propulsion" debug line and for the acceptance measurements. */
	float LastGovernedWeights = 0.0f;
	float PropulsionScale = 1.0f;

	// Variant for forces whose APPLIED torque is not (worldPosition-CoM)×force — e.g. the wave-mass flow
	// drag after selective roll/yaw decoupling (spec option C). Pass the linear force and the actual torque
	// so the budget reflects what is really applied rather than the naive point-cross-force.
	void RegisterAppliedForceWithTorque(const FString& category, const FVector& force, const FVector& torque);

};
