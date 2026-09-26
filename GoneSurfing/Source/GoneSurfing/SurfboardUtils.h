// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "FluidDynamics.h"
#include <map>
#include <list>
#include <set>
#include <string>
#include "SharedCalculations.h"
#include "WeightDistribution.h"
#include "SurfboardUtils.generated.h"

class USurfTuningSubsystem;


UCLASS()
class GONESURFING_API ASurfboardUtils : public AActor
{
	GENERATED_BODY()
	
public:	
	// Sets default values for this actor's properties
	ASurfboardUtils();
	std::map<FString, std::list<AFluidDynamics*>> fluidDynamicsPerSurfboard;
	std::set<FString> surfboardNames;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	AWeightDistribution* weightDistribution;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	ASharedCalculations* sharedCalculationsFront;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	ASharedCalculations* sharedCalculationsBack;

	// Nose FluidDynamics actor — its `forwards` carries the board's real (rocker-pitched) orientation,
	// unlike the SharedCalculations actors which are axis-aligned (flat chord). Used as the carve-grip
	// redirect target so the grip follows the rolled, rockered nose. See specs/carve-grip-via-redirect.md.
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	AFluidDynamics* noseFluidDynamics;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	ABuoyancy* buoyancyNoseLeft;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	ABuoyancy* buoyancyNoseRight;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	ABuoyancy* buouyancyTailLeft;
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	ABuoyancy* buoyancyTailRight;
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Surfboard")
	float waveNormalDampingRate = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float SurfboardSidewaysDamping;

	/** Fraction of SurfboardSidewaysDamping that survives with no water contact. 0 = the fix.
	 *  See SurfTuningSubsystem.h for why. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float SidewaysDampingAirScale = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float WorldDownwardsDamping;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float WorldUpwardsDamping;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float SurfboardForwardsDamping;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float AmountUnderWaterEquilibrium = 0.5;

	/** Spawn an AShadowController in BeginPlay if the level has none. The board's shadow casters need
	 *  no per-level setup, so this saves dragging one into every .umap; turn it off (or place a
	 *  controller by hand, which also suppresses the spawn) to configure it explicitly.
	 *  See specs/board-shadow-grounding.md. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shadow")
	bool bAutoSpawnShadowController = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	bool debugDamping = false;

	/** Throttle for the debugDamping UE_LOG block — only log on every Nth tick. 1 = log every tick, 30 ≈ 2 Hz, 60 = 1 Hz. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(ClampMin = "1"))
	int32 debugLogEveryNthTick = 1;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	bool debugComponentVelocity = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float debugDrawDuration = 0.1;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float SurfboardVerticalDamping;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float AngularDampingX;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float AngularDampingY;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float AngularDampingZ;

	// Planing redirect (rocker lift) strength — rotation rate (rad/s) toward the wave up-slope, gated by
	// nose submersion. 0 = off; ~2.0 is the tuned sweet spot. Runtime scratch, overlaid from the tuning
	// subsystem each tick, then pushed to p.Chaos.Solver.PlaningRedirectMaxAngle. See specs/planing-redirect.md.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Damping")
	float PlaningRedirectMaxAngle = 2.0f;

	// One-sided planing redirect scale (1 = symmetric legacy, 0 = never redirect outward motion). Runtime
	// scratch, overlaid from the tuning subsystem each tick, then pushed to
	// p.Chaos.Solver.PlaningRedirectOutwardScale. See specs/pitch-righting-and-redirect-escape.md.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Damping")
	float PlaningRedirectOutwardScale = 1.0f;

	// Crest-proximity fades (cm; 0 = off) for the planing redirect / carve grip gates. Runtime scratch,
	// overlaid from the tuning subsystem each tick, multiplied into the gate CVar feeds. Kept in sync
	// with USurfTuningSubsystem defaults. See specs/pitch-righting-and-redirect-escape.md (Correction).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Damping")
	float PlaningRedirectCrestFade = 600.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Damping")
	float CarveGripCrestFade = 600.0f;

	// Surface-relative pitch righting servo rate (1/s), 0 = off. Runtime scratch, overlaid from the tuning
	// subsystem each tick, pushed contact-gated to p.Chaos.Solver.PitchRightingRate.
	// See specs/pitch-righting-and-redirect-escape.md.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Damping")
	float PitchRightingRate = 0.0f;

	// Carve-grip redirect strength — rotation rate (rad/s) of horizontal velocity toward heading, gated by
	// nose submersion. 0 = off. Runtime scratch, overlaid from the tuning subsystem each tick, then pushed
	// to p.Chaos.Solver.CarveGripRate. See specs/carve-grip-via-redirect.md.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Damping")
	float CarveGripRate = 4.0f;

	// Speed-dependent yaw-rate ceiling (rad/s at/below the knee; 0 = off). Runtime scratch, overlaid from
	// the tuning subsystem each tick; the resolved cap(v) is pushed to p.Chaos.Solver.MaxAngularVelocityZ.
	// See specs/lip-snap-spinout-and-air.md (T1).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Damping")
	float YawRateCapMax = 2.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Damping")
	float YawRateCapSpeedKnee = 200.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Damping")
	float CarveGripTurnUnfadeRate = 1.0f;

	/** 0 = gate the carve grip on nose submersion (original), 1 = on the board-wide contact gate.
	 *  See SurfTuningSubsystem.h for why. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float CarveGripContactBlend = 1.0f;

	/** 0 = only a yaw toward the wave lifts the crest fade (original), 1 = the yaw rate does,
	 *  either direction. See SurfTuningSubsystem.h for why. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float CarveGripTurnUnfadeSymmetric = 1.0f;

	// Whitewater consequences (specs/broken-wave-no-consequences.md A1/A3), overlaid from the tuning
	// subsystem each tick; keyed on the SCs' brokenGeo.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Damping")
	float BrokenDampingRateScale = 10.0f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Damping")
	float BrokenDampingGate = 1.0f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Damping")
	float BrokenYawCapScale = 0.5f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Damping")
	float WhitewaterAlongNoseScale = 0.1f;

	// Wave-carry redirect rate (rad/s): rotate horizontal velocity toward the wave's shoreward travel
	// direction to hold station on the crest. 0 = off. Overlaid from the tuning subsystem, pushed to
	// p.Chaos.Solver.WaveCarryRedirectRate. See specs/wave-carry-redirect.md.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Damping")
	float WaveCarryRedirectRate = 0.0f;

	// Target shoreward speed (cm/s) for the wave-carry self-limiting gate. Overlaid from the tuning subsystem.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Damping")
	float WaveCarryTargetCrossSpeed = 60.0f;

	// Surface-relative pitch damping (see specs/surface-relative-pitch-damping.md). Runtime scratch,
	// overwritten from the tuning subsystem each tick. AwayExtra adds pitch-axis damping when the board
	// rotates away from the local water surface; TowardReduction subtracts it when rotating toward, for
	// faster surface tracking. The engine applies the toward/away sign test; the project feeds the
	// misalignment + contact-gated extras. Replaces AngularDampingYNoseDownExtra.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Damping")
	float AngularDampingYAwayExtra = 0.8f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Damping")
	float AngularDampingYTowardReduction = 0.10f;

	// How strongly tilt-back (weight in tail) reduces yaw damping when the board is planing.
	// Effective reduction = amountTiltingBack × avgAmountPlaning × YawDampingTiltBackInfluence.
	// At avgAmountPlaning=0 (stopped) the reduction is always 0, so a stopped board has full damping.
	// 0.0 = no reduction even while planing; 1.0 = damping can hit 0 at fully-tilted-back & planing.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(ClampMin = "0.0", ClampMax = "1.0"))
	float YawDampingTiltBackInfluence = 0.7f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	bool UseCustomLinearDamping = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	bool UseCustomAngularDamping = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float ClampYVelocityAt = 2000.0f;

	/** Hard ceilings on world-frame linear velocity per axis. Caught by the engine fork's
	 *  Chaos integrator (p.Chaos.Solver.MaxVelocityX/ZUp/ZDown). Each is independent;
	 *  negative values disable that axis's clamp. The Y ceiling lives in ClampYVelocityAt
	 *  for backwards compat. See specs/velocity-ceiling-damping.md. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Damping")
	float MaxVelocityX = 3000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Damping")
	float MaxVelocityZUp = 1000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Damping")
	float MaxVelocityZDown = 2000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Damping")
	float VelocityDampingThreshold = 500.0f;  // cm/s - when to start velocity damping

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Damping")
	float VelocityDampingScale = 0.0005f;  // Scaling factor for velocity-based damping (squared in calculation)

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Damping")
	float MaxVelocityDamping = 0.95f;  // Maximum total forward damping (cap)

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Damping")
	bool UseVelocityDependentDamping = true;  // Feature toggle


private:
	int32 debugFrameCounter = 0;

	/** Cached pointer to the shared tuning subsystem. Resolved once in BeginPlay
	 *  via SurfTuning::Get(). When non-null, BeginPlay and Tick refresh this
	 *  actor's damping UPROPERTYs from the subsystem at the top of each frame, so
	 *  the rest of the function body reads tuning-overlaid values transparently.
	 *  The actor's own UPROPERTYs stay for BP-load compat but are runtime scratch.
	 *  See specs/runtime-tuning.md Phase 1. */
	UPROPERTY(Transient)
	USurfTuningSubsystem* Tuning = nullptr;

	/** Copy damping coefficients from the tuning subsystem onto this actor's
	 *  scratch UPROPERTYs. No-op if Tuning is null. */
	void RefreshFromTuningSubsystem();

protected:
	// Called when the game starts or when spawned
	virtual void BeginPlay() override;

	IConsoleVariable *DampingLocalX = IConsoleManager::Get().FindConsoleVariable(TEXT("p.Chaos.Solver.DampingLocalX"));
	IConsoleVariable *DampingLocalY = IConsoleManager::Get().FindConsoleVariable(TEXT("p.Chaos.Solver.DampingLocalY"));
	IConsoleVariable *DampingLocalZ = IConsoleManager::Get().FindConsoleVariable(TEXT("p.Chaos.Solver.DampingLocalZ"));
	IConsoleVariable *DampingZDown = IConsoleManager::Get().FindConsoleVariable(TEXT("p.Chaos.Solver.DampingZDown"));
	IConsoleVariable *DampingZUp = IConsoleManager::Get().FindConsoleVariable(TEXT("p.Chaos.Solver.DampingZUp"));
	IConsoleVariable *RotationPitch = IConsoleManager::Get().FindConsoleVariable(TEXT("p.Chaos.Solver.RotationPitch"));
	IConsoleVariable *RotationYaw = IConsoleManager::Get().FindConsoleVariable(TEXT("p.Chaos.Solver.RotationYaw"));
	IConsoleVariable *RotationRoll = IConsoleManager::Get().FindConsoleVariable(TEXT("p.Chaos.Solver.RotationRoll"));
	IConsoleVariable *MinimalDampingMultiplierX = IConsoleManager::Get().FindConsoleVariable(TEXT("p.Chaos.Solver.MinimalDampingMultiplierX"));
	IConsoleVariable *MinimalDampingMultiplierY = IConsoleManager::Get().FindConsoleVariable(TEXT("p.Chaos.Solver.MinimalDampingMultiplierY"));
	IConsoleVariable *MinimalDampingMultiplierZ = IConsoleManager::Get().FindConsoleVariable(TEXT("p.Chaos.Solver.MinimalDampingMultiplierZ"));
	IConsoleVariable *AngularDampingXCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("p.Chaos.Solver.AngularDampingX"));
	IConsoleVariable *AngularDampingYCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("p.Chaos.Solver.AngularDampingY"));
	IConsoleVariable *AngularDampingZCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("p.Chaos.Solver.AngularDampingZ"));
	IConsoleVariable *PitchMisalignmentCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("p.Chaos.Solver.PitchMisalignment"));
	// Planing-redirect gate: fed from the NOSE (front SC) amountUnderWater so the rocker-lift redirect is
	// strong when the nose digs into the face and fades as it rides up. See specs/planing-redirect.md.
	IConsoleVariable *PlaningRedirectGateCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("p.Chaos.Solver.PlaningRedirectGate"));
	IConsoleVariable *PlaningRedirectMaxAngleCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("p.Chaos.Solver.PlaningRedirectMaxAngle"));
	// Up-the-face direction (-waveSlopeDownVec, world) the redirect rotates velocity toward.
	IConsoleVariable *PlaningRedirectUpXCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("p.Chaos.Solver.PlaningRedirectUpX"));
	IConsoleVariable *PlaningRedirectUpYCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("p.Chaos.Solver.PlaningRedirectUpY"));
	IConsoleVariable *PlaningRedirectUpZCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("p.Chaos.Solver.PlaningRedirectUpZ"));
	IConsoleVariable *PlaningRedirectOutwardScaleCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("p.Chaos.Solver.PlaningRedirectOutwardScale"));
	IConsoleVariable *PitchRightingRateCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("p.Chaos.Solver.PitchRightingRate"));
	// Carve grip: rate + nose-submersion gate + heading (nose FD forwards, horizontal) the redirect rotates toward.
	IConsoleVariable *CarveGripRateCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("p.Chaos.Solver.CarveGripRate"));
	// Yaw-rate ceilings per sign (rad/s, -1 = off), resolved from YawRateCap*, the board speed and which
	// sign turns the nose toward the wave, each tick.
	IConsoleVariable *MaxAngularVelocityZPosCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("p.Chaos.Solver.MaxAngularVelocityZPos"));
	IConsoleVariable *MaxAngularVelocityZNegCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("p.Chaos.Solver.MaxAngularVelocityZNeg"));
	IConsoleVariable *CarveGripGateCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("p.Chaos.Solver.CarveGripGate"));
	IConsoleVariable *CarveHeadingXCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("p.Chaos.Solver.CarveHeadingX"));
	IConsoleVariable *CarveHeadingYCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("p.Chaos.Solver.CarveHeadingY"));
	// Wave-carry redirect: rate + gate + shoreward travel dir (-resolvedWaveBackDirection). See specs/wave-carry-redirect.md.
	IConsoleVariable *WaveCarryRedirectRateCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("p.Chaos.Solver.WaveCarryRedirectRate"));
	// Wave-normal damping (FR1). Fed every tick alongside the redirect directions above.
	IConsoleVariable *WaveNormalDampingRateCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("p.Chaos.Solver.WaveNormalDampingRate"));
	IConsoleVariable *WaveNormalDampingGateCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("p.Chaos.Solver.WaveNormalDampingGate"));
	IConsoleVariable *WhitewaterDampingRateCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("p.Chaos.Solver.WhitewaterDampingRate"));
	IConsoleVariable *WhitewaterAlongScaleCVar  = IConsoleManager::Get().FindConsoleVariable(TEXT("p.Chaos.Solver.WhitewaterAlongScale"));
	IConsoleVariable *WhitewaterNoseXCVar       = IConsoleManager::Get().FindConsoleVariable(TEXT("p.Chaos.Solver.WhitewaterNoseX"));
	IConsoleVariable *WhitewaterNoseYCVar       = IConsoleManager::Get().FindConsoleVariable(TEXT("p.Chaos.Solver.WhitewaterNoseY"));
	IConsoleVariable *WaveNormalXCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("p.Chaos.Solver.WaveNormalX"));
	IConsoleVariable *WaveNormalYCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("p.Chaos.Solver.WaveNormalY"));
	IConsoleVariable *WaterVelXCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("p.Chaos.Solver.WaterVelX"));
	IConsoleVariable *WaterVelYCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("p.Chaos.Solver.WaterVelY"));
	IConsoleVariable *WaterVelZCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("p.Chaos.Solver.WaterVelZ"));

	IConsoleVariable *WaveCarryRedirectGateCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("p.Chaos.Solver.WaveCarryRedirectGate"));
	IConsoleVariable *WaveCarryDirXCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("p.Chaos.Solver.WaveCarryDirX"));
	IConsoleVariable *WaveCarryDirYCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("p.Chaos.Solver.WaveCarryDirY"));
	IConsoleVariable *AngularDampingYAwayExtraCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("p.Chaos.Solver.AngularDampingYAwayExtra"));
	IConsoleVariable *AngularDampingYTowardReductionCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("p.Chaos.Solver.AngularDampingYTowardReduction"));
	IConsoleVariable *DebugDampingCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("p.Chaos.Solver.DebugDamping"));
	IConsoleVariable *UseCustomLinearDampingCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("p.Chaos.Solver.UseCustomLinearDamping"));
	IConsoleVariable *UseCustomAngularDampingCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("p.Chaos.Solver.UseCustomAngularDamping"));
	IConsoleVariable *MaxVelocityYCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("p.Chaos.Solver.MaxVelocityY"));
	IConsoleVariable *MaxVelocityXCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("p.Chaos.Solver.MaxVelocityX"));
	IConsoleVariable *MaxVelocityZUpCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("p.Chaos.Solver.MaxVelocityZUp"));
	IConsoleVariable *MaxVelocityZDownCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("p.Chaos.Solver.MaxVelocityZDown"));
	IConsoleVariable *IsMovableCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("p.Chaos.Solver.IsMovable"));

public:
	// Called every frame
	virtual void Tick(float DeltaTime) override;

	UFUNCTION(BlueprintCallable, Category="Surfing")
	void SetMovable(bool _movable);
};
