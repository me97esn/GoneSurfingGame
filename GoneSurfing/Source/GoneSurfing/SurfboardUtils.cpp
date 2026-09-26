// Fill out your copyright notice in the Description page of Project Settings.


#include "SurfboardUtils.h"
#include "SurfLog.h"
#include "Kismet/GameplayStatics.h"
#include "FluidDynamics.h"
#include "Buoyancy.h"
#include "Chaos/PBDRigidsEvolutionGBF.h"
#include "Chaos/PerParticleEtherDrag.h"
#include "HAL/IConsoleManager.h"
#include "SurfDebug.h"
#include "SurfTuningSubsystem.h"
#include "ShadowController.h"
#include "EngineUtils.h"

// Sets default values
ASurfboardUtils::ASurfboardUtils()
{
 	// Set this actor to call Tick() every frame.  You can turn this off to improve performance if you don't need it.
	PrimaryActorTick.bCanEverTick = true;

}

// Pull every damping coefficient from the shared SurfTuningSubsystem into this
// actor's UPROPERTY scratch fields. After this call, the rest of BeginPlay/Tick
// can keep reading `this->X` and transparently see live-tuned values. Warn-once
// if the subsystem is missing — the actor's hand-edited UPROPERTYs are the
// fallback in that case (pre-Phase-1 behavior). See specs/runtime-tuning.md.
void ASurfboardUtils::RefreshFromTuningSubsystem()
{
	if (!Tuning)
	{
		return;
	}
	waveNormalDampingRate        = Tuning->waveNormalDampingRate;
	SurfboardSidewaysDamping     = Tuning->SurfboardSidewaysDamping;
	SidewaysDampingAirScale      = Tuning->SidewaysDampingAirScale;
	SurfboardForwardsDamping     = Tuning->SurfboardForwardsDamping;
	SurfboardVerticalDamping     = Tuning->SurfboardVerticalDamping;
	WorldDownwardsDamping        = Tuning->WorldDownwardsDamping;
	WorldUpwardsDamping          = Tuning->WorldUpwardsDamping;
	AngularDampingX              = Tuning->AngularDampingX;
	AngularDampingY              = Tuning->AngularDampingY;
	AngularDampingZ              = Tuning->AngularDampingZ;
	AngularDampingYAwayExtra        = Tuning->AngularDampingYAwayExtra;
	AngularDampingYTowardReduction  = Tuning->AngularDampingYTowardReduction;
	YawDampingTiltBackInfluence  = Tuning->YawDampingTiltBackInfluence;
	PlaningRedirectMaxAngle      = Tuning->PlaningRedirectMaxAngle;
	PlaningRedirectOutwardScale  = Tuning->PlaningRedirectOutwardScale;
	PlaningRedirectCrestFade     = Tuning->PlaningRedirectCrestFade;
	CarveGripCrestFade           = Tuning->CarveGripCrestFade;
	PitchRightingRate            = Tuning->PitchRightingRate;
	CarveGripRate                = Tuning->CarveGripRate;
	YawRateCapMax                = Tuning->YawRateCapMax;
	YawRateCapSpeedKnee          = Tuning->YawRateCapSpeedKnee;
	CarveGripTurnUnfadeRate      = Tuning->CarveGripTurnUnfadeRate;
	CarveGripContactBlend        = Tuning->CarveGripContactBlend;
	CarveGripTurnUnfadeSymmetric = Tuning->CarveGripTurnUnfadeSymmetric;
	BrokenDampingRateScale       = Tuning->BrokenDampingRateScale;
	BrokenDampingGate            = Tuning->BrokenDampingGate;
	BrokenYawCapScale            = Tuning->BrokenYawCapScale;
	WhitewaterAlongNoseScale     = Tuning->WhitewaterAlongNoseScale;
	WaveCarryRedirectRate        = Tuning->WaveCarryRedirectRate;
	WaveCarryTargetCrossSpeed    = Tuning->WaveCarryTargetCrossSpeed;
	ClampYVelocityAt             = Tuning->ClampYVelocityAt;
	MaxVelocityX                 = Tuning->MaxVelocityX;
	MaxVelocityZUp               = Tuning->MaxVelocityZUp;
	MaxVelocityZDown             = Tuning->MaxVelocityZDown;
	VelocityDampingThreshold     = Tuning->VelocityDampingThreshold;
	VelocityDampingScale         = Tuning->VelocityDampingScale;
	MaxVelocityDamping           = Tuning->MaxVelocityDamping;
	AmountUnderWaterEquilibrium  = Tuning->AmountUnderWaterEquilibrium;
}

// Called when the game starts or when spawned
void ASurfboardUtils::BeginPlay()
{
	Super::BeginPlay();

	Tuning = SurfTuning::Get(this);
	if (!Tuning)
	{
		UE_LOG(LogSurf, Warning, TEXT("ASurfboardUtils::BeginPlay: SurfTuningSubsystem unavailable; falling back to actor UPROPERTYs."));
	}
	RefreshFromTuningSubsystem();

	this->DebugDampingCVar->Set(this->debugDamping);
	this->UseCustomLinearDampingCVar->Set(this->UseCustomLinearDamping);
	this->UseCustomAngularDampingCVar->Set(this->UseCustomAngularDamping);
	this->MaxVelocityYCVar->Set(this->ClampYVelocityAt); // TODO: This should probably vary with different waves
	if (this->MaxVelocityXCVar)     { this->MaxVelocityXCVar->Set(this->MaxVelocityX); }
	if (this->MaxVelocityZUpCVar)   { this->MaxVelocityZUpCVar->Set(this->MaxVelocityZUp); }
	if (this->MaxVelocityZDownCVar) { this->MaxVelocityZDownCVar->Set(this->MaxVelocityZDown); }
	this->IsMovableCVar->Set(true);

	// Wire the two SharedCalculations actors to each other so either can compute
	// a board-wide averaged value (used by the yaw hydrofoil; see
	// specs/yaw-hydrofoil-board-wide-slip.md).
	if (this->sharedCalculationsFront && this->sharedCalculationsBack)
	{
		this->sharedCalculationsFront->otherHalfSharedCalculations = this->sharedCalculationsBack;
		this->sharedCalculationsBack->otherHalfSharedCalculations  = this->sharedCalculationsFront;
	}

	// Wire the WeightDistribution actor into both SCs so the bottom-hydrofoil turn-gate
	// can read the surfer's intent (commanded lateral weight shift).
	if (this->weightDistribution)
	{
		if (this->sharedCalculationsFront) this->sharedCalculationsFront->weightDistribution = this->weightDistribution;
		if (this->sharedCalculationsBack)  this->sharedCalculationsBack->weightDistribution  = this->weightDistribution;
	}

	// Wire one SC into WD so the pump impulse path can read boardWideSlopeSin
	// for slope attenuation. Front SC is arbitrary; the slope signal is board-
	// wide so either side works. See specs/pumping.md.
	if (this->weightDistribution && this->sharedCalculationsFront)
	{
		this->weightDistribution->sharedCalculations = this->sharedCalculationsFront;
	}

	// Board shadow grounding. Spawned rather than placed: the controller needs no per-level
	// configuration (it resolves the board from the FluidDynamics actors itself), so requiring a
	// drag into every .umap would be friction with nothing behind it. Skipped if one was placed by
	// hand — a placed actor carries hand-tuned properties this must not shadow. Single controller
	// per world: on a multi-board level, place them explicitly. See specs/board-shadow-grounding.md.
	if (this->bAutoSpawnShadowController)
	{
		bool bAlreadyPresent = false;
		for (TActorIterator<AShadowController> it(GetWorld()); it; ++it)
		{
			bAlreadyPresent = true;
			break;
		}
		if (!bAlreadyPresent)
		{
			FActorSpawnParameters params;
			params.ObjectFlags |= RF_Transient; // runtime-only; never dirties the level
			AShadowController* shadows = GetWorld()->SpawnActor<AShadowController>(
				AShadowController::StaticClass(), FTransform::Identity, params);
			UE_LOG(LogSurf, Log, TEXT("ASurfboardUtils::BeginPlay: shadow controller %s"),
				shadows ? TEXT("spawned") : TEXT("FAILED to spawn"));
		}
	}

	// One-shot dump of tunable defaults. Captured at startup so the values can be
	// promoted into USurfTuningSubsystem header defaults; see specs/runtime-tuning.md.
#if WITH_EDITOR
	const FString lbl = GetActorLabel();
#else
	const FString lbl = GetName();
#endif
	UE_LOG(LogSurf, Warning, TEXT("TuningDefaults: SurfboardUtils [%s] SurfboardSidewaysDamping=%g SurfboardForwardsDamping=%g SurfboardVerticalDamping=%g WorldDownwardsDamping=%g WorldUpwardsDamping=%g"),
		*lbl, SurfboardSidewaysDamping, SurfboardForwardsDamping, SurfboardVerticalDamping, WorldDownwardsDamping, WorldUpwardsDamping);
	UE_LOG(LogSurf, Warning, TEXT("TuningDefaults: SurfboardUtils [%s] AngularDampingX=%g AngularDampingY=%g AngularDampingZ=%g AngularDampingYAwayExtra=%g AngularDampingYTowardReduction=%g YawDampingTiltBackInfluence=%g"),
		*lbl, AngularDampingX, AngularDampingY, AngularDampingZ, AngularDampingYAwayExtra, AngularDampingYTowardReduction, YawDampingTiltBackInfluence);
	UE_LOG(LogSurf, Warning, TEXT("TuningDefaults: SurfboardUtils [%s] ClampYVelocityAt=%g MaxVelocityX=%g MaxVelocityZUp=%g MaxVelocityZDown=%g"),
		*lbl, ClampYVelocityAt, MaxVelocityX, MaxVelocityZUp, MaxVelocityZDown);
	UE_LOG(LogSurf, Warning, TEXT("TuningDefaults: SurfboardUtils [%s] VelocityDampingThreshold=%g VelocityDampingScale=%g MaxVelocityDamping=%g UseVelocityDependentDamping=%d UseCustomLinearDamping=%d UseCustomAngularDamping=%d AmountUnderWaterEquilibrium=%g"),
		*lbl, VelocityDampingThreshold, VelocityDampingScale, MaxVelocityDamping, UseVelocityDependentDamping ? 1 : 0, UseCustomLinearDamping ? 1 : 0, UseCustomAngularDamping ? 1 : 0, AmountUnderWaterEquilibrium);
}

void ASurfboardUtils::SetMovable(bool _movable){
	this->IsMovableCVar->Set(_movable);
}

// Called every frame
void ASurfboardUtils::Tick(float DeltaTime)
{
	// Pull live tuning values onto this actor's UPROPERTY scratch BEFORE anything
	// reads them. The MaxVelocity*/ClampY CVars below also re-push every frame so
	// runtime edits in the tuning panel take effect on the next physics tick.
	RefreshFromTuningSubsystem();
	if (Tuning)
	{
		if (MaxVelocityYCVar)     { MaxVelocityYCVar->Set(ClampYVelocityAt); }
		if (MaxVelocityXCVar)     { MaxVelocityXCVar->Set(MaxVelocityX); }
		if (MaxVelocityZUpCVar)   { MaxVelocityZUpCVar->Set(MaxVelocityZUp); }
		if (MaxVelocityZDownCVar) { MaxVelocityZDownCVar->Set(MaxVelocityZDown); }
	}

	float avgAmountUnderWater = (this->buoyancyNoseLeft->amountUnderWater +
								 this->buouyancyTailLeft->amountUnderWater +
								 this->buoyancyNoseRight->amountUnderWater +
								 this->buoyancyTailRight->amountUnderWater) /
								4;

	// Using simple way to calculate if the board tilts back: more weight in the tail equals tilting backwards.
	//
	// Measured down from the furthest-forward weight the player can actually command, not from a
	// hardcoded 0.5. WeightMaxInFront caps player input below centre (0.45 by default), so against a
	// fixed 0.5 this could never reach 0 and yaw damping was permanently reduced by ~13% even with
	// the weight as far forward as the player could put it. At maximum forward weight the tilt-back
	// influence should be nil. Clamped to 0.5 so raising the cap back to 1.0 restores exactly the
	// old behaviour rather than reading a fully forward stance as tilted back.
	const float tiltBackZeroAt = FMath::Min(Tuning ? Tuning->WeightMaxInFront : 0.5f, 0.5f);
	const float amountTiltingBack = FMath::Clamp(
		(tiltBackZeroAt - this->weightDistribution->amountInFront) * 3.0f, 0.0f, 1.0f);

	float avgAmountPlaning = (this->sharedCalculationsFront->AmountPlaning +
							  this->sharedCalculationsBack->AmountPlaning)  /
							 2;

	Super::Tick(DeltaTime);
	auto Rotation = GetActorRotation();

	auto v = this->sharedCalculationsFront->componentVelocity;
	

	this->RotationPitch->Set((float)Rotation.Pitch);
	this->RotationYaw->Set((float)Rotation.Yaw);
	this->RotationRoll->Set((float)Rotation.Roll);
	// UE_LOG(LogSurf, Display, TEXT("Pitch: %f, Yaw: %f, Roll: %f"), (float)Rotation.Pitch, (float)Rotation.Yaw, (float)Rotation.Roll);
	if (DampingLocalX && DampingLocalY && DampingLocalZ &&
		DampingZDown && DampingZUp)
	{
		// Push the project's debugDamping flag through to the engine-side cvar so the Chaos
		// integrator emits its V-before / V-after damping log block. Throttle it by the same
		// debugLogEveryNthTick rate as our project logs — otherwise the engine fires ~5 log
		// blocks per physics substep with no internal throttling, flooding the log.
		const bool shouldEngineLogThisTick = (this->debugDamping || SurfDebug::ShouldDebug(this, TEXT("damping"))) &&
			(debugLogEveryNthTick <= 1 || (debugFrameCounter + 1) % debugLogEveryNthTick == 0);
		if (DebugDampingCVar)
		{
			DebugDampingCVar->Set(shouldEngineLogThisTick);
		}
		if(v.Z > 0){
			this->DampingLocalZ->Set(this->SurfboardVerticalDamping * (1 - avgAmountUnderWater));
			// this->DampingLocalZ->Set(this->SurfboardVerticalDamping * (1 - avgAmountUnderWater));
		}
		else
		{
			this->DampingLocalZ->Set(this->SurfboardVerticalDamping * avgAmountUnderWater);
		}

		// Calculate velocity-dependent forward damping
	FVector velocityWorldSpace = (this->sharedCalculationsFront->componentVelocity +
								  this->sharedCalculationsBack->componentVelocity) / 2;
	FVector velocityLocalSpace = Rotation.UnrotateVector(velocityWorldSpace);
	float forwardSpeed = FMath::Abs(velocityLocalSpace.X);  // Speed in forward direction

	// Base damping (existing logic)
	float baseDamping = this->SurfboardForwardsDamping * (1 - avgAmountPlaning);

	// Add velocity-dependent component if above threshold
	float finalDamping = baseDamping;
	if (this->UseVelocityDependentDamping && forwardSpeed > this->VelocityDampingThreshold)
	{
		// Calculate excess velocity beyond threshold
		float excessVelocity = forwardSpeed - this->VelocityDampingThreshold;

		// Quadratic scaling: damping increases with velocity²
		// Scale is squared to allow easier tuning (0.0005 becomes 0.00000025)
		float scaleSquared = this->VelocityDampingScale * this->VelocityDampingScale;
		float velocityDampingComponent = scaleSquared * excessVelocity * excessVelocity;

		// Add to base damping and clamp to max
		finalDamping = FMath::Min(baseDamping + velocityDampingComponent, this->MaxVelocityDamping);
	}

	this->DampingLocalX->Set(finalDamping);// It appears that X is forwards when I debug this.

		this->DampingZUp->Set(this->WorldUpwardsDamping * (1-avgAmountUnderWater/2));
		this->DampingZDown->Set(this->WorldDownwardsDamping * avgAmountUnderWater/2);
		this->AngularDampingXCVar->Set(this->AngularDampingX);
		this->AngularDampingYCVar->Set(this->AngularDampingY);

		// Surface-relative pitch damping feed. Push the board's pitch misalignment vs the local water
		// surface (waveRelativePitchSin, averaged front/back) to the engine, which damps pitch rotation
		// moving AWAY from alignment more than rotation moving TOWARD it. Both extras are contact-gated by
		// average amountWetted so an airborne board isn't damped (nothing to track). The away/toward sign
		// test in the engine replaces the old weight/wetted gates. See specs/surface-relative-pitch-damping.md.
		const float frontPitchSin = this->sharedCalculationsFront ? (float)this->sharedCalculationsFront->waveRelativePitchSin : 0.0f;
		const float backPitchSin  = this->sharedCalculationsBack  ? (float)this->sharedCalculationsBack->waveRelativePitchSin  : 0.0f;
		const float pitchMisalign = 0.5f * (frontPitchSin + backPitchSin);
		// amountWetted is dead/pegged at ~1.0 (never gated); use the live amountUnderWater, which reads ~0
		// airborne as this gate intends. See [[submersion-gates-comparison]].
		const float frontWetted   = this->sharedCalculationsFront ? this->sharedCalculationsFront->amountUnderWater : 0.0f;
		const float backWetted    = this->sharedCalculationsBack  ? this->sharedCalculationsBack->amountUnderWater  : 0.0f;
		const float contactGate   = FMath::SmoothStep(0.1f, 0.5f, 0.5f * (frontWetted + backWetted));

		// Sideways (board-local Y) linear damping - the anti-sideways-slide term, and the oldest
		// custom damping in the project. Moved down here from the top of the block so it can share
		// the ONE contact definition the pitch and wave-normal damping already use: it was the only
		// local linear axis with no contact gate at all (Z is scaled by avgAmountUnderWater, X by
		// (1 - avgAmountPlaning)), so it went on braking a board that had left the water. Because
		// the axis is board-LOCAL, mid-slide "sideways" carries most of the DOWN-THE-LINE velocity,
		// so what it took was exactly the speed the board should have kept. Measured: 266 cm/s of
		// horizontal speed lost over 0.45 s of airborne flight, where it must be constant.
		// SidewaysDampingAirScale = what survives with no contact; 0 = the fix, 1 = the old
		// ungated behaviour for A/B. (Y is sideways when debugged - the original note.)
		// See specs/carve-grip-via-redirect.md "Step 4 revisited".
		this->DampingLocalY->Set(this->SurfboardSidewaysDamping
			* FMath::Lerp(FMath::Clamp(this->SidewaysDampingAirScale, 0.0f, 1.0f), 1.0f, contactGate));
		const float effectiveAwayExtra       = this->AngularDampingYAwayExtra       * contactGate;
		const float effectiveTowardReduction = this->AngularDampingYTowardReduction * contactGate;
		// Wave-normal damping feed (FR1, specs/wave-interaction-damping-and-redirect.md). The solver
		// damps the wave-normal component of velocity RELATIVE TO THE WATER, after the redirects, so
		// it only removes the into-wave velocity the hull could not turn. Contact-gated with the same
		// gate as the pitch damping: an airborne board has no water to be damped against.
		{
			const ASharedCalculations* SC = this->sharedCalculationsFront ? this->sharedCalculationsFront
			                                                             : this->sharedCalculationsBack;
			// The cross-shore axis. Down-the-line motion has ~zero component along it, which is what
			// keeps FR3's "normal only" promise without a second gate.
			//
			// Deliberately NOT the sampled waveNormal, which wavePenetrationDrag uses: measured over a
			// ride, that normal is cross-shore dominant only 78% of the time and points DOWN THE LINE
			// on the other 22% (mean |y| 0.369 against |x| 0.830), with 22 deg of tick-to-tick jitter
			// at p90. It carries the exporter's inflated Y component — see M6 in
			// specs/wave-mass-thrust-closing-speed.md. Damping along that axis would intermittently
			// brake the board down the line, which is exactly what FR3 forbids.
			//
			// resolvedWaveBackDirection is derived from the InfiniteWaveManager tile geometry by an
			// unsaturated hill-climb rather than sampled per tick, so it is stable. Same axis the
			// torque budget already decomposes into. Falls back to waveBackDirection.
			FVector waveNormalHoriz = FVector::ZeroVector;
			FVector absWaterVel     = FVector::ZeroVector;
			if (SC)
			{
				waveNormalHoriz = SC->resolvedWaveBackDirection.IsZero()
					? SC->waveBackDirection.GetSafeNormal()
					: SC->resolvedWaveBackDirection.GetSafeNormal();
				waveNormalHoriz = FVector(waveNormalHoriz.X, waveNormalHoriz.Y, 0.0f).GetSafeNormal();
				absWaterVel     = SC->absoluteWaterVelocity;
			}
			// The whitewater (broken-wave A1): the board is in collapsed water by the wave-geometry
			// service's read at either SC. Opens the gate below where the slope gate is shut (a bore
			// reads 0.03-0.10) and scales the rate, so the water carries the board at its own speed.
			const float brokenGeo = FMath::Max(
				this->sharedCalculationsFront ? this->sharedCalculationsFront->brokenGeo : 0.0f,
				this->sharedCalculationsBack  ? this->sharedCalculationsBack->brokenGeo  : 0.0f);
			const float brokenGate = FMath::Clamp(brokenGeo * this->BrokenDampingGate, 0.0f, 1.0f);
			const float waveNormalRate = this->waveNormalDampingRate * FMath::Lerp(1.0f, this->BrokenDampingRateScale, brokenGate);
			// In the bore the nose and the tail sit in different water (M16: -118 vs -343 two metres
			// apart at a collapse); the bore that has the board is the stronger one. While broken, the
			// target blends from the front SC's water toward whichever SC's water runs faster shoreward.
			if (brokenGate > 0.0f && this->sharedCalculationsFront && this->sharedCalculationsBack)
			{
				const FVector wF = this->sharedCalculationsFront->absoluteWaterVelocity;
				const FVector wB = this->sharedCalculationsBack->absoluteWaterVelocity;
				const FVector strongest = (FVector::DotProduct(wB, waveNormalHoriz) < FVector::DotProduct(wF, waveNormalHoriz)) ? wB : wF;
				absWaterVel = FMath::Lerp(absWaterVel, strongest, brokenGate);
			}
			// The whitewater damping proper (fork: p.Chaos.Solver.WhitewaterDampingRate): the WHOLE
			// horizontal velocity toward the water's, not just the cross-shore component - a board in a
			// bore is a cork; it keeps neither its along-line speed nor its heading's momentum (M16: the
			// normal-only term left the board threading the bore along its nose at -150 while the water
			// went shoreward). Contact-gated softly: the lip lifts the board (underW 0.4 at the collapse)
			// and any water contact counts in aerated water; only a board thrown clear is exempt.
			const float whitewaterContact = FMath::SmoothStep(0.02f, 0.15f, 0.5f * (frontWetted + backWetted));
			const float whitewaterRate = this->waveNormalDampingRate * this->BrokenDampingRateScale * brokenGate * whitewaterContact;
			if (this->WhitewaterDampingRateCVar) { this->WhitewaterDampingRateCVar->Set(whitewaterRate); }
			// M18: the split in the board's frame - the nose, and the along-nose fraction.
			{
				FVector noseH = this->noseFluidDynamics ? this->noseFluidDynamics->forwards : FVector::ZeroVector;
				noseH.Z = 0.0;
				noseH = noseH.GetSafeNormal();
				if (this->WhitewaterNoseXCVar)      { this->WhitewaterNoseXCVar->Set((float)noseH.X); }
				if (this->WhitewaterNoseYCVar)      { this->WhitewaterNoseYCVar->Set((float)noseH.Y); }
				if (this->WhitewaterAlongScaleCVar) { this->WhitewaterAlongScaleCVar->Set(this->WhitewaterAlongNoseScale); }
			}
			// A zero normal (flat water, no face) disables it in the solver via Normalize() failing.
			if (this->WaveNormalDampingRateCVar) { this->WaveNormalDampingRateCVar->Set(waveNormalRate); }
			// Contact AND (slope OR broken). Contact alone leaves the damping fully on out on the flat,
			// where it drags the board's cross-shore velocity down to the ~53 cm/s the water is doing
			// there and the board stops dead a second after leaving the wave. The slope gate is what
			// kept it off in the whitewater too (M10/M15), hence the broken term.
			float slopeGate = 1.0f;
			if (SC && Tuning && Tuning->waveNormalDampingMinSlopeSin > 0.0f)
			{
				slopeGate = FMath::SmoothStep(
					Tuning->waveNormalDampingMinSlopeSin,
					Tuning->waveNormalDampingMinSlopeSin + FMath::Max(0.001f, Tuning->waveNormalDampingSlopeWidth),
					SC->boardWideSlopeSin);
			}
			const float waveNormalGate = contactGate * FMath::Max(slopeGate, brokenGate);
			if (this->WaveNormalDampingGateCVar) { this->WaveNormalDampingGateCVar->Set(waveNormalGate); }
			if (this->WaveNormalXCVar)           { this->WaveNormalXCVar->Set((float)waveNormalHoriz.X); }
			if (this->WaveNormalYCVar)           { this->WaveNormalYCVar->Set((float)waveNormalHoriz.Y); }
			if (this->WaterVelXCVar)             { this->WaterVelXCVar->Set((float)absWaterVel.X); }
			if (this->WaterVelYCVar)             { this->WaterVelYCVar->Set((float)absWaterVel.Y); }
			if (this->WaterVelZCVar)             { this->WaterVelZCVar->Set((float)absWaterVel.Z); }

			if (SurfDebug::ShouldDebug(this, TEXT("wavenormal")) && SC)
			{
				const FVector boardVel = SC->componentVelocity;
				UE_LOG(LogSurf, Warning,
					TEXT("WAVENORMAL [%s] n=(%.3f, %.3f) rate=%.3f gate=%.2f (slope=%.2f broken=%.2f) ww=%.3f | boardVn=%.1f waterVn=%.1f relVn=%.1f (>0 = board crossing into the wave)"),
					*GetName(), waveNormalHoriz.X, waveNormalHoriz.Y, waveNormalRate, waveNormalGate, slopeGate, brokenGate, whitewaterRate,
					FVector::DotProduct(boardVel, waveNormalHoriz), FVector::DotProduct(absWaterVel, waveNormalHoriz),
					FVector::DotProduct(boardVel - absWaterVel, waveNormalHoriz));
			}
		}

		if (this->PitchMisalignmentCVar)              { this->PitchMisalignmentCVar->Set(pitchMisalign); }
		if (this->AngularDampingYAwayExtraCVar)       { this->AngularDampingYAwayExtraCVar->Set(effectiveAwayExtra); }
		if (this->AngularDampingYTowardReductionCVar) { this->AngularDampingYTowardReductionCVar->Set(effectiveTowardReduction); }

		// Pitch-righting servo feed: the engine drives the pitch rate toward alignment with the surface
		// at this rate (× the misalignment it already receives above). Contact-gated like the damping
		// extras so an airborne board isn't steered. See specs/pitch-righting-and-redirect-escape.md.
		if (this->PitchRightingRateCVar) { this->PitchRightingRateCVar->Set(this->PitchRightingRate * contactGate); }

		// Planing redirect feed. The engine rotates the board's world velocity toward the up-the-face
		// direction at a rate of PlaningRedirectMaxAngle * gate. Gate = NOSE submersion (front SC
		// amountUnderWater) — nose-specific because the redirect is about the nose climbing the face. The
		// direction is up-slope = -waveSlopeDownVec (world); its magnitude is ~0 on flat water, which
		// disables the redirect there. See specs/planing-redirect.md.
		const float noseUnderWater = this->sharedCalculationsFront ? (float)this->sharedCalculationsFront->amountUnderWater : 0.0f;
		// Crest-proximity fade: the redirect/grip targets ("up the face" / the into-wave nose) are the
		// escort-over-the-lip near the crest, so fade them out as the nose approaches it. crestDistFront > 0
		// = the nose is on the FRONT-FACE side, that many cm short of the crest (signedDistanceToCrest is
		// negative there); <= 0 = at/behind the crest -> gate 0. Knob 0 disables (gate 1 = current behavior).
		// See specs/pitch-righting-and-redirect-escape.md (Correction).
		const float crestDistFront = this->sharedCalculationsFront ? -this->sharedCalculationsFront->signedDistanceToCrest : 1e9f;
		const float planingCrestGate = (this->PlaningRedirectCrestFade > 0.0f)
			? FMath::SmoothStep(0.0f, this->PlaningRedirectCrestFade, crestDistFront) : 1.0f;
		float gripCrestGate = (this->CarveGripCrestFade > 0.0f)
			? FMath::SmoothStep(0.0f, this->CarveGripCrestFade, crestDistFront) : 1.0f;

		// Which yaw sign turns the nose toward the wave: a positive angular velocity about +Z moves the nose
		// by W x nose = (-w*ny, w*nx), so d(nose . back)/dt has the sign of w * (nx*by - ny*bx). Shared by the
		// grip un-fade below and the yaw-rate ceiling feed further down. See specs/lip-snap-spinout-and-air.md.
		double towardSign = 0.0;
		float  yawTowardWave = 0.0f; // rad/s, >= 0; yaw toward the wave only (feeds the log + the cap's semantics)
		float  yawForUnfade  = 0.0f; // rad/s, >= 0; what lifts the crest fade (see CarveGripTurnUnfadeSymmetric)
		if (this->sharedCalculationsFront && this->noseFluidDynamics)
		{
			const FVector nose = FVector(this->noseFluidDynamics->forwards.X, this->noseFluidDynamics->forwards.Y, 0.0).GetSafeNormal();
			const FVector back = this->sharedCalculationsFront->resolvedWaveBackDirection.IsZero()
				? this->sharedCalculationsFront->waveBackDirection : this->sharedCalculationsFront->resolvedWaveBackDirection;
			towardSign = nose.X * back.Y - nose.Y * back.X;
			if (this->noseFluidDynamics->surfboardMesh)
			{
				const float wz = (float)this->noseFluidDynamics->surfboardMesh->GetPhysicsAngularVelocityInRadians().Z;
				yawTowardWave = FMath::Max(0.0f, towardSign > 0.0 ? wz : -wz);
				// What the crest un-fade below keys on. The toward-only signal is 0 for the whole
				// second half of a turn - a top turn yaws AWAY from the wave by definition - so on
				// the measured ride it read 0.561 during the snap and 0.000 for everything after,
				// and the grip stayed faded out no matter how wet the board was. |wz| asks the
				// question the fade actually wants: is this rail loaded in a turn.
				yawForUnfade = FMath::Lerp(yawTowardWave, FMath::Abs(wz),
					FMath::Clamp(this->CarveGripTurnUnfadeSymmetric, 0.0f, 1.0f));
			}
		}
		// Lift the crest fade while yawing toward the wave (a loaded rail in a turn grips; see the tunable).
		if (this->CarveGripTurnUnfadeRate > 0.0f && gripCrestGate < 1.0f)
		{
			const float unfade = FMath::Min(1.0f, yawForUnfade / this->CarveGripTurnUnfadeRate);
			gripCrestGate += (1.0f - gripCrestGate) * unfade;
		}
		if (this->PlaningRedirectGateCVar) { this->PlaningRedirectGateCVar->Set(noseUnderWater * planingCrestGate); }
		const FVector upSlope = this->sharedCalculationsFront ? -this->sharedCalculationsFront->waveSlopeDownVec : FVector::ZeroVector;
		if (this->PlaningRedirectUpXCVar) { this->PlaningRedirectUpXCVar->Set((float)upSlope.X); }
		if (this->PlaningRedirectUpYCVar) { this->PlaningRedirectUpYCVar->Set((float)upSlope.Y); }
		if (this->PlaningRedirectUpZCVar) { this->PlaningRedirectUpZCVar->Set((float)upSlope.Z); }
		// Strength knob (rad/s), overlaid from the tuning subsystem; 0 = off.
		if (this->PlaningRedirectMaxAngleCVar) { this->PlaningRedirectMaxAngleCVar->Set(this->PlaningRedirectMaxAngle); }
		// One-sided scale (1 = symmetric legacy); see specs/pitch-righting-and-redirect-escape.md.
		if (this->PlaningRedirectOutwardScaleCVar) { this->PlaningRedirectOutwardScaleCVar->Set(this->PlaningRedirectOutwardScale); }

		// Carve grip feed. The engine rotates the board's horizontal velocity toward CarveHeading at
		// CarveGripRate * gate. Heading = the NOSE FluidDynamics actor's forwards, horizontal + normalized
		// (rocker-pitched, so it aids the carve when rolled — the SCs are axis-aligned and won't supply it).
		// Gate = nose amountUnderWater (front SC). See specs/carve-grip-via-redirect.md.
		if (this->CarveGripRateCVar) { this->CarveGripRateCVar->Set(this->CarveGripRate); }
		// Contact signal for the GRIP only - the planing redirect above keeps the nose, because
		// "the nose climbing the face" is a real rationale for that one and not for this one.
		// Grip comes from the fins and the loaded rail, both aft of the nose, so a lifted nose over
		// a wet tail is a board that still grips. See specs/carve-grip-via-redirect.md.
		const float gripContact = FMath::Lerp(noseUnderWater, contactGate,
			FMath::Clamp(this->CarveGripContactBlend, 0.0f, 1.0f));
		if (this->CarveGripGateCVar) { this->CarveGripGateCVar->Set(gripContact * gripCrestGate); }

		// Yaw-rate ceiling feed: cap(v) = Max * min(1, Knee / v), horizontal board speed, on the yaw sign that
		// turns the nose TOWARD the wave (resolvedWaveBackDirection); the other sign is left free. A positive
		// angular velocity about +Z moves the nose by W x nose = (-w*ny, w*nx), so d(nose . back)/dt has the
		// sign of w * (nx*by - ny*bx): positive-Z yaw is toward the wave iff that 2D cross is positive. Not
		// contact-gated: in the air a board has no rail to yaw with either. -1 = off.
		// See specs/lip-snap-spinout-and-air.md (T1).
		{
			float yawCapPos = -1.0f, yawCapNeg = -1.0f;
			if (this->YawRateCapMax > 0.0f && this->sharedCalculationsFront && this->noseFluidDynamics)
			{
				const float hSpeed = (float)this->sharedCalculationsFront->componentVelocity.Size2D();
				const float knee   = FMath::Max(this->YawRateCapSpeedKnee, 1.0f);
				// A3 (broken-wave spec): in the whitewater the ceiling drops by BrokenYawCapScale and
				// applies to BOTH signs - the away-from-wave sign is unlimited on the face (the cutback)
				// and only ever fed here. The redirect-family way to take control away in the foam.
				const float brokenGeo = FMath::Max(
					this->sharedCalculationsFront->brokenGeo,
					this->sharedCalculationsBack ? this->sharedCalculationsBack->brokenGeo : 0.0f);
				const float cap    = this->YawRateCapMax * FMath::Min(1.0f, knee / FMath::Max(hSpeed, 1.0f))
				                   * FMath::Lerp(1.0f, this->BrokenYawCapScale, brokenGeo);
				if (towardSign > 0.0) { yawCapPos = cap; } else { yawCapNeg = cap; }
				// The away sign has no cap on the face and cannot be lerped from "none", so it gets
				// cap / broken: unlimited as broken -> 0, the same cap as the toward sign at 1. A hard
				// "if broken > 0" here fed the FULL cap to a cutback on a pocket ride that grazed the
				// band's edge at broken ~1e-5 (M17) - the lip-snap M7 regression in one tick.
				if (brokenGeo > 0.01f)
				{
					const float awayCap = cap / brokenGeo;
					if (towardSign > 0.0) { yawCapNeg = awayCap; } else { yawCapPos = awayCap; }
				}
				if (SurfDebug::ShouldDebug(this, TEXT("yawcap")))
				{
					UE_LOG(LogSurf, Warning, TEXT("YAWCAP v=%.0f cap=%.3f rad/s on %s broken=%.2f | cross=%.3f yawToward=%.3f yawUnfade=%.3f crestGate=%.2f contact(nose=%.2f board=%.2f used=%.2f) gripGate=%.2f"),
						hSpeed, cap, brokenGeo > 0.0f ? TEXT("both") : (towardSign > 0.0 ? TEXT("+Z") : TEXT("-Z")), brokenGeo, towardSign,
						yawTowardWave, yawForUnfade, gripCrestGate, noseUnderWater, contactGate, gripContact, gripContact * gripCrestGate);
				}
			}
			if (this->MaxAngularVelocityZPosCVar) { this->MaxAngularVelocityZPosCVar->Set(yawCapPos); }
			if (this->MaxAngularVelocityZNegCVar) { this->MaxAngularVelocityZNegCVar->Set(yawCapNeg); }
		}
		if (this->noseFluidDynamics)
		{
			FVector noseFwd = this->noseFluidDynamics->forwards;
			noseFwd.Z = 0.0;
			noseFwd = noseFwd.GetSafeNormal();
			if (this->CarveHeadingXCVar) { this->CarveHeadingXCVar->Set((float)noseFwd.X); }
			if (this->CarveHeadingYCVar) { this->CarveHeadingYCVar->Set((float)noseFwd.Y); }
		}

		// Wave-carry redirect feed. The engine rotates the board's horizontal velocity toward the wave's
		// shoreward travel direction (-resolvedWaveBackDirection) at WaveCarryRedirectRate * gate.
		// SELF-LIMITING gate (specs/wave-carry-redirect.md option 1): drive while the board's shoreward speed is
		// below the crest's (WaveCarryTargetCrossSpeed) and fade to 0 as it matches, so it settles at the keep-up
		// drift instead of the v1 bang-bang. Also gated off flat water (slope).
		if (this->WaveCarryRedirectRateCVar) { this->WaveCarryRedirectRateCVar->Set(this->WaveCarryRedirectRate); }
		{
			FVector carryDir = FVector::ZeroVector;
			float waveCarryGate = 0.0f;
			float lag = 0.0f, boardCross = 0.0f;
			if (this->sharedCalculationsFront)
			{
				const FVector backDir = this->sharedCalculationsFront->resolvedWaveBackDirection; // +backDir (toward back)
				if (!backDir.IsNearlyZero())
				{
					carryDir = FVector(-backDir.X, -backDir.Y, 0.0f).GetSafeNormal(); // shoreward
					// boardCross = board velocity along +backDir (shoreward = negative). lag>0 = shoreward too slow.
					boardCross = (float)FVector::DotProduct(this->sharedCalculationsFront->componentVelocity, backDir);
					lag = boardCross + this->WaveCarryTargetCrossSpeed;
					const float lagGate = FMath::Clamp(lag / 30.0f, 0.0f, 1.0f); // ramp over 30 cm/s
					const float slopeGate = FMath::SmoothStep(0.08f, 0.14f, this->sharedCalculationsFront->boardWideSlopeSin);
					waveCarryGate = lagGate * slopeGate;
				}
			}
			if (this->WaveCarryRedirectGateCVar) { this->WaveCarryRedirectGateCVar->Set(waveCarryGate); }
			if (this->WaveCarryDirXCVar) { this->WaveCarryDirXCVar->Set((float)carryDir.X); }
			if (this->WaveCarryDirYCVar) { this->WaveCarryDirYCVar->Set((float)carryDir.Y); }
			if (SurfDebug::ShouldDebug(this, TEXT("wavecarry")))
			{
				UE_LOG(LogSurf, Warning, TEXT("WAVECARRY [%s] carryDir=(%.3f, %.3f) boardCross=%.1f lag=%.1f gate=%.2f rate=%.1f"),
					*GetName(), carryDir.X, carryDir.Y, boardCross, lag, waveCarryGate, this->WaveCarryRedirectRate);
			}
		}

		if ((this->debugDamping || SurfDebug::ShouldDebug(this, TEXT("damping"))) &&
		    (debugLogEveryNthTick <= 1 || (debugFrameCounter + 1) % debugLogEveryNthTick == 0))
		{
			UE_LOG(LogSurf, Warning,
				TEXT("PitchDamping: pitchMisalign=%.3f (frontSin=%.3f backSin=%.3f) frontW=%.3f backW=%.3f contactGate=%.3f -> awayExtra=%.4f towardRed=%.4f | planingRedirectGate(noseUW)=%.3f"),
				pitchMisalign, frontPitchSin, backPitchSin, frontWetted, backWetted, contactGate, effectiveAwayExtra, effectiveTowardReduction, noseUnderWater);
		}
		// Less yaw damping when weight is back AND the board is actually carving (planing).
		// Tilt-back collapses front-of-board water contact, so the steady-state stabilization
		// from front hydrodynamics is gone — but that's also when we want fast carving yaw.
		// Gating by avgAmountPlaning makes the reduction only apply during an active carve:
		// a stopped board (planing=0) gets full damping and the residual spin dies.
		// YawDampingTiltBackInfluence puts a floor on how much the modulation can reduce damping.
		this->AngularDampingZCVar->Set(this->AngularDampingZ * (1 - amountTiltingBack * avgAmountPlaning * this->YawDampingTiltBackInfluence));

		// Debug logging for buoyancy state
		++debugFrameCounter;
		if ((this->debugDamping || SurfDebug::ShouldDebug(this, TEXT("damping"))) && (debugLogEveryNthTick <= 1 || debugFrameCounter % debugLogEveryNthTick == 0))
		{
			UE_LOG(LogSurf, Warning, TEXT("=== SurfboardUtils Frame ==="));
			UE_LOG(LogSurf, Warning, TEXT("  Buoyancy Points [NL:%.3f NR:%.3f TL:%.3f TR:%.3f]"),
				this->buoyancyNoseLeft->amountUnderWater,
				this->buoyancyNoseRight->amountUnderWater,
				this->buouyancyTailLeft->amountUnderWater,
				this->buoyancyTailRight->amountUnderWater);
			UE_LOG(LogSurf, Warning, TEXT("  Avg Amount Under Water: %.3f"), avgAmountUnderWater);
			UE_LOG(LogSurf, Warning, TEXT("  Avg Amount Planing: %.3f"), avgAmountPlaning);
			UE_LOG(LogSurf, Warning, TEXT("  Forward Speed: %.1f cm/s"), forwardSpeed);
		UE_LOG(LogSurf, Warning, TEXT("  Base Forward Damping: %.3f"), baseDamping);
		if (forwardSpeed > this->VelocityDampingThreshold)
		{
			UE_LOG(LogSurf, Warning, TEXT("  Velocity Damping Component: %.3f"), finalDamping - baseDamping);
		}
		UE_LOG(LogSurf, Warning, TEXT("  Final Forward Damping: %.3f"), finalDamping);
			UE_LOG(LogSurf, Warning, TEXT("  Set DampingLocalY: %.3f (Sideways, pre-contact-gate; airScale=%.2f)"), this->SurfboardSidewaysDamping, this->SidewaysDampingAirScale);
			UE_LOG(LogSurf, Warning, TEXT("  Set DampingLocalZ: %.3f (Vertical)"), this->DampingLocalZ->GetFloat());
			UE_LOG(LogSurf, Warning, TEXT("  Component Velocity Z: %.3f"), v.Z);
			UE_LOG(LogSurf, Warning, TEXT(""));
		}
	}
	else
	{
		UE_LOG(LogSurf, Warning, TEXT("Could not find Damping console variables. Are you using the custom build engine?"));
	}
	if (this->debugComponentVelocity || SurfDebug::ShouldDebug(this, TEXT("component_velocity")))
	{
		FVector componentVelocity = (this->sharedCalculationsFront->componentVelocity + this->sharedCalculationsBack->componentVelocity)/2;
		FVector pointInWorldSpace = GetActorLocation();
		DrawDebugCone(
				GetWorld(),
				pointInWorldSpace,
				componentVelocity,
				componentVelocity.SizeSquared() * 0.000001,
				FMath::DegreesToRadians(0),
				FMath::DegreesToRadians(0),
				100, // Thickness
				FColor::Yellow,
				false,
				this->debugDrawDuration);
	}
}

