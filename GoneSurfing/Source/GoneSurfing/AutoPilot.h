// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "AutoPilot.generated.h"

USTRUCT(BlueprintType)
struct FAutopilotStep
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FString description;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float duration = 2.0;
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(ClampMin = "0.0", ClampMax = "1.0") )
	float weightRight = 0.5;
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(ClampMin = "0.0", ClampMax = "1.0") )
	float weightNose = 0.5;

	UPROPERTY(EditAnywhere, BlueprintReadWrite  )
	bool jetEngineOn = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite  )
	float jetMultiplier = 1.0;	
};



UCLASS()
class GONESURFING_API AAutoPilot : public AActor
{
	GENERATED_BODY()
	
public:	
	// Sets default values for this actor's properties
	AAutoPilot();
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite )
	bool enabled;

	UPROPERTY(EditAnywhere, BlueprintReadWrite )
	bool includeInSuite = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FAutopilotStep currentStep;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	TArray<FAutopilotStep> steps;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FVector StartLocation;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FRotator StartRotation;

	UPROPERTY(EditAnywhere, BlueprintReadWrite )
	AActor* surfboard;

protected:
	// Called when the game starts or when spawned
	virtual void BeginPlay() override;

public:	
	// Called every frame
	virtual void Tick(float DeltaTime) override;
	
	UFUNCTION(BlueprintCallable, Category="Surfing")
	void ChangeWeight(FString Description, float WeightRight, float WeightNose, bool JetEngineOn, float jetMultiplier);
	void Start();

private:
	/** Current step index being executed */
	int32 CurrentStepIndex = 0;

	/** Elapsed time in current step (simulation time) */
	float ElapsedTimeInStep = 0.0f;

	/** Whether autopilot has been started */
	bool bStarted = false;

};


