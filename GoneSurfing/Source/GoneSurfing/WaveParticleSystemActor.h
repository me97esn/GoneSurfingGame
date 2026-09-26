// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "WaveHeight.h"
#include "NiagaraComponent.h"
#include "WaveParticleSystemActor.generated.h"

UCLASS()
class GONESURFING_API AWaveParticleSystemActor : public AActor
{
	GENERATED_BODY()

public:
	// Sets default values for this actor's properties
	AWaveParticleSystemActor();

	// Niagara Actor spawned in the editor (we'll get the component from this)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particles")
	class ANiagaraActor* NiagaraActor;

	// Reference to the WaveHeight actor
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave Data")
	AWaveHeight* WaveHeight;

	// Distance between wave data sampling points
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave Data")
	float DistanceBetweenPoints = 100.0f;

	// Grid size for sampling (X direction)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave Data")
	int32 GridSizeX = 10;

	// Grid size for sampling (Y direction)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave Data")
	int32 GridSizeY = 10;

	// Row name for wave data lookup
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave Data")
	FName WaveDataRowName = "WaveData"; // TODO: Default name is wrong, look in the editor what it should be

	// Names of the Niagara user parameters to set (using Vector arrays - Array Float3)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particles")
	FName PositionArrayName = "PositionArray";

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particles")
	FName NormalArrayName = "NormalArray";

protected:
	// Called when the game starts or when spawned
	virtual void BeginPlay() override;

public:
	// Called every frame
	virtual void Tick(float DeltaTime) override;

private:
	// Cached Niagara component reference (retrieved from NiagaraActor)
	UNiagaraComponent* NiagaraComponent;

	// Update particle positions and orientations based on wave data
	void UpdateParticles();
};
