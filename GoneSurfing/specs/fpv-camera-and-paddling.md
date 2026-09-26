# Spec: FPV Camera and Paddling System

## Overview
Implement a first-person view (FPV) camera system attached to the surfboard with stabilization, and a paddling control system that allows the player to propel the surfboard forward using joystick input. This replaces the current free-flying TemporaryCameraPawn with an attached camera that follows the surfboard's movement while providing a stable viewing experience.

## Objective
Create an immersive surfing experience by:
1. Attaching the camera to the surfboard with configurable offset
2. Stabilizing camera rotation to prevent excessive bouncing and rolling
3. Implementing intuitive paddling controls using right joystick
4. Providing smooth camera rotation relative to surfboard orientation

## Requirements

### Functional Requirements (FR)

#### FR1: Camera Attachment
- Camera must be attached to the surfboard actor
- Camera position offset from surfboard origin must be configurable (default: head height above board)
- Camera must follow surfboard's world position

#### FR2: Camera Stabilization
- **Yaw**: Camera follows surfboard yaw rotation with smooth interpolation/lag
  - Fixed smoothing value to start (can be adjusted later)
  - Prevents jarring camera movement during quick surfboard turns
- **Pitch**: Camera does NOT follow surfboard pitch (remains level/stabilized)
- **Roll**: Camera does NOT follow surfboard roll (remains level/stabilized)
- Camera stabilization should handle surfboard vertical movement without excessive bouncing

#### FR3: Camera Control (Left Joystick)
- Left joystick controls camera yaw rotation only
- Camera yaw is **relative to surfboard** (local rotation)
  - Example: Pushing left rotates camera left relative to surfboard's current facing
  - If surfboard rotates, camera maintains relative angle to board
- Pitch and roll control removed (previously supported by TemporaryCameraPawn)
- Camera yaw rotation = Surfboard yaw + Player input offset

#### FR4: Paddling Control (Right Joystick)
- Right joystick up applies forward force to surfboard
- Force magnitude proportional to joystick input (0-100%)
- Force is continuous while joystick held up
- Force direction: Always surfboard's forward vector (X-axis)
- Force application point: Center of surfboard mesh
- Right joystick down/left/right: No action (reserved for future features)

#### FR5: Input System
- Must support both PC and mobile platforms (already implemented in TemporaryCameraPawn)
- Use Unreal's Enhanced Input System
- Create new Input Actions:
  - Paddle action (right joystick/WASD up)
  - Update Look action to only affect yaw
- Maintain existing Input Mapping Context structure

#### FR6: Control Setup
- Since no game controls currently exist, implement basic player controller setup:
  - Create/configure PlayerController
  - Set up Enhanced Input subsystem
  - Configure input mode (game only, hide cursor)
  - Handle both PC and mobile input

### Non-Functional Requirements (NFR)

#### NFR1: Performance
- Camera updates should run at 60+ FPS on target platforms
- Stabilization calculations should be lightweight (use interpolation, not complex physics)

#### NFR2: Platform Support
- Must work on both PC (Windows) and Mobile (Android)
- Input sensitivity should be configurable per platform

#### NFR3: Configurability
- All key parameters should be exposed as UPROPERTY EditAnywhere:
  - Camera offset from surfboard
  - Yaw smoothing factor
  - Paddle force magnitude
  - Look sensitivity (yaw only)
  - Platform-specific sensitivities

#### NFR4: Code Quality
- Follow existing Unreal C++ coding standards
- Use proper component architecture (SceneComponent hierarchy)
- Include comprehensive logging for debugging
- Comment complex stabilization logic

## Architecture

### Current Surfboard Setup
**Important:** The surfboard is a `StaticMeshActor`, which **cannot** be possessed by a PlayerController. Only `APawn` subclasses can receive player input. Therefore, we must create a separate Pawn that references and controls the surfboard.

### Component Structure (Required Approach)

```
SurfboardPawn (new APawn subclass - receives player input)
├── RootComponent (USceneComponent)
│   └── CameraComponent (UCameraComponent)
└── SurfboardReference (TSoftObjectPtr<AStaticMeshActor>)
    └── [Points to existing surfboard StaticMeshActor in level]

SurfboardStaticMeshActor (existing - physics simulation)
└── StaticMeshComponent (existing physics mesh)
```

**Why this structure:**
- `SurfboardPawn` is possessed by PlayerController and handles all input
- `SurfboardPawn` contains the camera and camera stabilization logic
- `SurfboardPawn` applies forces to the referenced surfboard's StaticMeshComponent
- Surfboard remains a StaticMeshActor for physics simulation
- Camera follows surfboard position/rotation each frame via reference

### Class Responsibilities

**SurfboardPawn (new APawn class)**
- Possessed by PlayerController to receive player input
- Contains and manages camera component
- Stores reference to surfboard StaticMeshActor
- Handles look input (yaw rotation relative to surfboard)
- Handles paddle input (applies forces to surfboard's StaticMeshComponent)
- Tick: Updates camera position/rotation to follow surfboard with stabilization

**SurfboardStaticMeshActor (existing)**
- Remains unchanged as StaticMeshActor
- Handles physics simulation via existing systems (SurfboardUtils, FluidDynamics, etc.)
- Receives forces from SurfboardPawn's paddle input

**Camera Stabilization Logic**
- Each frame:
  1. Get surfboard's world transform
  2. Extract surfboard yaw, ignore pitch/roll
  3. Interpolate camera yaw toward (surfboard yaw + player yaw offset)
  4. Set camera rotation to stabilized rotation
  5. Update camera position to surfboard position + offset

**Paddling System**
- Paddle input handler:
  1. Read right joystick Y-axis value (0 to 1 when pushed up)
  2. Calculate force: `PaddleForce * JoystickValue`
  3. Get surfboard forward vector
  4. Apply force to surfboard mesh: `AddForceAtLocation(ForwardVector * Force, CenterLocation)`

## Implementation Details

### Files to Create/Modify

**New Files:**
- `Source/GoneSurfing/SurfboardPawn.h` - Pawn class that receives player input
- `Source/GoneSurfing/SurfboardPawn.cpp` - Implementation

**Modified Files:**
- Input Mapping Context (Blueprint/Content asset) - Add Paddle action
- GameMode - Set SurfboardPawn as default pawn class
- Level Blueprint or GameMode - Set up reference from SurfboardPawn to existing surfboard actor

**Unchanged Files:**
- Surfboard StaticMeshActor remains unchanged
- SurfboardUtils and physics systems remain unchanged

### Key Implementation Points

#### Camera Stabilization Algorithm
```cpp
void Tick(float DeltaTime)
{
    // Validate surfboard reference
    if (!SurfboardActor)
        return;

    // Get surfboard's StaticMeshComponent
    UStaticMeshComponent* SurfboardMesh = SurfboardActor->GetStaticMeshComponent();
    if (!SurfboardMesh)
        return;

    // Get surfboard transform
    FTransform SurfboardTransform = SurfboardMesh->GetComponentTransform();
    FRotator SurfboardRotation = SurfboardTransform.GetRotation().Rotator();

    // Extract only yaw from surfboard
    float SurfboardYaw = SurfboardRotation.Yaw;

    // Calculate target camera yaw (surfboard yaw + player offset)
    float TargetYaw = SurfboardYaw + PlayerYawOffset;

    // Smooth interpolation
    float CurrentYaw = CameraComponent->GetComponentRotation().Yaw;
    float SmoothedYaw = FMath::FInterpTo(CurrentYaw, TargetYaw, DeltaTime, YawSmoothingSpeed);

    // Set stabilized rotation (pitch and roll = 0)
    FRotator StabilizedRotation(0.0f, SmoothedYaw, 0.0f);
    CameraRoot->SetWorldRotation(StabilizedRotation);

    // Update position with offset
    FVector SurfboardLocation = SurfboardTransform.GetLocation();
    FVector CameraLocation = SurfboardLocation + CameraOffset;
    CameraRoot->SetWorldLocation(CameraLocation);
}
```

#### Paddling Force Application
```cpp
void Paddle(const FInputActionValue& Value)
{
    // Validate surfboard reference
    if (!SurfboardActor)
        return;

    UStaticMeshComponent* SurfboardMesh = SurfboardActor->GetStaticMeshComponent();
    if (!SurfboardMesh)
        return;

    float JoystickValue = Value.Get<FVector2D>().Y; // Y-axis (up/down)

    if (JoystickValue <= 0.0f)
        return; // Only respond to upward input

    // Calculate force magnitude
    float ForceMagnitude = PaddleForceStrength * JoystickValue;

    // Get surfboard forward direction
    FVector ForwardVector = SurfboardMesh->GetForwardVector();
    FVector Force = ForwardVector * ForceMagnitude;

    // Apply force at center of mass
    FVector ForceLocation = SurfboardMesh->GetComponentLocation();
    SurfboardMesh->AddForceAtLocation(Force, ForceLocation);
}
```

#### Look Control (Yaw Only)
```cpp
void Look(const FInputActionValue& Value)
{
    FVector2D LookVector = Value.Get<FVector2D>();

    // Only use X-axis for yaw
    float YawInput = LookVector.X * LookSensitivity;

    // Update player yaw offset (relative to surfboard)
    PlayerYawOffset += YawInput;

    // Optional: Clamp to -180/+180 range
    PlayerYawOffset = FMath::Fmod(PlayerYawOffset + 180.0f, 360.0f) - 180.0f;
}
```

### Configuration Properties

```cpp
// Camera Configuration
UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera")
FVector CameraOffset = FVector(0.0f, 0.0f, 180.0f); // ~Head height in cm

UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera")
float YawSmoothingSpeed = 5.0f; // Interp speed for yaw smoothing

UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera")
float LookSensitivity = 1.0f;

UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera")
float MobileLookSensitivity = 1.5f;

// Paddling Configuration
UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Paddling")
float PaddleForceStrength = 10000.0f; // Force in Newtons

UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Paddling")
bool bDebugPaddleForce = false;

// Surfboard Reference
UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surfboard")
TObjectPtr<AStaticMeshActor> SurfboardActor; // Reference to existing surfboard in level

// Internal state
UPROPERTY()
float PlayerYawOffset = 0.0f; // Player's yaw rotation relative to surfboard
```

### Input Mapping

**Input Actions:**
- `IA_Look` - Vector2D - Left joystick, mouse (yaw only now)
- `IA_Paddle` - Vector2D - Right joystick, WASD (new)

**Input Mapping Context:**
- Left Joystick → IA_Look (X-axis for yaw)
- Right Joystick → IA_Paddle (Y-axis for forward paddling)
- Mouse Move → IA_Look (for PC)
- WASD → IA_Paddle (for PC testing)

## Acceptance Criteria

### AC1: Camera Follows Surfboard Position
**Given** the surfboard is moving in the world
**When** the game is running
**Then** the camera position should follow the surfboard with the configured offset

### AC2: Camera Yaw Follows Surfboard with Smoothing
**Given** the surfboard rotates yaw 90 degrees
**When** observing the camera rotation
**Then** the camera should smoothly interpolate to the new yaw over several frames

### AC3: Camera Ignores Surfboard Pitch and Roll
**Given** the surfboard pitches forward 30 degrees
**When** observing the camera rotation
**Then** the camera pitch should remain at 0 degrees (level)

**Given** the surfboard rolls left 45 degrees
**When** observing the camera rotation
**Then** the camera roll should remain at 0 degrees (level)

### AC4: Left Joystick Controls Yaw Relative to Surfboard
**Given** the player pushes left joystick left
**When** the camera rotates
**Then** the camera should rotate left relative to the surfboard's current facing

**Given** the surfboard then rotates
**When** observing the camera
**Then** the camera should maintain its relative angle to the surfboard

### AC5: Right Joystick Up Applies Paddling Force
**Given** the player pushes right joystick fully up
**When** observing the surfboard
**Then** a forward force should be applied to the surfboard mesh

**Given** the player pushes right joystick halfway up
**When** observing the surfboard
**Then** a forward force at 50% magnitude should be applied

**Given** the player releases the right joystick
**When** observing the surfboard
**Then** no paddling force should be applied

### AC6: Paddling Force Direction Matches Surfboard
**Given** the surfboard is facing direction D
**When** the player paddles
**Then** the force should be applied in direction D (surfboard forward)

### AC7: Works on PC and Mobile
**Given** the game is running on PC
**When** using mouse and WASD
**Then** camera and paddling controls should work correctly

**Given** the game is running on Android
**When** using touch joysticks
**Then** camera and paddling controls should work correctly

## Test Cases

### Test 1: Camera Offset Configuration
**Input:** Set CameraOffset to (0, 0, 200)
**Expected:** Camera should be 200cm above surfboard origin

### Test 2: Yaw Smoothing
**Input:** Surfboard instantly rotates 90° yaw
**Expected:** Camera smoothly interpolates over ~0.5-1 second (depending on YawSmoothingSpeed)

### Test 3: Pitch Stabilization
**Input:** Surfboard pitches up 45°
**Expected:** Camera pitch remains 0°, view stays level

### Test 4: Roll Stabilization
**Input:** Surfboard rolls right 60°
**Expected:** Camera roll remains 0°, horizon stays level

### Test 5: Relative Yaw Control
**Setup:** Surfboard facing North (yaw 0°)
**Input:** Push left joystick left
**Expected:** Camera rotates to face West relative to board
**Then:** Surfboard rotates to East (yaw 90°)
**Expected:** Camera now faces South (maintaining relative angle)

### Test 6: Paddling Force Magnitude
**Input:** Right joystick 100% up
**Expected:** Force = PaddleForceStrength * 1.0
**Input:** Right joystick 50% up
**Expected:** Force = PaddleForceStrength * 0.5

### Test 7: Paddling Direction
**Setup:** Surfboard facing East
**Input:** Right joystick up
**Expected:** Force applied in East direction (surfboard forward vector)

### Test 8: No Paddling on Down/Left/Right
**Input:** Right joystick down
**Expected:** No force applied
**Input:** Right joystick left or right
**Expected:** No force applied

## Migration from TemporaryCameraPawn

### What to Keep
- Enhanced Input System setup
- Platform detection (PLATFORM_ANDROID)
- Input sensitivity configuration
- PlayerController setup (input mode, cursor visibility)

### What to Remove
- Free-fly movement (Move action with XYZ translation)
- Pitch and roll control in Look action
- bUseControllerRotationPitch/Roll

### What to Add
- Surfboard reference and mesh lookup
- Camera stabilization tick logic
- Paddling input action and handler
- Yaw-only look control relative to surfboard

## Debug Features

### Debug Visualization
```cpp
if (bDebugPaddleForce)
{
    DrawDebugLine(World, ForceLocation, ForceLocation + Force * 0.01f,
                  FColor::Yellow, false, 0.1f, 0, 5.0f);

    DrawDebugString(World, ForceLocation + FVector(0, 0, 100),
                   FString::Printf(TEXT("Paddle: %.0f N"), ForceMagnitude),
                   nullptr, FColor::Yellow, 0.1f);
}
```

### Debug Logging
```cpp
UE_LOG(LogTemp, Log, TEXT("SurfboardPawn: Camera Yaw=%.1f, Surfboard Yaw=%.1f, Offset=%.1f"),
       CameraYaw, SurfboardYaw, PlayerYawOffset);

UE_LOG(LogTemp, Log, TEXT("Paddle: Input=%.2f, Force=%.0f N"),
       JoystickValue, ForceMagnitude);
```

## Open Questions

1. **Should camera offset be world-space or local-space relative to surfboard?**
   - Answer: Local-space (rotates with surfboard) for more intuitive positioning

2. **Should we clamp player yaw offset to prevent looking backwards?**
   - : Start without clamping, add if needed for gameplay

3. **Should paddling have a stamina/cooldown system?**
   -  Not in initial implementation, add later if needed

4. **Should camera have vertical smoothing (Z-axis position lag)?**
   - : Start without, add if bouncing is too jarring

5. **What should be the default paddle force strength?**
   - : Start with 10000.0 and tune during testing

6. **How should the SurfboardPawn find the surfboard in the level?**
   - Option A: Manual reference assignment in editor (EditAnywhere property) Edit: prefer this option.
   - Option B: Auto-find by tag or class at BeginPlay
   - Option C: Spawned together and linked in GameMode
   - Recommendation: Start with Option A (manual assignment) for simplicity

## Implementation Phases

### Phase 1: Basic Camera Attachment (MVP)
- [ ] Create SurfboardPawn class or modify existing surfboard
- [ ] Add camera component with configurable offset
- [ ] Implement basic position following (no stabilization yet)
- [ ] Set up player controller and input system
- [ ] Test camera follows surfboard position

### Phase 2: Camera Stabilization
- [ ] Implement yaw smoothing (follow surfboard yaw with interpolation)
- [ ] Implement pitch/roll stabilization (keep level)
- [ ] Add debug visualization for rotation values
- [ ] Test on moving/rotating surfboard

### Phase 3: Yaw Control
- [ ] Modify Look input action to only affect yaw
- [ ] Implement relative yaw control (offset from surfboard)
- [ ] Remove pitch/roll control
- [ ] Test camera rotation relative to surfboard

### Phase 4: Paddling System
- [ ] Create Paddle input action
- [ ] Implement paddle input handler
- [ ] Apply force to surfboard mesh at center
- [ ] Add debug visualization for forces
- [ ] Test paddling propels surfboard forward

### Phase 5: Polish and Tuning
- [ ] Tune default values (camera offset, smoothing, force strength)
- [ ] Test on both PC and mobile
- [ ] Add comprehensive logging
- [ ] Performance testing
- [ ] Documentation

## Status
- [ ] Spec approved
- [ ] Phase 1 complete
- [ ] Phase 2 complete
- [ ] Phase 3 complete
- [ ] Phase 4 complete
- [ ] Phase 5 complete
- [ ] Tested on PC
- [ ] Tested on Mobile
- [ ] Code reviewed
- [ ] Merged to main

## Related Files
- `Source/GoneSurfing/TemporaryCameraPawn.h` - Reference for input setup
- `Source/GoneSurfing/TemporaryCameraPawn.cpp` - Reference for Enhanced Input
- `Source/GoneSurfing/SurfboardUtils.h` - Surfboard physics system
- Surfboard Blueprint - Existing surfboard actor

## Notes
- Initial implementation should prioritize simplicity and getting basic functionality working
- Tuning values (smoothing, force strength) will require iterative testing
- Consider adding configuration presets for different "feel" (arcade vs realistic)
- Future enhancements: Camera shake, FOV changes based on speed, spray particles
