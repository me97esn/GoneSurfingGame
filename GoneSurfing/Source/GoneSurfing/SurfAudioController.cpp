// Fill out your copyright notice in the Description page of Project Settings.

#include "SurfAudioController.h"
#include "SprayController.h"
#include "SurfboardPawn.h"
#include "Components/AudioComponent.h"
#include "Sound/SoundBase.h"
#include "Kismet/GameplayStatics.h"
#include "EngineUtils.h"   // TActorIterator

DEFINE_LOG_CATEGORY_STATIC(LogSurfAudio, Log, All);

ASurfAudioController::ASurfAudioController()
{
	PrimaryActorTick.bCanEverTick = true;
	// Read after physics so relativeWaterVelocityMagnitude is this frame's finished value
	// (matches ASprayController, which reads the same post-physics accumulators).
	PrimaryActorTick.TickGroup = TG_PostPhysics;
	// Keep ticking while paused so we can drive the water to silence on pause (see Tick).
	PrimaryActorTick.bTickEvenWhenPaused = true;
}

bool ASurfAudioController::InitializeIfNeeded()
{
	if (bInitialized)
	{
		return true;
	}

	// Resolve the drive source if it wasn't wired up in-editor.
	if (!sharedCalculations)
	{
		for (TActorIterator<ASharedCalculations> It(GetWorld()); It; ++It)
		{
			sharedCalculations = *It;
			break;
		}
	}
	if (!sharedCalculations)
	{
		if (!bInitFailedWarned)
		{
			UE_LOG(LogSurfAudio, Warning, TEXT("SurfAudioController: no ASharedCalculations set or found — rush audio idle."));
			bInitFailedWarned = true;
		}
		return false;
	}

	// Optional: spray-hiss source. Missing is fine — spray hiss simply stays silent.
	if (!sprayController)
	{
		for (TActorIterator<ASprayController> It(GetWorld()); It; ++It)
		{
			sprayController = *It;
			break;
		}
	}

	if (!RushSound)
	{
		if (!bInitFailedWarned)
		{
			UE_LOG(LogSurfAudio, Warning, TEXT("SurfAudioController: RushSound not set — nothing to play. Assign a looping MetaSound Source."));
			bInitFailedWarned = true;
		}
		return false;
	}

	// Spawn the looping audio component and attach it to the board (falls back to this actor's root).
	RushAudioComponent = NewObject<UAudioComponent>(this);
	if (!RushAudioComponent)
	{
		return false;
	}
	RushAudioComponent->bAutoActivate = false;
	RushAudioComponent->SetSound(RushSound);
	RushAudioComponent->RegisterComponent();

	USceneComponent* AttachTarget = GetRootComponent();
	if (Surfboard && Surfboard->GetRootComponent())
	{
		AttachTarget = Surfboard->GetRootComponent();
	}
	if (AttachTarget)
	{
		RushAudioComponent->AttachToComponent(AttachTarget, FAttachmentTransformRules::KeepRelativeTransform);
	}
	RushAudioComponent->Play();

	bInitialized = true;
	UE_LOG(LogSurfAudio, Log, TEXT("SurfAudioController: initialized (SC=%s, board=%s)."),
		*sharedCalculations->GetName(), Surfboard ? *Surfboard->GetName() : TEXT("<none>"));
	return true;
}

void ASurfAudioController::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (!InitializeIfNeeded())
	{
		return;
	}

	// Water is silent while the game is paused AND for the whole duration of a replay (under kinematic
	// replay the drive scalars are stale, so playing water would be wrong). The music (a separate ambient
	// actor) keeps going. bTickEvenWhenPaused lets this Tick run during a world pause so we can push the 0s.
	bool bReplaying = false;
	if (const ASurfboardPawn* Pawn = Cast<ASurfboardPawn>(UGameplayStatics::GetPlayerPawn(this, 0)))
	{
		bReplaying = Pawn->bReplayActive;
	}
	const bool bActive = bAudioEnabled && !bReplaying && !(GetWorld() && GetWorld()->IsPaused());

	const float Mag = sharedCalculations->relativeWaterVelocityMagnitude;

	// Rush: smoothstep the raw rush speed into a 0..1 intensity. Below RushMinSpeed = silence at rest.
	float Intensity = 0.0f;
	if (bActive)
	{
		Intensity = FMath::SmoothStep(RushMinSpeed, FMath::Max(RushFullSpeed, RushMinSpeed + 1.0f), Mag);
	}

	// Spray hiss: summed spawn rates across all spray sites, normalised to 0..1.
	float SprayIntensity = 0.0f;
	if (bActive && sprayController)
	{
		FVector VelLeft, VelRight, VelTail;
		float RateLeft = 0.0f, RateRight = 0.0f, RateTail = 0.0f;
		sprayController->GetSprayOutputs(VelLeft, RateLeft, VelRight, RateRight, VelTail, RateTail);
		SprayIntensity = FMath::Clamp((RateLeft + RateRight + RateTail) / FMath::Max(1.0f, SpraySaturationRate), 0.0f, 1.0f);
	}

	// Submersion: how buried the board is (0..1) — muffles the whole mix via a low-pass in the graph.
	const float Submersion = FMath::Clamp(sharedCalculations->amountUnderWater, 0.0f, 1.0f);

	if (RushAudioComponent)
	{
		RushAudioComponent->SetFloatParameter(RushIntensityParam, Intensity);
		RushAudioComponent->SetFloatParameter(RushSpeedParam, Mag);
		RushAudioComponent->SetFloatParameter(SprayIntensityParam, SprayIntensity);
		RushAudioComponent->SetFloatParameter(SubmersionParam, Submersion);
	}

	if (bDebugAudio && (debugFrameCounter++ % FMath::Max(1, debugLogEveryNthTick) == 0))
	{
		UE_LOG(LogSurfAudio, Display,
			TEXT("SurfAudio: relWaterVelMag=%.0f -> Rush=%.2f | Spray=%.2f | Submersion=%.2f"),
			Mag, Intensity, SprayIntensity, Submersion);
	}
}
