# GridLODActor Infinite Tiling Manager Specification

## Overview
Create a manager actor that spawns and manages multiple GridLODActor instances in a grid pattern, creating an infinite ocean effect by dynamically repositioning actors as the camera moves.

## Core Concept

### Static Grid Pattern
Instead of infinite actors, maintain a fixed number (e.g., 3x3 = 9 actors) that represent a "window" around the camera. As the camera moves, reposition the furthest actors to the opposite side.

**Example (3x3 grid, top view):**
```
Initial state (camera at center):
[0,0] [1,0] [2,0]
[0,1] [1,1*] [2,1]  (* = camera)
[0,2] [1,2] [2,2]

Camera moves right:
[1,0] [2,0] [0,0]  <- [0,0] repositioned from left to right
[1,1*] [2,1] [0,1]
[1,2] [2,2] [0,2]
```

### Coordinate System

**World Space:** Unreal's standard coordinate system (X, Y, Z)
**Grid Space:** Integer grid coordinates (GridX, GridY)
**Cell Offset:** Position offset for each GridLODActor instance

Each GridLODActor represents one "cell" in the infinite grid:
- Cell [0,0] at position (0, 0)
- Cell [1,0] at position (GridCellSizeX, 0)
- Cell [0,1] at position (0, GridCellSizeY)

## Design

### Manager Actor: AGridLODManager

**Responsibilities:**
1. Spawn initial grid of GridLODActor instances
2. Track camera position
3. Detect when camera crosses grid boundaries
4. Reposition actors to maintain "window" around camera
5. Update frame offsets for repositioned actors

### Key Parameters

```cpp
/** Number of grid cells in X direction */
UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid")
int32 GridWidth = 3;

/** Number of grid cells in Y direction */
UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid")
int32 GridHeight = 3;

/** Size of each grid cell in world units (should match GridLODActor's GridSize) */
UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid")
FVector2D GridCellSize = FVector2D(100.0f, 100.0f);

/** Template actor to spawn (GridLODActor blueprint) */
UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid")
TSubclassOf<AGridLODActor> GridActorTemplate;

/** Frame offset per grid cell (for temporal tiling) */
UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid")
int32 FrameOffsetPerCell = 0;

/** Distance threshold for repositioning (fraction of cell size, 0.5 = half cell) */
UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid", meta = (ClampMin = "0.1", ClampMax = "1.0"))
float RepositionThreshold = 0.5f;

/** Optional camera actor to track (if null, uses player camera) */
UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid")
TObjectPtr<AActor> TrackedCamera;
```

### Data Structures

#### Grid Cell Info
```cpp
USTRUCT()
struct FGridCellInfo
{
    GENERATED_BODY()

    /** Spawned GridLODActor instance */
    UPROPERTY()
    TObjectPtr<AGridLODActor> Actor;

    /** Current grid coordinates (in infinite grid space) */
    int32 GridX;
    int32 GridY;

    /** Frame offset for this cell */
    int32 FrameOffset;

    FGridCellInfo()
        : Actor(nullptr)
        , GridX(0)
        , GridY(0)
        , FrameOffset(0)
    {
    }
};
```

#### Camera Grid Position
Track which grid cell the camera is currently in:
```cpp
/** Last grid cell the camera was in (for change detection) */
FIntPoint LastCameraGridCell;

/** Current grid cell the camera is in */
FIntPoint CurrentCameraGridCell;
```

## Algorithm

### Initialization (BeginPlay)

```cpp
void AGridLODManager::BeginPlay()
{
    // Calculate initial camera grid position
    FVector CameraPos = GetCameraPosition();
    CurrentCameraGridCell = WorldToGridCell(CameraPos);
    LastCameraGridCell = CurrentCameraGridCell;

    // Spawn grid of actors centered around camera
    SpawnInitialGrid();
}
```

### Spawn Initial Grid

```cpp
void AGridLODManager::SpawnInitialGrid()
{
    // Calculate grid center offset (to center around camera)
    int32 OffsetX = -GridWidth / 2;
    int32 OffsetY = -GridHeight / 2;

    for (int32 Y = 0; Y < GridHeight; ++Y)
    {
        for (int32 X = 0; X < GridWidth; ++X)
        {
            // Calculate global grid coordinates
            int32 GlobalX = CurrentCameraGridCell.X + OffsetX + X;
            int32 GlobalY = CurrentCameraGridCell.Y + OffsetY + Y;

            // Spawn actor at grid position
            SpawnGridActor(GlobalX, GlobalY);
        }
    }
}
```

### Spawn Grid Actor

```cpp
AGridLODActor* AGridLODManager::SpawnGridActor(int32 GridX, int32 GridY)
{
    if (!GridActorTemplate)
    {
        return nullptr;
    }

    // Calculate world position
    FVector WorldPos = GridCellToWorld(GridX, GridY);

    // Spawn actor
    FActorSpawnParameters SpawnParams;
    SpawnParams.Owner = this;
    AGridLODActor* NewActor = GetWorld()->SpawnActor<AGridLODActor>(
        GridActorTemplate,
        WorldPos,
        FRotator::ZeroRotator,
        SpawnParams
    );

    if (NewActor)
    {
        // Calculate frame offset based on grid position
        int32 FrameOffset = CalculateFrameOffset(GridX, GridY);
        NewActor->FrameOffset = FrameOffset;

        // Store in grid cells array
        FGridCellInfo CellInfo;
        CellInfo.Actor = NewActor;
        CellInfo.GridX = GridX;
        CellInfo.GridY = GridY;
        CellInfo.FrameOffset = FrameOffset;
        GridCells.Add(CellInfo);
    }

    return NewActor;
}
```

### Calculate Frame Offset

Multiple strategies for frame offsets:

**Strategy 1: Linear Offset (Simple)**
```cpp
int32 AGridLODManager::CalculateFrameOffset(int32 GridX, int32 GridY)
{
    // Different offset per cell to avoid visual repetition
    return (GridX + GridY) * FrameOffsetPerCell;
}
```

**Strategy 2: Modulo Pattern (Seamless Tiling)**
```cpp
int32 AGridLODManager::CalculateFrameOffset(int32 GridX, int32 GridY)
{
    // Wrap frame offsets to create repeating pattern
    int32 PatternX = GridX % PatternSizeX;
    int32 PatternY = GridY % PatternSizeY;
    return (PatternX + PatternY) * FrameOffsetPerCell;
}
```

**Strategy 3: Pseudo-Random (Maximum Variation)**
```cpp
int32 AGridLODManager::CalculateFrameOffset(int32 GridX, int32 GridY)
{
    // Use grid coordinates as seed for deterministic "random" offset
    int32 Seed = GridX * 73856093 ^ GridY * 19349663;  // Prime number hash
    return (Seed % MaxFrameOffset);
}
```

### Tick - Check Camera Movement

```cpp
void AGridLODManager::Tick(float DeltaTime)
{
    Super::Tick(DeltaTime);

    // Get current camera position
    FVector CameraPos = GetCameraPosition();
    CurrentCameraGridCell = WorldToGridCell(CameraPos);

    // Check if camera has moved to a different grid cell
    if (CurrentCameraGridCell != LastCameraGridCell)
    {
        // Calculate movement delta
        FIntPoint Delta = CurrentCameraGridCell - LastCameraGridCell;

        // Reposition actors based on movement
        RepositionGrid(Delta);

        LastCameraGridCell = CurrentCameraGridCell;
    }
}
```

### Reposition Grid

This is the key function that creates the infinite effect:

```cpp
void AGridLODManager::RepositionGrid(FIntPoint Delta)
{
    // Delta represents how many cells the camera moved
    // Example: Delta = (1, 0) means camera moved right by 1 cell

    if (Delta.X != 0)
    {
        RepositionGridX(Delta.X);
    }

    if (Delta.Y != 0)
    {
        RepositionGridY(Delta.Y);
    }
}

void AGridLODManager::RepositionGridX(int32 DeltaX)
{
    // Moving right (DeltaX > 0): Reposition leftmost column to right side
    // Moving left (DeltaX < 0): Reposition rightmost column to left side

    int32 CenterX = CurrentCameraGridCell.X;
    int32 HalfWidth = GridWidth / 2;

    if (DeltaX > 0)
    {
        // Camera moved right - move leftmost actors to right side
        int32 OldX = CenterX - HalfWidth - 1;
        int32 NewX = CenterX + HalfWidth;

        RepositionColumn(OldX, NewX);
    }
    else if (DeltaX < 0)
    {
        // Camera moved left - move rightmost actors to left side
        int32 OldX = CenterX + HalfWidth + 1;
        int32 NewX = CenterX - HalfWidth;

        RepositionColumn(OldX, NewX);
    }
}

void AGridLODManager::RepositionColumn(int32 OldGridX, int32 NewGridX)
{
    // Find all cells at OldGridX and move them to NewGridX
    for (FGridCellInfo& Cell : GridCells)
    {
        if (Cell.GridX == OldGridX)
        {
            // Update grid coordinates
            Cell.GridX = NewGridX;

            // Update world position
            FVector NewWorldPos = GridCellToWorld(Cell.GridX, Cell.GridY);
            Cell.Actor->SetActorLocation(NewWorldPos);

            // Update frame offset
            Cell.FrameOffset = CalculateFrameOffset(Cell.GridX, Cell.GridY);
            Cell.Actor->FrameOffset = Cell.FrameOffset;

            UE_LOG(LogTemp, Log, TEXT("GridLODManager: Repositioned actor from X=%d to X=%d (new pos: %s)"),
                OldGridX, NewGridX, *NewWorldPos.ToString());
        }
    }
}
```

Similar logic for Y axis:
```cpp
void AGridLODManager::RepositionGridY(int32 DeltaY)
{
    int32 CenterY = CurrentCameraGridCell.Y;
    int32 HalfHeight = GridHeight / 2;

    if (DeltaY > 0)
    {
        // Camera moved forward - move rearmost actors to front
        int32 OldY = CenterY - HalfHeight - 1;
        int32 NewY = CenterY + HalfHeight;
        RepositionRow(OldY, NewY);
    }
    else if (DeltaY < 0)
    {
        // Camera moved backward - move frontmost actors to rear
        int32 OldY = CenterY + HalfHeight + 1;
        int32 NewY = CenterY - HalfHeight;
        RepositionRow(OldY, NewY);
    }
}
```

### Coordinate Conversion

```cpp
FIntPoint AGridLODManager::WorldToGridCell(FVector WorldPos) const
{
    // Convert world position to grid cell coordinates
    int32 GridX = FMath::FloorToInt(WorldPos.X / GridCellSize.X);
    int32 GridY = FMath::FloorToInt(WorldPos.Y / GridCellSize.Y);
    return FIntPoint(GridX, GridY);
}

FVector AGridLODManager::GridCellToWorld(int32 GridX, int32 GridY) const
{
    // Convert grid cell coordinates to world position (cell center)
    float WorldX = GridX * GridCellSize.X + (GridCellSize.X * 0.5f);
    float WorldY = GridY * GridCellSize.Y + (GridCellSize.Y * 0.5f);
    return FVector(WorldX, WorldY, GetActorLocation().Z);
}
```

## Integration with WaveHeight Tiling

When WaveHeight tiling is enabled, each GridLODActor instance shows the same wave pattern but:
1. **Position offset** creates spatial tiling
2. **Frame offset** creates temporal variation (optional)

**Configuration:**
```cpp
// WaveHeight actor (one per GridLODActor)
bEnableTiling = true;
TileScale = (1.0, 1.0, 1.0);
TileOffset = (0.0, 0.0, 0.0);  // Tiling handles position automatically

// GridLODActor
FrameOffset = calculated by manager;  // Temporal variation

// GridLODManager
GridCellSize = (100.0, 100.0);  // Match WaveHeight data table size
FrameOffsetPerCell = 10;  // Small offset for variation
```

## Edge Cases and Considerations

### Fast Camera Movement
If camera moves multiple cells per frame:
```cpp
void AGridLODManager::RepositionGrid(FIntPoint Delta)
{
    // Handle large deltas (teleporting, fast movement)
    if (FMath::Abs(Delta.X) > GridWidth || FMath::Abs(Delta.Y) > GridHeight)
    {
        // Camera moved too far - respawn entire grid
        RespawnEntireGrid();
        return;
    }

    // Normal incremental repositioning
    // ...
}
```

### Cell Size Mismatch
GridCellSize must match GridLODActor's GridSize:
```cpp
void AGridLODManager::ValidateConfiguration()
{
    if (GridCells.Num() > 0 && GridCells[0].Actor)
    {
        FVector2D ActorGridSize = GridCells[0].Actor->GridSize;
        if (!ActorGridSize.Equals(GridCellSize, 1.0f))
        {
            UE_LOG(LogTemp, Warning, TEXT("GridLODManager: GridCellSize mismatch! Manager=%s, Actor=%s"),
                *GridCellSize.ToString(), *ActorGridSize.ToString());
        }
    }
}
```

### Seamless Boundaries
To avoid visible seams between GridLODActor instances:
1. Ensure GridLODActor meshes align perfectly at edges
2. Use same WaterController for all instances
3. Consider small overlap between cells (0.1%)

### Memory Management
With 3x3 grid = 9 actors, each with streaming:
```
Meshes per actor: ~72 (with selective preloading)
Total meshes: 9 × 72 = 648 meshes

Old approach (no manager): 1 actor × 144 meshes = 144
New approach (9 actors): 9 × 72 = 648 meshes

Note: More meshes but creates infinite ocean effect
```

## Debug Visualization

Add debug drawing to visualize the grid:

```cpp
void AGridLODManager::DrawDebugVisualization()
{
    if (!bShowDebugVisualization)
    {
        return;
    }

    // Draw grid cell boundaries
    for (const FGridCellInfo& Cell : GridCells)
    {
        FVector CellCenter = GridCellToWorld(Cell.GridX, Cell.GridY);
        FVector HalfSize(GridCellSize.X * 0.5f, GridCellSize.Y * 0.5f, 100.0f);

        // Color based on distance from camera
        FColor Color = (Cell.GridX == CurrentCameraGridCell.X && Cell.GridY == CurrentCameraGridCell.Y)
            ? FColor::Green   // Camera cell
            : FColor::Cyan;   // Other cells

        DrawDebugBox(GetWorld(), CellCenter, HalfSize, Color, false, -1.0f, 0, 5.0f);

        // Draw grid coordinates
        FString Label = FString::Printf(TEXT("[%d,%d]\nFrame:%d"),
            Cell.GridX, Cell.GridY, Cell.FrameOffset);
        DrawDebugString(GetWorld(), CellCenter + FVector(0, 0, 200.0f),
            Label, nullptr, Color, -1.0f, true, 1.5f);
    }

    // Draw camera grid cell
    FVector CameraPos = GetCameraPosition();
    DrawDebugSphere(GetWorld(), CameraPos, 50.0f, 8, FColor::Red, false, -1.0f, 0, 3.0f);
}
```

## Configuration Examples

### Example 1: 3x3 Grid, No Frame Offset
```cpp
GridWidth = 3;
GridHeight = 3;
GridCellSize = (100.0, 100.0);  // Match GridLODActor->GridSize
FrameOffsetPerCell = 0;  // All actors show same frame
```
Result: 9 identical actors, seamless infinite ocean

### Example 2: 5x5 Grid, Temporal Variation
```cpp
GridWidth = 5;
GridHeight = 5;
GridCellSize = (100.0, 100.0);
FrameOffsetPerCell = 5;  // Each cell offset by 5 frames
```
Result: 25 actors with slight temporal variation for visual interest

### Example 3: 5x1 Strip (Recommended for Forward Movement)
```cpp
GridWidth = 5;
GridHeight = 1;
GridCellSize = (100.0, 100.0);  // Match GridLODActor->GridSize
FrameOffsetPerCell = 0;
```
Result: 5 actors in a horizontal strip, optimized for side-to-side gameplay
- Camera centered at cell [2, 0]
- 2 cells on left, 2 cells on right
- Only repositions in X direction (Y stays constant)
- Memory: 5 actors × ~72 meshes = ~360 meshes (with selective preloading)

### Example 4: 1x5 Strip (Forward-Only Movement)
```cpp
GridWidth = 1;
GridHeight = 5;
GridCellSize = (100.0, 100.0);
FrameOffsetPerCell = 0;
```
Result: 5 actors in a vertical strip, optimized for forward/backward gameplay

## Implementation Checklist

- [ ] Create AGridLODManager actor class
- [ ] Add FGridCellInfo struct
- [ ] Implement SpawnInitialGrid()
- [ ] Implement SpawnGridActor()
- [ ] Implement coordinate conversion (WorldToGridCell, GridCellToWorld)
- [ ] Implement CalculateFrameOffset() with chosen strategy
- [ ] Implement Tick() with camera tracking
- [ ] Implement RepositionGrid(), RepositionGridX(), RepositionGridY()
- [ ] Implement RepositionColumn(), RepositionRow()
- [ ] Add debug visualization (DrawDebugVisualization)
- [ ] Add validation (ValidateConfiguration)
- [ ] Handle edge cases (fast movement, teleporting)
- [ ] Test with WaveHeight tiling enabled
- [ ] Test camera movement in all directions
- [ ] Verify no visible seams or popping
- [ ] Profile memory and performance
- [ ] Create Blueprint from AGridLODManager
- [ ] Document configuration in editor

## Testing Strategy

1. **Static Test:** Place camera at origin, verify 3x3 grid spawns correctly
2. **Movement Test:** Move camera right/left/forward/back, verify repositioning
3. **Diagonal Test:** Move camera diagonally, verify both X and Y reposition
4. **Fast Movement Test:** Teleport camera far away, verify respawn
5. **Visual Test:** Enable debug visualization, watch grid cells update
6. **Seam Test:** Look for visible boundaries between GridLODActor instances
7. **Frame Offset Test:** Verify temporal variation if enabled
8. **Performance Test:** Monitor frame rate with 9+ actors active

## Future Enhancements

1. **LOD for Manager:** Reduce grid size (3x3 → 2x2) when camera high above
2. **Async Spawning:** Spawn/reposition actors asynchronously
3. **Pooling:** Pool actors instead of spawn/destroy
4. **Predictive Loading:** Preload cells in camera movement direction
5. **Frustum Culling:** Only reposition visible cells
6. **Variable Cell Sizes:** Different sizes based on distance from camera
