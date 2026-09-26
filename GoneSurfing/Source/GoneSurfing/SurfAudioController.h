// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "SharedCalculations.h"
#include "SurfAudioController.generated.h"

class UAudioComponent;
class USoundBase;

/**
 * Layer 1 (reactive board-water) surf audio — first proof.
 *
 * Drives a looping MetaSound from the per-tick water-rush scalar, exactly like ASprayController drives
 * its Niagara system: read a signal off ASharedCalculations, normalise it, push it as a MetaSound input
 * every tick. Output-only — never writes to the force pipeline or any gameplay state, so it cannot affect
 * snapshot/trace-replay CSVs (and is a harmless no-op on the headless audio-less device).
 *
 * MetaSound inputs written each tick (name them these in the graph, or rename via the params below):
 *   float RushIntensity  — 0..1, relativeWaterVelocityMagnitude smoothstepped over RushMinSpeed..RushFullSpeed.
 *                          Wire to the noise/bandpass gain (and cutoff) — this is the "how fast" knob.
 *   float RushSpeed      — raw relativeWaterVelocityMagnitude (cm/s), for pitch mapping if wanted.
 *   float SprayIntensity — 0..1, summed spray spawn-rates; wire to a second (brighter) noise layer's gain.
 *   float Submersion     — 0..1, amountUnderWater; wire to a low-pass cutoff over the whole mix (muffle).
 *
 * One instance per surfboard; place in the level and assign RushSound (a looping MetaSound Source).
 * See specs/surf-audio.md.
 */
UCLASS()
class GONESURFING_API ASurfAudioController : public AActor
{
	GENERATED_BODY()

public:
	ASurfAudioController();

	/** The surfboard the rush sound follows — the audio component attaches to its root component so the
	 *  sound is positioned at the board. Optional (falls back to this actor's location). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surf Audio")
	TObjectPtr<AActor> Surfboard;

	/** Supplies the drive scalar (relativeWaterVelocityMagnitude). Auto-resolved to the first
	 *  ASharedCalculations found in the level when left unset. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surf Audio")
	TObjectPtr<ASharedCalculations> sharedCalculations;

	/** The rush sound — a LOOPING MetaSound Source whose graph reads the RushIntensity/RushSpeed inputs. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surf Audio")
	TObjectPtr<USoundBase> RushSound;

	/** Master switch (live-toggleable). When off, RushIntensity is forced to 0 (graph should go silent). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surf Audio")
	bool bAudioEnabled = true;

	/** Normalisation window: relativeWaterVelocityMagnitude (cm/s) is smoothstepped over
	 *  [RushMinSpeed, RushFullSpeed] to produce RushIntensity 0..1. Below min = silence at rest.
	 *  Keep RushMinSpeed roughly in step with the spray gate (sprayMinSpeed) so audio and VFX agree. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surf Audio", meta = (ClampMin = "0.0"))
	float RushMinSpeed = 100.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surf Audio", meta = (ClampMin = "1.0"))
	float RushFullSpeed = 1500.0f;

	/** Optional spray-hiss source. Its summed spawn rates drive SprayIntensity. Auto-resolved to the
	 *  first ASprayController in the level when unset; spray hiss stays silent if none exists. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surf Audio")
	TObjectPtr<class ASprayController> sprayController;

	/** Summed spray spawn-rate (particles/s across all sites) that maps to full SprayIntensity (1.0). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surf Audio", meta = (ClampMin = "1.0"))
	float SpraySaturationRate = 300.0f;

	/** MetaSound input names the controller writes each tick (match these in the MetaSound graph). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surf Audio")
	FName RushIntensityParam = FName("RushIntensity");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surf Audio")
	FName RushSpeedParam = FName("RushSpeed");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surf Audio")
	FName SprayIntensityParam = FName("SprayIntensity");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surf Audio")
	FName SubmersionParam = FName("Submersion");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	bool bDebugAudio = false;

	/** Throttle for the debug UE_LOG — only log every Nth tick. 1 = every tick, 30 ≈ 2 Hz. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug", meta = (ClampMin = "1"))
	int32 debugLogEveryNthTick = 30;

	virtual void Tick(float DeltaTime) override;

private:
	/** Resolve sharedCalculations, spawn the looping audio component attached to the board, start it.
	 *  Runs on the first tick (not BeginPlay) so BP-assigned references have settled. True once ready. */
	bool InitializeIfNeeded();

	UPROPERTY(Transient)
	TObjectPtr<UAudioComponent> RushAudioComponent;

	bool bInitialized = false;
	bool bInitFailedWarned = false;
	int32 debugFrameCounter = 0;
};
