// Fill out your copyright notice in the Description page of Project Settings.

#include "WaveParticleSystemActor.h"
#include "SurfLog.h"
#include "NiagaraComponent.h"
#include "NiagaraDataInterfaceArrayFunctionLibrary.h"
#include "NiagaraSystem.h"
#include "NiagaraActor.h"

// Sets default values
AWaveParticleSystemActor::AWaveParticleSystemActor()
{
	// Set this actor to call Tick() every frame
	PrimaryActorTick.bCanEverTick = true;

	// Create the root component
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("RootComponent"));

	// Initialize pointers
	NiagaraActor = nullptr;
	NiagaraComponent = nullptr;
}

// Called when the game starts or when spawned
void AWaveParticleSystemActor::BeginPlay()
{
	Super::BeginPlay();

	if (!WaveHeight)
	{
		UE_LOG(LogSurf, Warning, TEXT("WaveParticleSystemActor: WaveHeight reference is not set!"));
	}

	// Get the Niagara component from the NiagaraActor
	if (NiagaraActor)
	{
		NiagaraComponent = NiagaraActor->GetNiagaraComponent();
		if (!NiagaraComponent)
		{
			UE_LOG(LogSurf, Warning, TEXT("WaveParticleSystemActor: Could not get NiagaraComponent from NiagaraActor!"));
		}
		else if (!NiagaraComponent->GetAsset())
		{
			UE_LOG(LogSurf, Warning, TEXT("WaveParticleSystemActor: NiagaraComponent has no Niagara System asset assigned."));
		}
	}
	else
	{
		UE_LOG(LogSurf, Warning, TEXT("WaveParticleSystemActor: NiagaraActor reference is not set! Please assign a NiagaraActor in the editor."));
	}
}

// Called every frame
void AWaveParticleSystemActor::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	// Update particles based on wave data
	UpdateParticles();
}

void AWaveParticleSystemActor::UpdateParticles()
{
	// Verify we have the required references
	if (!WaveHeight || !NiagaraComponent)
	{
		return;
	}

	// Get the current actor location
	FVector actorLocation = GetActorLocation();

	// Extract frame number from WaveDataRowName (format: "Frame_XXX" or just "XXX")
	FString rowNameStr = WaveDataRowName.ToString();
	int32 frameNumber = FCString::Atoi(*rowNameStr);
	if (frameNumber == 0 && rowNameStr.Contains(TEXT("Frame_")))
	{
		// Try to extract number after "Frame_"
		FString frameStr = rowNameStr.Replace(TEXT("Frame_"), TEXT(""));
		frameNumber = FCString::Atoi(*frameStr);
	}

	// Fetch wave data around the actor's location
	TArray<TTuple<double, FVector>> waveData = WaveHeight->GetWaveDataAroundLocation(
		actorLocation,
		frameNumber,
		DistanceBetweenPoints,
		GridSizeX,
		GridSizeY
	);

	// Prepare Vector arrays to send to Niagara (using Array Float3 data interface)
	TArray<FVector> positionArray;
	TArray<FVector> normalArray;

	// Calculate starting offset for positioning particles
	float startOffsetX = -(GridSizeX - 1) * DistanceBetweenPoints / 2.0f;
	float startOffsetY = -(GridSizeY - 1) * DistanceBetweenPoints / 2.0f;

	int32 index = 0;

	// Iterate through the grid and build the data arrays
	for (int32 y = 0; y < GridSizeY; ++y)
	{
		for (int32 x = 0; x < GridSizeX; ++x)
		{
			if (index < waveData.Num())
			{
				// Get the wave height and normal for this grid point
				double waveHeight = waveData[index].Get<0>();
				FVector waveNormal = waveData[index].Get<1>();

				// Calculate the particle position
				float particleX = actorLocation.X + startOffsetX + (x * DistanceBetweenPoints);
				float particleY = actorLocation.Y + startOffsetY + (y * DistanceBetweenPoints);
				float particleZ = static_cast<float>(waveHeight);

				// Add to Vector arrays
				positionArray.Add(FVector(particleX, particleY, particleZ));
				normalArray.Add(waveNormal);

				index++;
			}
		}
	}

	// Send the arrays to Niagara via Data Interface Array Function Library
	// These parameter names must match the Array Float3 Data Interface parameters in your Niagara system
	if (NiagaraComponent && NiagaraComponent->GetAsset())
	{
		UNiagaraDataInterfaceArrayFunctionLibrary::SetNiagaraArrayVector(NiagaraComponent, PositionArrayName, positionArray);
		UNiagaraDataInterfaceArrayFunctionLibrary::SetNiagaraArrayVector(NiagaraComponent, NormalArrayName, normalArray);

		UE_LOG(LogSurf, Verbose, TEXT("WaveParticleSystemActor: Updated %d particles and sent data to Niagara"), waveData.Num());
	}
}
