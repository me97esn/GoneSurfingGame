// Copyright Epic Games, Inc. All Rights Reserved.

#include "TemporaryCameraPawn.h"
#include "SurfLog.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "InputMappingContext.h"

ATemporaryCameraPawn::ATemporaryCameraPawn()
{
	PrimaryActorTick.bCanEverTick = true;

	// Create scene root
	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	RootComponent = SceneRoot;

	// Create camera component
	CameraComponent = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	CameraComponent->SetupAttachment(RootComponent);

	// Default configuration
	MovementSpeed = 1000.0f;
	LookSensitivity = 1.0f;  // Mouse look sensitivity
	MobileLookSensitivity = 1.5f;  // Touch joystick sensitivity (normalized 0-1 input)

	// Initialize rotation
	CurrentRotation = FRotator::ZeroRotator;
	MovementInput = FVector::ZeroVector;

	// Use controller rotation for camera orientation
	bUseControllerRotationPitch = true;
	bUseControllerRotationYaw = true;
	bUseControllerRotationRoll = false;
}

void ATemporaryCameraPawn::BeginPlay()
{
	Super::BeginPlay();

	// Set initial rotation
	CurrentRotation = GetActorRotation();

	UE_LOG(LogSurf, Log, TEXT("TemporaryCameraPawn: BeginPlay - Platform: %s"), PLATFORM_ANDROID ? TEXT("Android") : TEXT("Other"));

	// Add Input Mapping Context
	if (APlayerController* PlayerController = Cast<APlayerController>(Controller))
	{
		UE_LOG(LogSurf, Log, TEXT("TemporaryCameraPawn: PlayerController found"));

		// Enable input
		PlayerController->SetShowMouseCursor(false);
		PlayerController->SetInputMode(FInputModeGameOnly());

		if (UEnhancedInputLocalPlayerSubsystem* Subsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PlayerController->GetLocalPlayer()))
		{
			UE_LOG(LogSurf, Log, TEXT("TemporaryCameraPawn: Enhanced Input Subsystem found"));

			if (InputMappingContext)
			{
				Subsystem->AddMappingContext(InputMappingContext, 0);
				UE_LOG(LogSurf, Log, TEXT("TemporaryCameraPawn: Added Input Mapping Context"));
			}
			else
			{
				UE_LOG(LogSurf, Warning, TEXT("TemporaryCameraPawn: No InputMappingContext assigned! Assign it in Blueprint or Editor."));
			}

			if (MoveAction)
			{
				UE_LOG(LogSurf, Log, TEXT("TemporaryCameraPawn: MoveAction is assigned"));
			}
			else
			{
				UE_LOG(LogSurf, Warning, TEXT("TemporaryCameraPawn: MoveAction is NOT assigned!"));
			}

			if (LookAction)
			{
				UE_LOG(LogSurf, Log, TEXT("TemporaryCameraPawn: LookAction is assigned"));
			}
			else
			{
				UE_LOG(LogSurf, Warning, TEXT("TemporaryCameraPawn: LookAction is NOT assigned!"));
			}
		}
		else
		{
			UE_LOG(LogSurf, Error, TEXT("TemporaryCameraPawn: Enhanced Input Subsystem NOT found!"));
		}
	}
	else
	{
		UE_LOG(LogSurf, Error, TEXT("TemporaryCameraPawn: No PlayerController!"));
	}
}

void ATemporaryCameraPawn::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	// Apply accumulated movement input
	if (!MovementInput.IsZero())
	{
		FVector Movement = MovementInput * MovementSpeed * DeltaTime;
		AddActorWorldOffset(Movement, true);
		MovementInput = FVector::ZeroVector;
	}
}

void ATemporaryCameraPawn::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);

	// Bind Enhanced Input actions
	if (UEnhancedInputComponent* EnhancedInputComponent = Cast<UEnhancedInputComponent>(PlayerInputComponent))
	{
		if (MoveAction)
		{
			EnhancedInputComponent->BindAction(MoveAction, ETriggerEvent::Triggered, this, &ATemporaryCameraPawn::Move);
		}
		else
		{
			UE_LOG(LogSurf, Warning, TEXT("TemporaryCameraPawn: No MoveAction assigned!"));
		}

		if (LookAction)
		{
			EnhancedInputComponent->BindAction(LookAction, ETriggerEvent::Triggered, this, &ATemporaryCameraPawn::Look);
		}
		else
		{
			UE_LOG(LogSurf, Warning, TEXT("TemporaryCameraPawn: No LookAction assigned!"));
		}
	}
}

void ATemporaryCameraPawn::Move(const FInputActionValue& Value)
{
	FVector2D MoveVector = Value.Get<FVector2D>();

	UE_LOG(LogSurf, Log, TEXT("TemporaryCameraPawn::Move called - X=%.2f, Y=%.2f"), MoveVector.X, MoveVector.Y);

	if (MoveVector.IsZero())
	{
		return;
	}

	// Get controller rotation (where the camera is looking)
	FRotator ControlRotation = Controller ? Controller->GetControlRotation() : GetActorRotation();

	// Calculate movement direction relative to controller rotation (ignore pitch)
	FRotator YawRotation(0, ControlRotation.Yaw, 0);

	// Forward/backward movement (Y input)
	FVector ForwardDirection = FRotationMatrix(YawRotation).GetUnitAxis(EAxis::X);
	MovementInput += ForwardDirection * MoveVector.Y;

	// Left/right movement (X input)
	FVector RightDirection = FRotationMatrix(YawRotation).GetUnitAxis(EAxis::Y);
	MovementInput += RightDirection * MoveVector.X;
}

void ATemporaryCameraPawn::Look(const FInputActionValue& Value)
{
	FVector2D LookVector = Value.Get<FVector2D>();

	// Determine sensitivity based on platform
	float Sensitivity = LookSensitivity;

#if PLATFORM_ANDROID || PLATFORM_IOS
	Sensitivity = MobileLookSensitivity;
#endif

	UE_LOG(LogSurf, Log, TEXT("TemporaryCameraPawn::Look called - X=%.2f, Y=%.2f, Sensitivity=%.2f"), LookVector.X, LookVector.Y, Sensitivity);

	if (LookVector.IsZero())
	{
		return;
	}

	// Use AddController input which properly handles pawn rotation
	// Note: Pitch needs to be inverted for mouse but NOT for gamepad/touch
	float PitchMultiplier = -1.0f;
#if PLATFORM_ANDROID || PLATFORM_IOS
	PitchMultiplier = 1.0f;  // Touch joysticks don't need inversion
#endif

	AddControllerYawInput(LookVector.X * Sensitivity);
	AddControllerPitchInput(LookVector.Y * Sensitivity * PitchMultiplier);
}
