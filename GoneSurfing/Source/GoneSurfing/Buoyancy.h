// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "SharedCalculations.h"
#include "WaveHeight.h"
#include "Kismet/KismetStringLibrary.h"
#include "ForceQueueManager.h"
#include <deque>
#include "Buoyancy.generated.h"

class USurfTuningSubsystem;

USTRUCT(BlueprintType)
struct FForceAndLocation
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FVector force;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FVector forceLocation;
	// Constructor
	FForceAndLocation() : force(FVector::ZeroVector), forceLocation(FVector::ZeroVector) {}
};

USTRUCT(BlueprintType)
struct FForcesAndLocations
{
	GENERATED_BODY()
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	TArray<FForceAndLocation> forcesAndLocations;
};

UCLASS()
class GONESURFING_API ABuoyancy : public AActor
{
	GENERATED_BODY()
	
public:	
	// Sets default values for this actor's properties
	ABuoyancy();
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float debugDrawDuration;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	ASharedCalculations* sharedCalculations;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float distanceWhereMaxForceShouldBeApplied;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	bool debug = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	AActor* surfboard;

	FVector force;

	// How much the wave face normal influences buoyancy direction.
	// 0.0 = always world up, 1.0 = fully follow wave face normal.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(ClampMin = "0.0", ClampMax = "1.0"))
	float waveFaceNormalInfluence = 0.5f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly)
	FColor DebugColor = FColor::Green;

	UPROPERTY(EditAnywhere, BlueprintReadOnly)
	float debugDrawForceMultiplier = 0.000001;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	AWaveHeight* waveHeight;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float amountUnderWater = 0.0;

	// Front-bias for the horizontal-velocity buoyancy term. The per-actor force is
	// scaled by (1 + frontBias * clamp(forwardOffset / boardHalfLength, -1, 1)) so
	// nose actors get a bonus and tail actors a deficit. For symmetric actor
	// placement the sum of forces is unchanged (heave preserved); only the moment
	// about CoM picks up a forward bias, producing a planing-style nose-up AOA.
	// 0 = off; ~0.5 means 50% boost at the nose, 50% deficit at the tail.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(ClampMin = "-2.0", ClampMax = "2.0"))
	float horizontalVelocityBuoyancyFrontBias = 0.5f;

	// Reference length used to normalize the front-bias offset. Picks the cm
	// distance at which the bias hits its full magnitude. Roughly the board's
	// half-length (nose-to-CoM in cm).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(ClampMin = "1.0"))
	float boardHalfLength = 100.0f;

protected:
	// Called when the game starts or when spawned
	virtual void BeginPlay() override;

public:	
	// Called every frame
	virtual void Tick(float DeltaTime) override;

	// Coefficients live solely in USurfTuningSubsystem (single source of truth;
	// falls back to the subsystem CDO defaults if no game instance). The old
	// BP-passed-argument path (FluidDynamicsConstants) is removed.
	UFUNCTION(BlueprintCallable, Category="Surfing")
	FVector calculateBuoyancyForce(
		UPrimitiveComponent *BasePrimComp,
		FString rowName);

	FVector calcComponentVelocity(UPrimitiveComponent* BasePrimComp);

	float calcAmountUnderWater(float waveZLocation, float componentZLocation);

	UFUNCTION(BlueprintCallable, Category = "Surfing")
	TArray<FVector> getEnquedForces();

	UFUNCTION(BlueprintCallable, Category = "Surfing")
	void enqueForce(FVector forceToEnque, int number_of_chunks);

	/** Apply a buoyancy force as an impulse (force × clampedDeltaTime) on the surfboard mesh,
	 *  at this actor's world location. NaN-guarded; no-ops if BasePrimComp is null or the force
	 *  is near zero. DeltaTime is clamped to 33 ms so a PIE-startup hitch can't launch the board. */
	UFUNCTION(BlueprintCallable, Category = "Surfing")
	void applyForceAsImpulse(UPrimitiveComponent* BasePrimComp, FVector forceToApply, float DeltaTime);

private:
	std::deque<std::deque<FVector>> ForceQueue;

	// Cached at first call to calculateBuoyancyForce — actor's offset from the
	// surfboard root projected onto the board's forward axis. Constant because
	// the buoyancy actor is rigidly attached to the surfboard.
	float cachedForwardOffset = 0.0f;
	bool forwardOffsetCached = false;

	/** Cached pointer to USurfTuningSubsystem; resolved in BeginPlay. When
	 *  non-null, the four shared-by-design UPROPERTYs above are refreshed each
	 *  tick and calculateBuoyancyForce ignores its BP-passed arg values.
	 *  See specs/runtime-tuning.md Phase 2/3. */
	UPROPERTY(Transient)
	USurfTuningSubsystem* Tuning = nullptr;

	void RefreshFromTuningSubsystem();
};
