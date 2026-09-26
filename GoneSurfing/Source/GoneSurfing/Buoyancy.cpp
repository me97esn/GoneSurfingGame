// Fill out your copyright notice in the Description page of Project Settings.

#include "Buoyancy.h"
#include "SurfLog.h"
#include "Components/PrimitiveComponent.h"
#include "SurfDebug.h"
#include "SurfRails.h"
#include "SurfTuningSubsystem.h"
#include <cmath> // For sin() and M_PI
#define _USE_MATH_DEFINES
#include <math.h>

// Sets default values
ABuoyancy::ABuoyancy()
{
 	// Set this actor to call Tick() every frame.  You can turn this off to improve performance if you don't need it.
	PrimaryActorTick.bCanEverTick = true;

}

void ABuoyancy::RefreshFromTuningSubsystem()
{
	if (!Tuning)
	{
		return;
	}
	distanceWhereMaxForceShouldBeApplied = Tuning->distanceWhereMaxForceShouldBeApplied;
	waveFaceNormalInfluence              = Tuning->waveFaceNormalInfluence;
	horizontalVelocityBuoyancyFrontBias  = Tuning->horizontalVelocityBuoyancyFrontBias;
	boardHalfLength                      = Tuning->boardHalfLength;
}

// Called when the game starts or when spawned
void ABuoyancy::BeginPlay()
{
	Super::BeginPlay();

	Tuning = SurfTuning::Get(this);
	RefreshFromTuningSubsystem();

	// One-shot dump of tunable defaults; see specs/runtime-tuning.md.
#if WITH_EDITOR
	const FString lbl = GetActorLabel();
#else
	const FString lbl = GetName();
#endif
	UE_LOG(LogSurf, Warning, TEXT("TuningDefaults: Buoyancy [%s] distanceWhereMaxForceShouldBeApplied=%g waveFaceNormalInfluence=%g horizontalVelocityBuoyancyFrontBias=%g boardHalfLength=%g"),
		*lbl, distanceWhereMaxForceShouldBeApplied, waveFaceNormalInfluence, horizontalVelocityBuoyancyFrontBias, boardHalfLength);
}

// Called every frame
void ABuoyancy::Tick(float DeltaTime)
{
	// Tick is called from derived BP classes
	Super::Tick(DeltaTime);
	RefreshFromTuningSubsystem();
}

void ABuoyancy::enqueForce(FVector forceToEnque, int number_of_chunks)
{
	ForceQueueManager::EnqueueForce(ForceQueue, forceToEnque, number_of_chunks);
}

TArray<FVector> ABuoyancy::getEnquedForces()
{
	return ForceQueueManager::GetEnqueuedForces(ForceQueue);
}

void ABuoyancy::applyForceAsImpulse(UPrimitiveComponent* BasePrimComp, FVector forceToApply, float DeltaTime)
{
	if (!BasePrimComp || forceToApply.ContainsNaN() || forceToApply.IsNearlyZero())
	{
		return;
	}
	// Clamp DeltaTime so a startup hitch (PIE-first-tick can span hundreds of ms)
	// can't multiply force into a launch-the-board impulse. 33 ms = 30 Hz floor;
	// normal-FPS gameplay is well under this so the clamp is inert in steady state.
	const float clampedDt = FMath::Min(DeltaTime, 0.033f);
	const FVector impulse = forceToApply * clampedDt;
	// Rails intro: still register into the force budget below (the diagnostic stays honest), just
	// don't push a kinematically-driven board. See specs/deterministic-ride-handoff.md.
	if (!SurfRails::AreForcesSuppressed())
	{
		BasePrimComp->AddImpulseAtLocation(impulse, GetActorLocation(), NAME_None);
	}
	// Register into the SharedCalculations force budget so the `torque` dump's vertical/into-wave
	// balance includes buoyancy alongside the FluidDynamics forces. See specs/submerged-downline-glide.md.
	if (this->sharedCalculations)
	{
		this->sharedCalculations->RegisterAppliedForce(TEXT("buoyancy"), GetActorLocation(), forceToApply);
	}
}

float ABuoyancy::calcAmountUnderWater(float waveZLocation, float componentZLocation)
{

	float surfaceDistance = componentZLocation - waveZLocation;
	float _amountUnderWater = 0.0f;

	if (this->debug || SurfDebug::ShouldDebug(this, TEXT("buoyancy")))
	{
		UE_LOG(LogSurf, Warning, TEXT("[%s] calcAmountUnderWater - waveZ: %.2f, compZ: %.2f, surfaceDist: %.2f, maxDist: %.2f"),
			*GetName(), waveZLocation, componentZLocation, surfaceDistance, this->distanceWhereMaxForceShouldBeApplied);
	}

	if (surfaceDistance < 0)
	{
		// negative distance => under water
		if (surfaceDistance < -this->distanceWhereMaxForceShouldBeApplied)
		{
			_amountUnderWater = 1;
		}
		else
		{
			_amountUnderWater = -surfaceDistance / this->distanceWhereMaxForceShouldBeApplied;
		}

		// Convert degrees to radians by multiplying with M_PI / 180.0
		double radians = ((_amountUnderWater * 180.0) - 90.0) * M_PI / 180.0;

		float sinusoidAmountUnderWater = (sin(radians) + 1.0) / 2.0;

		if (this->debug || SurfDebug::ShouldDebug(this, TEXT("buoyancy")))
		{
			UE_LOG(LogSurf, Warning, TEXT("[%s]   -> UNDERWATER: rawAmount: %.3f, sinusoidAmount: %.3f"),
				*GetName(), _amountUnderWater, sinusoidAmountUnderWater);
		}

		return sinusoidAmountUnderWater;
	}
	else
	{
		// above water
		if (this->debug || SurfDebug::ShouldDebug(this, TEXT("buoyancy")))
		{
			UE_LOG(LogSurf, Warning, TEXT("[%s]   -> ABOVE WATER: returning 0"), *GetName());
		}
		return 0;
	}
}

FVector ABuoyancy::calculateBuoyancyForce(
	UPrimitiveComponent *BasePrimComp,
	FString rowName)
{
	// Coefficients live solely in the tuning subsystem (single source of truth).
	// Falls back to the subsystem CDO — i.e. the header defaults — if no game
	// instance is up (e.g. editor-time calls).
	const USurfTuningSubsystem* T = Tuning ? Tuning : GetDefault<USurfTuningSubsystem>();
	const float basicFloatBuoyancyCoefficient         = T->basicFloatBuoyancyCoefficient;
	const float horizontalVelocityBuoyancyCoefficient = T->horizontalVelocityBuoyancyCoefficient;
	const float verticalVelocityBuoyancyCoefficient   = T->verticalVelocityBuoyancyCoefficient;
	const float amountUnderWaterPower                 = T->amountUnderWaterPower;
	const float weightForceMaxMultiplier              = T->weightForceMaxMultiplier;

	// Null check
	if (!this->waveHeight)
	{
		UE_LOG(LogSurf, Warning, TEXT("Buoyancy::calculateBuoyancyForce: waveHeight is null"));
		return FVector::ZeroVector;
	}

	// Extract frame number from rowName (format: "Frame_XXX" or just "XXX")
	int32 frameNumber = FCString::Atoi(*rowName);
	if (frameNumber == 0 && rowName.Contains(TEXT("Frame_")))
	{
		// Try to extract number after "Frame_"
		FString frameStr = rowName.Replace(TEXT("Frame_"), TEXT(""));
		frameNumber = FCString::Atoi(*frameStr);
	}

	auto locationAndNormal = this->waveHeight->calculateWaveLocationAndNormal(GetActorLocation(), frameNumber);
	FVector waveLocation = locationAndNormal[0];
	auto surfaceNormal = locationAndNormal[1];

	float componentZ = GetRootComponent()->GetComponentLocation().Z;
	float waveZ = waveLocation.Z;
	float _amountUnderWater = calcAmountUnderWater(waveZ, componentZ);

	// (The old per-SC amountWetted contact signal was removed — it was pegged at 1.0 and, being per-SC,
	// could not carry left/right. FluidDynamics now computes a per-actor AFluidDynamics::actorWetted from
	// each actor's own waterColumnAbove. See specs/per-actor-wetting.md.)

	if (this->debug || SurfDebug::ShouldDebug(this, TEXT("buoyancy")))
	{
		FVector actorLoc = GetActorLocation();
		UE_LOG(LogSurf, Warning, TEXT("[%s] Buoyancy - ActorLoc:(%.2f,%.2f,%.2f), ComponentZ: %.2f, WaveZ: %.2f, Diff: %.2f, AmountUnderWater: %.3f"),
			*GetName(), actorLoc.X, actorLoc.Y, actorLoc.Z, componentZ, waveZ, componentZ - waveZ, _amountUnderWater);
	}

	this->amountUnderWater = _amountUnderWater;
	this->sharedCalculations->amountUnderWater = this->amountUnderWater;

	float basicForceAmount = amountUnderWater * basicFloatBuoyancyCoefficient;
	FVector worldUp = FVector::UpVector;
	FVector buoyancyDirection = FMath::Lerp(worldUp, surfaceNormal, this->waveFaceNormalInfluence).GetSafeNormal();
	// Only use x and y component of the velocity to calculate horisontal buoyancy force. This doesn't have to be exact. I prefer not to use sqrt here, 
	// since that would require too much performance.
	// So I am trying out a super simple apx of Pythagoras theorem
	// Also: use component velocity rather then relative water velocity, because water velocity differs too much between frames.

	float horizontalVelocityMagnitude = abs(this->sharedCalculations->componentVelocity.X) + abs(this->sharedCalculations->componentVelocity.Y);

	// The horizontal velocity effects the buoyancy force, since a moving object pushes the water away, creating a wake.
	// The wake pushes back on the object = added buoyancy force.
	// If this force is world up, it interferes with the turns. Therefore local up is used.

	// Front-bias: nose actors get a bonus, tail actors a deficit, with sum
	// unchanged for symmetric placement. Models the forward shift of pressure
	// center on real planing surfaces (the "hang ten" effect) and counteracts
	// the bottomLift per-actor amountUnderWater nose-down feedback. See
	// nose-dive-bug.md for the analysis.
	if (!forwardOffsetCached && surfboard && sharedCalculations)
	{
		FVector worldOffset = GetActorLocation() - surfboard->GetActorLocation();
		FVector boardForwardWorld = sharedCalculations->forwards.GetSafeNormal();
		cachedForwardOffset = FVector::DotProduct(worldOffset, boardForwardWorld);
		forwardOffsetCached = true;
	}
	float frontBiasFactor = 1.0f;
	if (horizontalVelocityBuoyancyFrontBias != 0.0f && boardHalfLength > 0.0f && forwardOffsetCached)
	{
		float normalizedOffset = FMath::Clamp(cachedForwardOffset / boardHalfLength, -1.0f, 1.0f);
		frontBiasFactor = FMath::Max(0.0f, 1.0f + horizontalVelocityBuoyancyFrontBias * normalizedOffset);
	}

	// Clamp to make sure that the force is not causing the board to gain speed upwards. The buoyancy should only slow it down downwards.
	float _horizontalVelocityForceAmountWithoutAmountUnderWater = horizontalVelocityMagnitude * horizontalVelocityBuoyancyCoefficient * frontBiasFactor;
	float _horizontalVelocityForceAmount = _horizontalVelocityForceAmountWithoutAmountUnderWater * amountUnderWater;

	this->sharedCalculations->AddPlaningForce(_horizontalVelocityForceAmountWithoutAmountUnderWater * buoyancyDirection);

	float horizontalVelocityForceAmount = FMath::Clamp(_horizontalVelocityForceAmount, 0.0f, weightForceMaxMultiplier);

	FVector horizontalVelocityForce = buoyancyDirection * horizontalVelocityForceAmount;

	// The vertical velocity effects the buoyancy force, since higher velocity downwards means that the water pushes with higher force. Negative velocity means downwards.
	// Positive Z relative water velocity means that the board is falling (should have higher buoyancy force), negative means that it is rising (should have lower buoyancy force).
	auto verticalVelocityForceAmount = verticalVelocityBuoyancyCoefficient * amountUnderWater * this->sharedCalculations->relativeWaterVelocity.Z;

	auto verticalVelocityForce = buoyancyDirection * verticalVelocityForceAmount;

	auto basicForce = buoyancyDirection * basicForceAmount;
	FVector buoyancyForce = basicForce + verticalVelocityForce + horizontalVelocityForce;

	// DEBUG: Log force components to understand sudden drops
	if (debug || SurfDebug::ShouldDebug(this, TEXT("buoyancy")))
	{

#if WITH_EDITOR
		UE_LOG(LogSurf, Warning, TEXT("[%s (%s)] FORCE COMPONENTS:"), *GetName(), *GetActorLabel());
#else
		UE_LOG(LogSurf, Warning, TEXT("[%s] FORCE COMPONENTS:"), *GetName());
#endif
		UE_LOG(LogSurf, Warning, TEXT("  buoyancyDirection: (%.3f, %.3f, %.3f)"), buoyancyDirection.X, buoyancyDirection.Y, buoyancyDirection.Z);
		UE_LOG(LogSurf, Warning, TEXT("  basicForce: (%.2f, %.2f, %.2f) [amount=%.2f]"), basicForce.X, basicForce.Y, basicForce.Z, basicForceAmount);
		UE_LOG(LogSurf, Warning, TEXT("  verticalVelForce: (%.2f, %.2f, %.2f) [amount=%.2f, relVelZ=%.2f]"),
			verticalVelocityForce.X, verticalVelocityForce.Y, verticalVelocityForce.Z, verticalVelocityForceAmount, this->sharedCalculations->relativeWaterVelocity.Z);
		UE_LOG(LogSurf, Warning, TEXT("  horizVelForce: (%.2f, %.2f, %.2f) [amount=%.2f, frontBiasFactor=%.3f, forwardOffset=%.1fcm]"), horizontalVelocityForce.X, horizontalVelocityForce.Y, horizontalVelocityForce.Z, horizontalVelocityForceAmount, frontBiasFactor, cachedForwardOffset);
		UE_LOG(LogSurf, Warning, TEXT("  TOTAL buoyancyForce: (%.2f, %.2f, %.2f)"), buoyancyForce.X, buoyancyForce.Y, buoyancyForce.Z);
	}

	if (debug || SurfDebug::ShouldDebug(this, TEXT("buoyancy")))
	{
		FVector buoyancyWorldLocation = GetActorLocation();

		DrawDebugCone(
			GetWorld(),
			buoyancyWorldLocation,
			basicForce,
			basicForceAmount * this->debugDrawForceMultiplier,
			FMath::DegreesToRadians(0),
			FMath::DegreesToRadians(0),
			100, // Thickness
			this->DebugColor,
			false,
			this->debugDrawDuration);

		DrawDebugCone(
			GetWorld(),
			buoyancyWorldLocation,
			verticalVelocityForce,
			verticalVelocityForceAmount * this->debugDrawForceMultiplier,
			FMath::DegreesToRadians(5),
			FMath::DegreesToRadians(5),
			20, // Thickness
			FColor(this->DebugColor.R * 0.7, this->DebugColor.G * 0.7, this->DebugColor.B * 0.7),
			false,
			this->debugDrawDuration);

		DrawDebugCone(
			GetWorld(),
			buoyancyWorldLocation,
			horizontalVelocityForce,
			horizontalVelocityForceAmount * this->debugDrawForceMultiplier,
			FMath::DegreesToRadians(10),
			FMath::DegreesToRadians(10),
			10, // Thickness
			FColor(this->DebugColor.R * 0.4, this->DebugColor.G * 0.4, this->DebugColor.B * 0.4),
			false,
			this->debugDrawDuration);
	}

	return buoyancyForce;
}
