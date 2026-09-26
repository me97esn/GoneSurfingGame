# Spec: InfiniteWaveManager - Infinite Side-Scrolling Wave System

## Overview
This specification describes the InfiniteWaveManager class, an Unreal Engine 5 actor that manages multiple GridLODActor instances to create an infinite side-scrolling wave effect. As the camera moves sideways, the system repositions GridLODActors that fall behind to positions ahead of the camera, maintaining the illusion of an endless ocean with continuously breaking waves.

## Current Implementation
Currently, three GridLODActors are manually positioned side-by-side:
- **Actor 1**: Position (0, 0, 0), FrameOffset: 0
- **Actor 2**: Position (mesh_width, 100, 0), FrameOffset: 85
- **Actor 3**: Position (mesh_width*2, 200, 0), FrameOffset: 85*2

Each GridLODActor displays a breaking wave animation by streaming high-resolution meshes nearby and lower-resolution meshes further away. The Y-offset (100 units) combined with the frame offset (85 frames) creates seamless wave transitions between adjacent actors.

## Objective
Enable infinite side-scrolling by automatically repositioning manually-created GridLODActors as the camera moves along the X-axis (sideways), creating an endless ocean where waves continue seamlessly in both directions.

## Important Design Constraints

### Actor Reuse (Performance)
GridLODActors perform extensive preloading and caching of meshes in both their Construction and BeginPlay methods. Creating/destroying actors would be extremely expensive and disrupt this caching. Therefore, the InfiniteWaveManager **MUST NOT spawn or destroy GridLODActors**. Instead, it shall:
- Reference manually-placed GridLODActors in the level
- Only reposition existing actors and update their FrameOffset
- Preserve all internal state, cached meshes, and LOD systems

### Repositioning Behavior (User Experience)
From the player's perspective, GridLODActors should appear to be **fixed in world space** - stationary wave sections that the camera moves past. The repositioning must be **invisible to the player**:
- Actors are only repositioned when they move far enough behind the camera that they're out of view
- The repositioned actor is moved far ahead of the camera (also out of view)
- To the player, it appears as if old wave sections are being left behind and new sections are appearing ahead
- In reality, the same actors are being recycled in a circular pattern
- **Critical**: Repositioning must NEVER occur when an actor is visible to the camera

## Requirements

### Functional Requirements

#### Core Functionality
1. **FR-1**: The class shall inherit from AActor and be exposed to Blueprints
2. **FR-2**: The class shall manage a configurable array of GridLODActor instances
3. **FR-3**: The class shall track camera position along the X-axis (sideways movement)
4. **FR-4**: The class shall automatically reposition GridLODActors ONLY when they fall outside of a configurable distance from the camera (i.e., when behind camera and no longer visible)
5. **FR-5**: When repositioning, the actor shall be moved far enough ahead of the camera that it is also out of view, creating the illusion of new content appearing
6. **FR-6**: GridLODActors shall appear stationary in world space until they exit the visible/active range
7. **FR-7**: The class shall update frame offsets when repositioning actors to ensure seamless wave continuity. It should respect the start frame and end frame of the GridLodActor, and make sure that the frame offset is not bigger then end_frame - start_frame. If it is bigger, it should wrap around.
8. **FR-8**: The class shall display the GridLodActors when NOT playing in editor, to make development easier.

#### Configuration Properties
9. **FR-9**: The class shall have a UPROPERTY ManagedGridActors (TArray<TObjectPtr<AGridLODActor>>) to reference manually-placed GridLODActors in the level (EditInstanceOnly)
10. **FR-10**: The class shall have a UPROPERTY ActorSpacing (float) to define the X-distance between adjacent actors (should match mesh_width from current setup)
11. **FR-11**: The class shall have a UPROPERTY YOffsetPerActor (float) to define the Y-offset between adjacent actors (default: 100.0)
12. **FR-12**: The class shall have a UPROPERTY FrameOffsetPerActor (int32) to define the frame offset increment between adjacent actors (default: 85)
13. **FR-13**: The class shall have a UPROPERTY TrackedCamera (TObjectPtr<AActor>) for optional camera actor reference (if null, uses player camera)
14. **FR-14**: The class shall have a UPROPERTY RepositionDistance (float) to define how far behind the camera an actor must be before repositioning occurs (default: ActorSpacing * 1.5, ensuring actor is well out of view)

#### Actor Management
15. **FR-15**: The class shall NEVER spawn or destroy GridLODActor instances (to preserve preloading and caching)
16. **FR-16**: On BeginPlay, the class shall validate that ManagedGridActors array is populated with valid actor references
17. **FR-17**: On BeginPlay, the class shall analyze current positions of managed actors and initialize their PositionIndex values
18. **FR-18**: The class shall maintain a sorted array of managed actors indexed by their logical position
19. **FR-19**: When camera moves forward (positive X), the class shall check if the leftmost actor is more than RepositionDistance behind the camera before moving it
20. **FR-20**: When camera moves backward (negative X), the class shall check if the rightmost actor is more than RepositionDistance behind the camera before moving it
21. **FR-21**: When repositioning, the actor shall be moved to a position that maintains the actor spacing pattern (NumManagedActors * ActorSpacing away from its previous position)
22. **FR-22**: When repositioning, the class shall calculate new position, Y-offset, and frame offset to maintain wave continuity
23. **FR-23**: The class shall ONLY modify the actor's position (via SetActorLocation) and FrameOffset property - never touching mesh caches or LOD state

#### Position Calculation
24. **FR-24**: Actor X position shall be calculated as: `BasePositionIndex * ActorSpacing`
25. **FR-25**: Actor Y position shall be calculated as: `BasePositionIndex * YOffsetPerActor`
26. **FR-26**: Actor Z position shall remain constant (use existing Z position from manually-placed actor)
27. **FR-27**: Actor frame offset shall be calculated as: `(BasePositionIndex * FrameOffsetPerActor) % (EndFrame - StartFrame)` to ensure wrapping

#### Repositioning Logic
28. **FR-28**: Each tick, the class shall check the distance between camera position and each managed actor's X position
29. **FR-29**: If an actor's X position is more than RepositionDistance behind the camera X position, trigger repositioning for that actor
30. **FR-30**: Repositioning shall move the actor to a new X position that is ahead of the rightmost actor (forward movement) or leftmost actor (backward movement)
31. **FR-31**: The new position shall maintain the ActorSpacing pattern, ensuring the actor appears as the next logical section in the sequence
32. **FR-32**: The class shall update the actor's FrameOffset property when repositioning with proper wrapping
33. **FR-33**: The class shall call SetActorLocation() to move the actor to its new position
34. **FR-34**: The class shall preserve the actor's internal state (loaded meshes, LOD system, preloaded caches) by never destroying actors
35. **FR-35**: Repositioning shall be instantaneous (single frame) since the actor is out of view - no interpolation needed

#### Debug & Visualization
36. **FR-36**: The class shall have a UPROPERTY bShowDebugVisualization (bool) to enable debug drawing
37. **FR-37**: Debug visualization shall display actor positions, RepositionDistance threshold lines, and camera position
38. **FR-38**: Debug visualization shall clearly show which actors are in the "active" range (within RepositionDistance) vs "recyclable" range
39. **FR-39**: The class shall have VisibleAnywhere properties showing current camera position and number of managed actors
40. **FR-40**: The class shall log repositioning events with actor names, old positions, new positions, and new frame offsets

### Non-Functional Requirements
1. **NFR-1**: Repositioning shall be smooth with no visible popping or stuttering
2. **NFR-2**: The system shall NOT handle rapid camera movement (player teleportation). This never happens. 
3. **NFR-3**: The code shall follow UE5 coding standards
4. **NFR-4**: The implementation shall be simple and maintainable
5. **NFR-5**: The system shall have minimal performance overhead (check only on tick, simple arithmetic)
6. **NFR-6**: The system shall support both forward and backward camera movement seamlessly
7. **NFR-7**: The system shall NEVER disrupt GridLODActor preloading, caching, or mesh streaming by avoiding actor spawning/destruction

## Acceptance Criteria
- InfiniteWaveManager can be placed in levels and configured via editor
- User manually places GridLODActors in level and assigns them to InfiniteWaveManager
- InfiniteWaveManager correctly initializes from manually-placed actors
- Camera movement triggers automatic actor repositioning
- Repositioned actors maintain wave animation continuity and all cached meshes
- System works seamlessly in both positive and negative X directions
- Debug visualization clearly shows system state
- No visible popping or discontinuities in wave animation
- GridLODActor mesh preloading and caching remains fully functional throughout repositioning

## Implementation Details

### Class Structure
**File**: `Source/GoneSurfing/InfiniteWaveManager.h`
**File**: `Source/GoneSurfing/InfiniteWaveManager.cpp`

### Key Properties

| Property | Type | Access | Default | Description |
|----------|------|--------|---------|-------------|
| `ManagedGridActors` | `TArray<TObjectPtr<AGridLODActor>>` | EditInstanceOnly, BlueprintReadWrite | Empty | Array of manually-placed GridLODActors to manage |
| `ActorSpacing` | `float` | EditAnywhere, BlueprintReadWrite | `1000.0` | X-distance between adjacent actors (mesh_width) |
| `YOffsetPerActor` | `float` | EditAnywhere, BlueprintReadWrite | `100.0` | Y-offset increment per actor |
| `FrameOffsetPerActor` | `int32` | EditAnywhere, BlueprintReadWrite | `85` | Frame offset increment per actor |
| `TrackedCamera` | `TObjectPtr<AActor>` | EditAnywhere, BlueprintReadWrite | Null | Camera to track (null = player camera) |
| `RepositionDistance` | `float` | EditAnywhere, BlueprintReadWrite | `1500.0` | Distance behind camera before actor is repositioned (recommended: ActorSpacing * 1.5) |
| `ActorInfoArray` | `TArray<FWaveActorInfo>` | Protected | Empty | Internal tracking info for managed actors |
| `LastCameraSectionIndex` | `int32` | Protected | `0` | Last tracked camera section index |
| `bShowDebugVisualization` | `bool` | EditAnywhere, BlueprintReadWrite | `false` | Enable debug drawing |
| `CurrentCameraPositionX` | `float` | VisibleAnywhere, BlueprintReadOnly | `0.0` | Current camera X position (for debugging) |
| `NumManagedActors` | `int32` | VisibleAnywhere, BlueprintReadOnly | `0` | Number of managed actors |

### Key Structures

#### FWaveActorInfo
```cpp
USTRUCT()
struct FWaveActorInfo
{
    GENERATED_BODY()

    /** Spawned GridLODActor instance */
    UPROPERTY()
    TObjectPtr<AGridLODActor> Actor;

    /** Logical position index in the infinite sequence */
    int32 PositionIndex;

    /** Current world position */
    FVector WorldPosition;

    /** Current frame offset */
    int32 FrameOffset;

    FWaveActorInfo()
        : Actor(nullptr)
        , PositionIndex(0)
        , WorldPosition(FVector::ZeroVector)
        , FrameOffset(0)
    {}
};
```

### Key Methods

#### InitializeFromManagedActors()
- **Category**: Wave Management
- **Blueprint Callable**: Yes
- **Behavior**:
  - Validates that ManagedGridActors array is populated (returns error if empty)
  - Analyzes current X positions of all managed actors
  - Sorts actors by X position
  - Assigns initial PositionIndex to each actor based on relative positions
  - Calculates expected FrameOffset for each actor
  - Populates ActorInfoArray with actor references and metadata
  - Logs initialization information with actor count and positions

#### GetCameraPosition()
- **Category**: Wave Management
- **Blueprint Pure**: Yes
- **Returns**: `FVector` - Current camera world position
- **Behavior**: Returns TrackedCamera position or player camera position if null

#### ShouldRepositionActor(FWaveActorInfo& ActorInfo)
- **Category**: Wave Management
- **Protected**: Yes
- **Returns**: `bool` - True if actor should be repositioned
- **Behavior**:
  - Calculates distance from camera to actor: `CameraX - ActorX`
  - Returns true if distance > RepositionDistance (actor is far enough behind camera)

#### UpdateActorPositions()
- **Category**: Wave Management
- **Protected**: Yes
- **Behavior**:
  - Called every tick
  - Gets current camera X position
  - Iterates through all managed actors in ActorInfoArray
  - For each actor, calls ShouldRepositionActor()
  - If true, determines repositioning direction (forward/backward based on camera movement)
  - Calls RepositionActorForward() or RepositionActorBackward() as needed
  - Updates CurrentCameraPositionX for debug display

#### RepositionActorForward(FWaveActorInfo& ActorInfo)
- **Category**: Wave Management
- **Protected**: Yes
- **Behavior**:
  - Called when actor is behind camera during forward movement
  - Finds the rightmost actor's PositionIndex
  - Calculates new PositionIndex = rightmost.PositionIndex + 1
  - Calculates new WorldPosition using CalculateActorTransform()
  - Calculates new FrameOffset using CalculateFrameOffset() with wrapping
  - Calls SetActorLocation() on the actor
  - Updates actor's FrameOffset property
  - Updates ActorInfo with new values
  - Logs repositioning event

#### RepositionActorBackward(FWaveActorInfo& ActorInfo)
- **Category**: Wave Management
- **Protected**: Yes
- **Behavior**:
  - Called when actor is behind camera during backward movement
  - Finds the leftmost actor's PositionIndex
  - Calculates new PositionIndex = leftmost.PositionIndex - 1
  - Calculates new WorldPosition using CalculateActorTransform()
  - Calculates new FrameOffset using CalculateFrameOffset() with wrapping
  - Calls SetActorLocation() on the actor
  - Updates actor's FrameOffset property
  - Updates ActorInfo with new values
  - Logs repositioning event

#### CalculateActorTransform(int32 PositionIndex)
- **Category**: Wave Management
- **Protected**: Yes
- **Returns**: `FTransform` - Calculated actor transform
- **Behavior**:
  - Calculates X, Y, Z position based on PositionIndex
  - Returns FTransform with calculated location

#### CalculateFrameOffset(int32 PositionIndex, AGridLODActor* Actor)
- **Category**: Wave Management
- **Protected**: Yes
- **Returns**: `int32` - Calculated frame offset with wrapping
- **Behavior**:
  - Calculates base offset: `PositionIndex * FrameOffsetPerActor`
  - Gets StartFrame and EndFrame from Actor (assumes GridLODActor has these properties)
  - Wraps offset: `StartFrame + ((BaseOffset - StartFrame) % (EndFrame - StartFrame))`
  - Returns wrapped frame offset

#### DrawDebugVisualization()
- **Category**: Debug
- **Protected**: Yes
- **Behavior**:
  - Draws boxes at actor positions (green = active/visible range, red = outside reposition distance)
  - Draws vertical lines at camera position
  - Draws vertical lines at camera +/- RepositionDistance (showing the reposition trigger zones)
  - Draws arrows showing camera direction of movement
  - Displays text at each actor showing: PositionIndex, FrameOffset, distance from camera
  - Displays text at camera showing current X position and number of active actors

### Lifecycle Behavior

#### Constructor
- Sets default values for properties
- Enables tick

#### BeginPlay()
- Validates configuration (ManagedGridActors is not empty, at least 3 actors recommended)
- Calls InitializeFromManagedActors()
- Initializes CurrentCameraPositionX based on current camera position
- Validates that RepositionDistance is greater than ActorSpacing (warns if not)

#### Tick(float DeltaTime)
- Calls UpdateActorPositions()
- If bShowDebugVisualization, calls DrawDebugVisualization()

#### EndPlay(EEndPlayReason::Type EndPlayReason)
- Clears ActorInfoArray (actors remain in level, only clear references)

### Repositioning Algorithm

```
Every Tick:
1. Get current camera X position (CameraX)
2. Store previous camera X position (LastCameraX) to determine movement direction
3. For each actor in ActorInfoArray:
   a. Calculate distance from camera: Distance = CameraX - ActorX
   b. If Distance > RepositionDistance:
      - Actor is far enough behind camera to be safely repositioned
      - Determine movement direction: Forward = (CameraX > LastCameraX)
      - If moving forward:
        * Find rightmost actor's PositionIndex
        * New PositionIndex = rightmost.PositionIndex + 1
        * Calculate new X position = NewPositionIndex * ActorSpacing
        * Calculate new Y position = NewPositionIndex * YOffsetPerActor
        * Calculate new FrameOffset = CalculateFrameOffset(NewPositionIndex, Actor) with wrapping
        * Call Actor->SetActorLocation(NewPosition)
        * Update Actor->FrameOffset property
        * Update ActorInfo struct
      - If moving backward:
        * Find leftmost actor's PositionIndex
        * New PositionIndex = leftmost.PositionIndex - 1
        * Calculate new X position = NewPositionIndex * ActorSpacing
        * Calculate new Y position = NewPositionIndex * YOffsetPerActor
        * Calculate new FrameOffset = CalculateFrameOffset(NewPositionIndex, Actor) with wrapping
        * Call Actor->SetActorLocation(NewPosition)
        * Update Actor->FrameOffset property
        * Update ActorInfo struct
4. Update LastCameraX = CameraX for next frame
```

**Key Points**:
- Actors appear stationary in world space (player flies past them)
- Only reposition when actor is RepositionDistance behind camera (out of view)
- New position is far ahead of camera (also out of view)
- To player, appears like infinite new content is appearing ahead
- In reality, same 3-7 actors are recycled continuously

### Example Configuration

For the current 3-actor setup (can be expanded to 5-7):
```
ActorSpacing = mesh_width (measure from your GridLODActor, e.g., 1000.0)
YOffsetPerActor = 100.0
FrameOffsetPerActor = 85
RepositionDistance = ActorSpacing * 1.5 (e.g., 1500.0)
  // This ensures actor is 1.5 sections behind camera before repositioning
  // With 3 actors spanning 3*ActorSpacing, this provides good buffer
```

**Why RepositionDistance = ActorSpacing * 1.5?**
- Camera starts at position 0
- Actors at: 0, 1000, 2000 (with ActorSpacing = 1000)
- When camera reaches 2500, actor at 0 is 2500 units behind
- Since 2500 > 1500 (RepositionDistance), actor repositions to 3000
- Actor is always well out of view when repositioned

#### Manual Setup Process:
1. Place 3-7 GridLODActor instances in your level side-by-side
2. Position them with proper spacing, Y-offsets, and FrameOffsets:
   - Actor 0: Position (0, 0, 0), FrameOffset = 0
   - Actor 1: Position (spacing, 100, 0), FrameOffset = 85
   - Actor 2: Position (2*spacing, 200, 0), FrameOffset = 170
   - (Optional) Actor 3: Position (3*spacing, 300, 0), FrameOffset = 255
   - (Optional) Actor 4: Position (4*spacing, 400, 0), FrameOffset = 340
3. Place InfiniteWaveManager in level
4. In Details panel, add all GridLODActors to ManagedGridActors array
5. Set ActorSpacing, YOffsetPerActor, FrameOffsetPerActor to match your manual setup
6. On BeginPlay, manager will analyze positions and maintain the pattern

### Dependencies

**Required Modules**:
- CoreMinimal
- Engine

**Required Classes**:
- `AGridLODActor` - Existing GridLODActor class with FrameOffset property

**Required GridLODActor Properties**:
- `FrameOffset` (int32, EditAnywhere, BlueprintReadWrite) - Must be accessible for updates
- `StartFrame` (int32, VisibleAnywhere) - First frame in the animation sequence
- `EndFrame` (int32, VisibleAnywhere) - Last frame in the animation sequence (used for wrapping)

### Edge Cases & Considerations

1. **Visibility Guarantee**: RepositionDistance MUST be large enough that actor is definitely out of view when repositioned. Recommend ActorSpacing * 1.5 minimum
2. **Actor Count**: With 3 actors, only 1 actor is ahead of camera at a time. Consider 5-7 actors for better buffering
3. **Negative Frame Offsets**: Frame offset calculation supports negative values for actors behind origin, with proper wrapping
4. **Actor Reuse**: Actors are NEVER spawned or destroyed, only repositioned (preserves all preloading and caching)
5. **Sort Stability**: ActorInfoArray should remain sorted by PositionIndex for efficient leftmost/rightmost lookups
6. **Frame Offset Wrapping**: MUST wrap using modulo (EndFrame - StartFrame) to prevent out-of-range frame values
7. **Z-Position**: Preserved from manually-placed actors, allows for waves at different heights
8. **Mesh Cache Preservation**: SetActorLocation() is non-destructive and preserves all GridLODActor internal state
9. **Incomplete Array**: System should validate that ManagedGridActors has at least 3 actors (warn if fewer)
10. **Camera Movement Detection**: Track camera movement direction to determine forward vs backward repositioning

### Performance Considerations

- Only check for repositioning each tick (minimal overhead)
- Use simple arithmetic for position calculations
- **CRITICAL**: Never spawn/destroy actors - this would invalidate mesh preloading and caching
- SetActorLocation() is lightweight and preserves all internal actor state
- Consider caching camera reference instead of repeated GetPlayerCameraManager() calls
- Sort ActorInfoArray only when necessary (after repositioning)
- Manual placement allows actors to complete expensive initialization before gameplay starts

### Future Enhancements

1. **Multi-Directional Support**: Extend to support Y-axis (forward/backward) movement with 2D grid
2. **Dynamic LOD**: Adjust GridLODActor LOD settings based on camera distance
3. **Async Loading**: Pre-load meshes for actors about to be repositioned
4. **Wave Variety**: Support different wave patterns for visual variety
5. **Performance Profiling**: Add stat tracking for repositioning frequency and cost

## Usage Example

### In Editor (Recommended Workflow):
1. Manually place 3-7 GridLODActor instances in your level
   - Position them side-by-side with proper spacing
   - Set their FrameOffset values (0, 85, 170, etc.)
   - Let them complete their construction and mesh preloading
2. Place InfiniteWaveManager in your level
3. In InfiniteWaveManager Details panel:
   - Add all GridLODActors to the ManagedGridActors array
   - Set ActorSpacing to match your mesh width (e.g., 1000.0)
   - Set YOffsetPerActor = 100
   - Set FrameOffsetPerActor = 85
   - Set RepositionDistance = ActorSpacing * 1.5 (e.g., 1500.0)
4. Play - manager analyzes positions and invisibly repositions actors when they move far enough behind camera

### In Blueprint (Runtime Configuration):
```cpp
// Get reference to InfiniteWaveManager in level
AInfiniteWaveManager* WaveManager = FindActorByClass<AInfiniteWaveManager>();

// Add manually-placed GridLODActors to manager
TArray<AActor*> FoundActors;
UGameplayStatics::GetAllActorsOfClass(GetWorld(), AGridLODActor::StaticClass(), FoundActors);
for (AActor* Actor : FoundActors)
{
    WaveManager->ManagedGridActors.Add(Cast<AGridLODActor>(Actor));
}

// Configure spacing and repositioning
WaveManager->ActorSpacing = 1000.0f;
WaveManager->YOffsetPerActor = 100.0f;
WaveManager->FrameOffsetPerActor = 85;
WaveManager->RepositionDistance = 1500.0f; // 1.5 * ActorSpacing

// Initialize
WaveManager->InitializeFromManagedActors();
```

## Testing Checklist

- [ ] Manager correctly references manually-placed GridLODActors
- [ ] InitializeFromManagedActors() correctly analyzes and sorts actors
- [ ] Initial positions and frame offsets are preserved from manual placement
- [ ] Camera movement forward triggers repositioning only when actor is RepositionDistance behind camera
- [ ] Camera movement backward triggers repositioning only when actor is RepositionDistance behind camera
- [ ] Actors appear stationary in world space (camera moves past them)
- [ ] Repositioning only occurs when actor is out of camera view
- [ ] Repositioned actor appears far ahead of camera (also out of view)
- [ ] Wave animation remains seamless during repositioning
- [ ] No visible popping - actors are never moved while visible
- [ ] GridLODActor mesh caches remain intact after repositioning
- [ ] LOD system continues to function correctly after repositioning
- [ ] System handles rapid camera movement (teleportation)
- [ ] Debug visualization shows correct state
- [ ] Negative frame offsets work correctly
- [ ] System works with 3, 5, and 7 manually-placed actors
- [ ] Performance is acceptable (minimal frame time impact)
- [ ] No mesh reloading occurs during repositioning

## Status
- [ ] Specification written
- [ ] Implementation complete
- [ ] Tests passing
- [ ] Code reviewed
- [ ] Merged to main

## Metadata
- **Created**: 2025-12-18
- **Author**: Development Team
- **Priority**: High
- **Estimated Effort**: 4-6 hours
- **Related Components**: GridLODActor, Camera Tracking
- **Related Specs**: [GridLODManager.spec.md](GridLODManager.spec.md), [MeshArrayActor.spec.md](MeshArrayActor.spec.md)
