// Fill out your copyright notice in the Description page of Project Settings.
/**
 * WeightDistribution simulates player weight shifting on the surfboard.
 *
 * Applies downward impulses at four points around the weight center to create
 * realistic pitch (nose up/down) and roll (lean left/right) moments.
 *
 * The weight position is controlled externally via amountInFront and amountToTheRight.
 * This class only calculates the resulting impulse positions and magnitudes;
 * the actual physics integration is handled by the surfboard mesh's force queue.
 */

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Buoyancy.h"  // For FForceAndLocation struct
#include "WeightDistribution.generated.h"

class USurfTuningSubsystem;
class ASharedCalculations;

UCLASS()
class GONESURFING_API AWeightDistribution : public AActor
{
	GENERATED_BODY()

public:
	// Sets default values for this actor's properties
	AWeightDistribution();

	// Reference to the surfboard actor
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weight Distribution")
	AActor* Surfboard;

	// Weight position: 0.0 = tail, 0.5 = center, 1.0 = nose
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weight Distribution")
	float amountInFront = 0.5f;

	// Weight position: 0.0 = left rail, 0.5 = center, 1.0 = right rail
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weight Distribution")
	float amountToTheRight = 0.5f;

	// Invert Y-axis if mesh was imported rotated 180 degrees (tail/nose swapped)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weight Distribution")
	bool invertXAxis = false;  // Named invertXAxis for backward compatibility, but controls Y-axis

	// Invert X-axis (lateral) if left/right is reversed
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weight Distribution")
	bool invertLateralAxis = true;

	// Pump input from the pawn's accelerometer pipeline (0..1). Written by
	// ASurfboardPawn::UpdatePumpInput each tick; read by Tick to scale the
	// pump impulse. Bypassed when Tuning->TestPumpInput > 0 (Phase 0 override).
	// See specs/pumping.md.
	UPROPERTY(BlueprintReadWrite, Category = "Pump")
	float PumpInput = 0.0f;

	/** PumpInput after BOTH attenuations (speed and slope) - what the pump is actually worth this
	 *  tick. Anything outside this actor that gates on "is the rider injecting energy" must read
	 *  this, never PumpInput: the pawn rewrites PumpInput with the raw signal every tick and this
	 *  actor attenuates it in place, so a reader that ticks first sees the raw value. That is
	 *  exactly how the bottom hydrofoil's pumpGate came to sit wide open on flat water while the
	 *  impulse itself was attenuated to nothing. */
	float PumpInputAttenuated = 0.0f;

	/** The pump as the PLAYER experiences it, untouched by any attenuation: true while a stroke is
	 *  running, with its phase in 0..1. The force path deliberately taints PumpInput in place (speed
	 *  and slope attenuation, often to exactly zero), so anything that shows the player their pump -
	 *  the animation, the debug sphere - must read these instead. A press must always produce a
	 *  visible stroke, even where it can produce no speed. */
	bool  bPumpActive = false;
	float PumpStrokePhase = 0.0f;

	/** How far into the crouch the surfer is, 0..1. Drives the crunch-down clip. */
	float PumpCharge = 0.0f;

	/** True while the legs are extending after a release; PumpReleasePhase runs 0..1 across it.
	 *  Drives the rise-up clip, and is the window the downward impulse is applied over. */
	bool  bPumpReleasing = false;
	float PumpReleasePhase = 0.0f;

	/** How deep the crouch was at the moment of release, 0..1. The rise clip has to start from that
	 *  depth rather than from a full crouch, or a half-held pump pops to fully crouched the instant
	 *  the button comes up. */
	float PumpReleaseFromCharge = 0.0f;



	// Wired by SurfboardUtils::BeginPlay to the front SharedCalculations actor.
	// Used by the pump impulse path in Tick to read boardWideSlopeSin for the
	// slope-attenuation gate (pump fades to zero on steep wave faces).
	UPROPERTY()
	ASharedCalculations* sharedCalculations = nullptr;

	// Torque magnitude for rolling/pitching the board. Fallback only — the effective
	// value at runtime comes from USurfTuningSubsystem::WeightTorqueMagnitude (see
	// calculateWeightTorque). Kept in sync with that default.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weight Distribution")
	// Kept in sync with USurfTuningSubsystem::WeightTorqueMagnitude (which overrides this live).
	float torqueMagnitude = 4000.0f;

	// Maximum tilt angle (degrees) at which torque drops to zero
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weight Distribution", meta=(ClampMin = "1.0", ClampMax = "90.0"))
	float maxTiltAngle = 45.0f;

	// Distance between the 4 application points (cm)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weight Distribution")
	float offsetDistance = 30.0f;

	// Enable debug visualization
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	bool debug = false;

	// Scale factor for debug force arrows
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	float debugForceScale = 0.001f;

	/**
	 * Calculates a torque vector in world space based on weight position.
	 * Apply this as an angular impulse to roll/pitch the board without adding net downward force.
	 */
	UFUNCTION(BlueprintCallable, Category = "Surfing")
	FVector calculateWeightTorque();

protected:
	// Called when the game starts or when spawned
	virtual void BeginPlay() override;

	// Internal bounds calculation properties
	FVector surfboardBoundsMin;
	FVector surfboardBoundsMax;
	FVector surfboardBoundsExtent;

	/** Cached pointer to USurfTuningSubsystem; resolved in BeginPlay. Drives
	 *  the pump impulse application path in Tick. See specs/pumping.md. */
	UPROPERTY(Transient)
	USurfTuningSubsystem* Tuning = nullptr;

public:
	// Called every frame
	virtual void Tick(float DeltaTime) override;
};
