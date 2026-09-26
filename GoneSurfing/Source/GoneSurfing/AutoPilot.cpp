// Fill out your copyright notice in the Description page of Project Settings.


#include "AutoPilot.h"
#include "SurfLog.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Engine/World.h"

// Sets default values
AAutoPilot::AAutoPilot()
{
 	// Set this actor to call Tick() every frame.  You can turn this off to improve performance if you don't need it.
	PrimaryActorTick.bCanEverTick = true;

}

void AAutoPilot::Start()
{
	if(!this->enabled){
		return;
	}

	if (this->steps.Num() == 0)
	{
		UE_LOG(LogSurf, Warning, TEXT("AutoPilot::Start - No steps defined!"));
		return;
	}

	// move to location and rotate to rotation if they are set
	UStaticMeshComponent* SurfboardStaticMesh = surfboard->FindComponentByClass<UStaticMeshComponent>();
	if (SurfboardStaticMesh)
	{
		if (!this->StartLocation.IsZero())
		{
			SurfboardStaticMesh->SetWorldLocation(this->StartLocation);
		}
		SurfboardStaticMesh->SetWorldRotation(this->StartRotation);
	}

	// Initialize timing state
	CurrentStepIndex = 0;
	ElapsedTimeInStep = 0.0f;
	bStarted = true;

	// Apply first step immediately
	auto firstStep = this->steps[0];
	UE_LOG(LogSurf, Display, TEXT(":::::: Setting initial weight: %s (duration: %.2fs)"),
		*firstStep.description, firstStep.duration);
	this->currentStep.description = firstStep.description;
	this->currentStep.weightRight = firstStep.weightRight;
	this->currentStep.weightNose = firstStep.weightNose;
	this->currentStep.jetEngineOn = firstStep.jetEngineOn;
	this->currentStep.jetMultiplier = firstStep.jetMultiplier;
}

// Called when the game starts or when spawned
void AAutoPilot::BeginPlay()
{
	Super::BeginPlay();
	this->Start();
}

void AAutoPilot::ChangeWeight(FString Description, float WeightRight, float WeightNose, bool JetEngineOn, float jetMultiplier)
{
	UE_LOG(LogSurf, Display, TEXT(":::::: Changing weight: %s"), *Description);

	if (JetEngineOn != this->currentStep.jetEngineOn)
	{
		UE_LOG(LogSurf, Warning, TEXT("JetEngine %s (multiplier: %.2f) - step: %s"),
			JetEngineOn ? TEXT("ON") : TEXT("OFF"), jetMultiplier, *Description);
	}

	this->currentStep.description = Description;
	this->currentStep.weightRight = WeightRight;
	this->currentStep.weightNose = WeightNose;
	this->currentStep.jetEngineOn = JetEngineOn;
	this->currentStep.jetMultiplier = jetMultiplier;
}

// Called every frame
void AAutoPilot::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	// Only advance if autopilot is started and enabled
	if (!bStarted || !enabled || steps.Num() == 0)
	{
		return;
	}

	// Check if we've completed all steps
	if (CurrentStepIndex >= steps.Num())
	{
		return;
	}

	// Accumulate time in current step (using simulation time, not real time)
	ElapsedTimeInStep += DeltaTime;

	// Check if current step duration has elapsed
	if (ElapsedTimeInStep >= steps[CurrentStepIndex].duration)
	{
		// Move to next step
		CurrentStepIndex++;

		// Check if there are more steps
		if (CurrentStepIndex < steps.Num())
		{
			// Reset elapsed time for new step
			ElapsedTimeInStep = 0.0f;

			// Apply the next step
			const FAutopilotStep& nextStep = steps[CurrentStepIndex];
			ChangeWeight(nextStep.description, nextStep.weightRight, nextStep.weightNose,
				nextStep.jetEngineOn, nextStep.jetMultiplier);
		}
		else
		{
			UE_LOG(LogSurf, Display, TEXT(":::::: AutoPilot completed all steps"));
			if (GetWorld() && GetWorld()->WorldType == EWorldType::Game)
			{
				UE_LOG(LogSurf, Display, TEXT(":::::: -game mode detected, calling QuitGame"));
				UKismetSystemLibrary::QuitGame(GetWorld(), nullptr, EQuitPreference::Quit, false);
			}
		}
	}
}

