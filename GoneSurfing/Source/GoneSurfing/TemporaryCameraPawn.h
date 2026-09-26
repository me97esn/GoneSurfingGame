// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "Camera/CameraComponent.h"
#include "InputActionValue.h"
#include "TemporaryCameraPawn.generated.h"

class UInputMappingContext;
class UInputAction;

/**
 * Temporary free-flying camera pawn for testing InfiniteWaveManager
 * Supports both PC (WASD + Mouse) and Mobile (Touch Joysticks)
 */
UCLASS()
class GONESURFING_API ATemporaryCameraPawn : public APawn
{
	GENERATED_BODY()

public:
	ATemporaryCameraPawn();

protected:
	virtual void BeginPlay() override;

public:
	virtual void Tick(float DeltaTime) override;
	virtual void SetupPlayerInputComponent(class UInputComponent* PlayerInputComponent) override;

	// ========== Components ==========

	/** Scene root for camera */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Camera")
	TObjectPtr<USceneComponent> SceneRoot;

	/** Camera component */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Camera")
	TObjectPtr<UCameraComponent> CameraComponent;

	// ========== Configuration ==========

	/** Movement speed (cm/s) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Movement")
	float MovementSpeed;

	/** Look sensitivity for mouse/touch */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Look")
	float LookSensitivity;

	/** Mobile touch sensitivity multiplier */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Look")
	float MobileLookSensitivity;

	// ========== Enhanced Input ==========

	/** Input Mapping Context */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Camera|Input")
	TObjectPtr<UInputMappingContext> InputMappingContext;

	/** Move Input Action */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Camera|Input")
	TObjectPtr<UInputAction> MoveAction;

	/** Look Input Action */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Camera|Input")
	TObjectPtr<UInputAction> LookAction;

protected:
	// ========== Input Handlers ==========

	/** Handle move input */
	void Move(const FInputActionValue& Value);

	/** Handle look input */
	void Look(const FInputActionValue& Value);

private:
	/** Current rotation */
	FRotator CurrentRotation;

	/** Accumulated movement input for this frame */
	FVector MovementInput;
};
