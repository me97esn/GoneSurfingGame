# Temporary Camera Controller Specification

## Overview

A temporary camera control system for testing the InfiniteWaveManager on both mobile devices and PC. This allows free camera movement to test the infinite wave repositioning system before implementing the final gameplay camera.

**Last Updated:** December 19, 2024

---

## Requirements

### Functional Requirements

1. **Mobile Controls**
   - Use existing touch joysticks (automatically present in UE5 mobile)
   - Left joystick: Move camera forward/backward/left/right
   - Right joystick: Look around (rotate camera)
   - Movement should be smooth and responsive

2. **PC Controls**
   - WASD: Move camera forward/backward/left/right
   - Mouse: Look around (rotate camera)
   - Same movement speed and behavior as mobile

3. **Camera Behavior**
   - Free-flying camera (no collision)
   - Maintain consistent movement speed across platforms
   - Smooth rotation without jitter
   - Camera should be recognized by InfiniteWaveManager as the tracked actor

### Non-Functional Requirements

- Simple implementation (this is temporary)
- No complex camera systems or advanced features
- Easy to replace later with final gameplay camera
- Should not interfere with existing game systems

---

## Architecture

### Component Design

**TemporaryCameraController** (C++ or Blueprint)
- Inherits from `APlayerController` or uses a `APawn` with camera component
- Handles input from both mobile touch and PC keyboard/mouse
- Provides smooth movement and rotation
- Can be assigned to InfiniteWaveManager's `CameraActor` property

### Input System

Use **Enhanced Input System** (already included in project dependencies):
- Input Mapping Context for camera controls
- Input Actions for:
  - Move (2D axis: forward/back, left/right)
  - Look (2D axis: pitch/yaw)

---

## Technical Design

### Class Structure

```cpp
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

    /** Camera component */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Camera")
    TObjectPtr<UCameraComponent> CameraComponent;

    /** Scene root for camera */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Camera")
    TObjectPtr<USceneComponent> SceneRoot;

    // ========== Configuration ==========

    /** Movement speed (cm/s) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Movement")
    float MovementSpeed = 1000.0f;

    /** Look sensitivity for mouse/touch */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Look")
    float LookSensitivity = 1.0f;

    /** Mobile touch sensitivity multiplier */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Look")
    float MobileLookSensitivity = 50.0f;

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
};
```

### Input Configuration

**Input Mapping Context:** `/Game/Input/IMC_TemporaryCamera`

**Input Actions:**
- `IA_Move` - 2D Axis (Vector2D)
  - PC: WASD keys
  - Mobile: Left touch joystick (automatic)

- `IA_Look` - 2D Axis (Vector2D)
  - PC: Mouse movement
  - Mobile: Right touch joystick (automatic)

**Key Mappings:**
- W → Move forward (+Y)
- S → Move backward (-Y)
- A → Move left (-X)
- D → Move right (+X)
- Mouse X → Look right/left (Yaw)
- Mouse Y → Look up/down (Pitch)

---

## Implementation Details

### Movement Implementation

```cpp
void ATemporaryCameraPawn::Move(const FInputActionValue& Value)
{
    FVector2D MoveVector = Value.Get<FVector2D>();

    if (MoveVector.IsZero())
    {
        return;
    }

    // Calculate movement direction relative to current rotation
    FRotator YawRotation(0, CurrentRotation.Yaw, 0);

    // Forward/backward movement
    FVector ForwardDirection = FRotationMatrix(YawRotation).GetUnitAxis(EAxis::X);
    AddMovementInput(ForwardDirection, MoveVector.Y);

    // Left/right movement
    FVector RightDirection = FRotationMatrix(YawRotation).GetUnitAxis(EAxis::Y);
    AddMovementInput(RightDirection, MoveVector.X);
}
```

### Look Implementation

```cpp
void ATemporaryCameraPawn::Look(const FInputActionValue& Value)
{
    FVector2D LookVector = Value.Get<FVector2D>();

    if (LookVector.IsZero())
    {
        return;
    }

    // Determine sensitivity based on platform
    float Sensitivity = LookSensitivity;

    #if PLATFORM_ANDROID || PLATFORM_IOS
        Sensitivity = MobileLookSensitivity;
    #endif

    // Apply rotation
    CurrentRotation.Yaw += LookVector.X * Sensitivity * GetWorld()->GetDeltaSeconds();
    CurrentRotation.Pitch = FMath::Clamp(CurrentRotation.Pitch + LookVector.Y * Sensitivity * GetWorld()->GetDeltaSeconds(), -89.0f, 89.0f);

    SetActorRotation(CurrentRotation);
}
```

### Mobile Touch Joystick Setup

UE5 automatically provides touch joysticks on mobile when using Enhanced Input. The two virtual joysticks appear by default:
- **Left joystick**: Mapped to move input
- **Right joystick**: Mapped to look input

**Configuration in Project Settings:**
- `Project Settings → Engine → Input → Default Touch Interface`
- Should be set to `DefaultVirtualJoysticks` or similar
- Enhanced Input automatically maps these to Input Actions

---

## Integration with InfiniteWaveManager

### Setup Steps

1. **Create TemporaryCameraPawn actor** in level
2. **Set as default pawn** for GameMode
3. **Assign to InfiniteWaveManager**:
   - In InfiniteWaveManager BeginPlay, auto-assign player pawn as CameraActor:
     ```cpp
     if (!CameraActor)
     {
         APlayerController* PC = GetWorld()->GetFirstPlayerController();
         if (PC && PC->GetPawn())
         {
             CameraActor = PC->GetPawn();
         }
     }
     ```
   - Or manually assign in Blueprint

---

## Testing Procedure

### PC Testing
1. Play in Editor (PIE)
2. Use WASD to move camera around
3. Use mouse to look around
4. Move camera along Y-axis to test infinite wave repositioning
5. Verify InfiniteWaveManager logs show proper actor repositioning

### Mobile Testing
1. Deploy to Android device
2. Touch left joystick to move camera
3. Touch right joystick to look around
4. Move camera sideways (Y-axis) to test wave system
5. Verify smooth performance on device

### What to Test
- [ ] Camera moves in correct direction relative to its rotation
- [ ] Camera rotation is smooth without jitter
- [ ] Movement speed feels appropriate
- [ ] InfiniteWaveManager recognizes camera as tracked actor
- [ ] GridLODActors reposition correctly as camera moves
- [ ] No flickering or repositioning issues
- [ ] Same behavior on PC and mobile

---

## Configuration Parameters

| Parameter | Default | Description |
|-----------|---------|-------------|
| `MovementSpeed` | 1000.0 cm/s | How fast camera moves |
| `LookSensitivity` | 1.0 | Mouse look sensitivity on PC |
| `MobileLookSensitivity` | 50.0 | Touch joystick sensitivity on mobile |

**Tuning Notes:**
- `MovementSpeed`: Increase to move faster through the wave system
- `MobileLookSensitivity`: Mobile needs higher value because joystick returns normalized values (0-1)
- Keep movement speed high enough to trigger repositioning (need to move beyond `RepositionDistance`)

---

## Alternative: Blueprint-Only Implementation

If C++ is too heavy for a temporary solution, can implement entirely in Blueprint:

**Blueprint Class:** `BP_TemporaryCameraPawn`
- Parent: `Pawn`
- Components:
  - Scene root
  - Camera component
- Blueprint nodes:
  - Enhanced Input action bindings
  - Movement calculation using `AddActorWorldOffset`
  - Rotation using `AddControllerYawInput` / `AddControllerPitchInput`

This may be faster to iterate but less portable between projects.

---

## Migration Path

When implementing the final gameplay camera:

1. **Create new camera system** (e.g., `GameplayCameraController`)
2. **Update GameMode** to use new pawn class
3. **Update InfiniteWaveManager** to reference new camera
4. **Delete** `TemporaryCameraPawn` and related assets
5. **Remove** temporary input mapping context

No changes needed to InfiniteWaveManager itself - it only needs an `AActor` reference.

---

## Dependencies

### Required Modules
- `EnhancedInput` (already in `GoneSurfing.Build.cs`)
- `Engine`
- `CoreUObject`

### Required Assets
- Input Mapping Context asset
- Input Action assets (Move, Look)
- Camera pawn Blueprint or C++ class

---

## File Structure

```
Source/GoneSurfing/
├── TemporaryCameraPawn.h
├── TemporaryCameraPawn.cpp

Content/Input/
├── IMC_TemporaryCamera.uasset        # Input Mapping Context
├── Actions/
│   ├── IA_Move.uasset                # Move Input Action
│   └── IA_Look.uasset                # Look Input Action

Content/Blueprints/
└── BP_TemporaryCameraPawn.uasset     # Optional: Blueprint child class
```

---

## Known Limitations

- No collision detection (camera can pass through objects)
- No acceleration/deceleration (instant movement)
- No camera shake or advanced effects
- No zoom or field-of-view adjustment
- **This is intentional** - keep it simple for testing

---

## Success Criteria

- [X] Camera controls work on PC with keyboard/mouse
- [X] Camera controls work on mobile with touch joysticks
- [X] InfiniteWaveManager tracks camera properly
- [X] Can test infinite wave system by moving camera around
- [X] Implementation is simple and easy to remove later
- [X] No performance issues on mobile

---

## Metadata
- **Created**: 2024-12-19
- **Author**: Development Team
- **Priority**: Medium
- **Estimated Effort**: 1-2 hours
- **Related Components**: InfiniteWaveManager, Enhanced Input System
- **Related Specs**: [InfiniteWaveManager.spec.md](InfiniteWaveManager.spec.md)
