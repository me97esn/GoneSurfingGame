# GridLODActor Selective Preloading Optimization

## Problem Statement

Current implementation (as of Phase 2 streaming) preloads **all meshes** (both LOD levels) for upcoming frames:
```cpp
// Current behavior in PreloadMeshesForFrameRange:
for each frame in [CurrentFrame, CurrentFrame + PreloadFrameCount]:
    for each cell (X, Y) in grid:
        Load high-res mesh for (X, Y, frame)
        Load low-res mesh for (X, Y, frame)
```

**Issues:**
1. **Wasted memory**: Loads meshes that will never be used
2. **Unnecessary I/O**: Camera movement is limited, LOD zones are predictable
3. **Cache pollution**: Useful meshes pushed out by unnecessary ones

**Key insight:** Camera never moves fast enough to change LOD zones dramatically in a few frames.

## Camera Movement Analysis

### Maximum Camera Movement
Assuming realistic gameplay:
- Fast camera movement: ~50-100 m/s
- At 60 FPS: ~1.6 m per frame
- Over 3 frames (PreloadFrameCount): ~5 m

With `LODDistanceThreshold = 50.0f`:
- High LOD zone radius: 50m
- Low LOD zone radius: Beyond 50m (up to MaxVisibleDistance)

**Conclusion:** Camera position changes are small relative to LOD zone sizes.

### LOD Transition Patterns

**Cells currently HIGH LOD:**
- Will stay HIGH LOD next frame (camera barely moved)
- Might become LOW LOD if camera moves away (rare, edge cells only)
- Will **never** be culled in next few frames

**Cells currently LOW LOD:**
- Will stay LOW LOD next frame (most cases)
- Might become HIGH LOD if camera moves closer (rare, edge cells only)
- Might become culled if camera moves away

**Cells currently CULLED:**
- Will stay culled next frame
- Might become LOW LOD if camera moves closer (rare)
- Will **never** become HIGH LOD directly

## Proposed Optimization Strategy

### Core Principle
**Only preload mesh LODs that cells will actually use based on their current state.**

### Preloading Rules

#### Rule 1: High-LOD Preloading (Conservative)
Preload HIGH-RES meshes only for:
1. Cells **currently** showing HIGH LOD
2. Cells **adjacent** to HIGH LOD cells (1-cell border)

**Rationale:**
- Current HIGH cells will stay HIGH next frames
- Adjacent cells might transition to HIGH if camera moves toward them
- Non-adjacent LOW cells cannot become HIGH in just a few frames

#### Rule 2: Low-LOD Preloading (Broader)
Preload LOW-RES meshes only for:
1. Cells **currently** showing LOW LOD
2. Cells **adjacent** to LOW LOD cells (1-cell border)
3. HIGH LOD cells (might transition to LOW if camera moves away)

**Rationale:**
- Current LOW cells will stay LOW next frames
- Adjacent cells might transition to LOW
- HIGH cells might degrade to LOW (rare but possible)

#### Rule 3: No Preloading for Culled Cells
Do **not** preload meshes for:
- Cells currently culled and not adjacent to visible cells

**Rationale:**
- Camera won't reach them in a few frames
- Waste of memory and I/O

### Visual Example

```
Grid (3x8):
C = Culled
L = Low LOD
H = High LOD
* = Camera position

Current Frame State:
[C] [C] [C] [C] [C] [C] [C] [C]
[C] [L] [L] [L] [L] [L] [L] [C]
[L] [L] [H] [H*][H] [L] [L] [L]

Preload for next frames:
HIGH-RES needed for: [2,2], [2,3], [2,4] + adjacent = [2,1], [2,5], [1,2], [1,3], [1,4]
LOW-RES needed for: All [L] cells + adjacent + all [H] cells (might degrade)

Do NOT preload:
- HIGH-RES for [0,*] cells (culled, too far)
- LOW-RES for [0,*] cells (culled)
- HIGH-RES for [1,0], [1,7], [2,0], [2,7] (LOW cells far from HIGH zone)
```

## Implementation Design

### Data Structures

#### Current LOD State Tracking
Already exists in `UGridCellComponent`:
```cpp
int32 CurrentLODLevel;  // -1=culled, 0=high, 1=low
```

### New Function: GetCellsNeedingPreload

```cpp
struct FLODPreloadRequest
{
    int32 GridX;
    int32 GridY;
    bool bNeedHighLOD;
    bool bNeedLowLOD;
};

TArray<FLODPreloadRequest> AGridLODActor::GetCellsNeedingPreload() const
{
    TArray<FLODPreloadRequest> Requests;

    // Track which cells are HIGH, LOW, or adjacent
    TSet<FIntPoint> HighLODCells;
    TSet<FIntPoint> LowLODCells;
    TSet<FIntPoint> AdjacentToHigh;
    TSet<FIntPoint> AdjacentToLow;

    // Pass 1: Identify current LOD states
    for (const UGridCellComponent* Cell : GridCells)
    {
        if (!Cell) continue;

        FIntPoint CellPos(Cell->GridX, Cell->GridY);

        if (Cell->CurrentLODLevel == 0)  // High LOD
        {
            HighLODCells.Add(CellPos);
        }
        else if (Cell->CurrentLODLevel == 1)  // Low LOD
        {
            LowLODCells.Add(CellPos);
        }
        // Culled cells (-1) are ignored
    }

    // Pass 2: Find adjacent cells
    for (const FIntPoint& HighCell : HighLODCells)
    {
        // Check 8-connected neighbors (or 4-connected if preferred)
        for (int32 dx = -1; dx <= 1; ++dx)
        {
            for (int32 dy = -1; dy <= 1; ++dy)
            {
                if (dx == 0 && dy == 0) continue;

                FIntPoint Neighbor(HighCell.X + dx, HighCell.Y + dy);

                // Check bounds
                if (Neighbor.X >= 0 && Neighbor.X < GridWidth &&
                    Neighbor.Y >= 0 && Neighbor.Y < GridHeight)
                {
                    AdjacentToHigh.Add(Neighbor);
                }
            }
        }
    }

    // Similar for Low LOD adjacent
    for (const FIntPoint& LowCell : LowLODCells)
    {
        for (int32 dx = -1; dx <= 1; ++dx)
        {
            for (int32 dy = -1; dy <= 1; ++dy)
            {
                if (dx == 0 && dy == 0) continue;

                FIntPoint Neighbor(LowCell.X + dx, LowCell.Y + dy);

                if (Neighbor.X >= 0 && Neighbor.X < GridWidth &&
                    Neighbor.Y >= 0 && Neighbor.Y < GridHeight)
                {
                    AdjacentToLow.Add(Neighbor);
                }
            }
        }
    }

    // Pass 3: Generate preload requests
    for (int32 Y = 0; Y < GridHeight; ++Y)
    {
        for (int32 X = 0; X < GridWidth; ++X)
        {
            FIntPoint CellPos(X, Y);
            FLODPreloadRequest Request;
            Request.GridX = X;
            Request.GridY = Y;
            Request.bNeedHighLOD = false;
            Request.bNeedLowLOD = false;

            // Rule 1: HIGH-RES preloading
            if (HighLODCells.Contains(CellPos) || AdjacentToHigh.Contains(CellPos))
            {
                Request.bNeedHighLOD = true;
            }

            // Rule 2: LOW-RES preloading
            if (LowLODCells.Contains(CellPos) ||
                AdjacentToLow.Contains(CellPos) ||
                HighLODCells.Contains(CellPos))  // HIGH might degrade to LOW
            {
                Request.bNeedLowLOD = true;
            }

            // Only add if at least one LOD is needed
            if (Request.bNeedHighLOD || Request.bNeedLowLOD)
            {
                Requests.Add(Request);
            }
        }
    }

    return Requests;
}
```

### Updated PreloadMeshesForFrameRange

```cpp
void AGridLODActor::PreloadMeshesForFrameRange(int32 StartFrame, int32 EndFrame)
{
    if (!bEnableMeshStreaming)
    {
        return;
    }

    // NEW: Get selective preload list based on current LOD state
    TArray<FLODPreloadRequest> PreloadRequests = GetCellsNeedingPreload();

    // Preload meshes for the specified frame range
    for (int32 Frame = StartFrame; Frame <= EndFrame; ++Frame)
    {
        // Skip if this frame is already cached
        if (CachedFrames.Contains(Frame))
        {
            continue;
        }

        // NEW: Only load meshes for cells that need them
        for (const FLODPreloadRequest& Request : PreloadRequests)
        {
            // Load high-res mesh if needed
            if (Request.bNeedHighLOD)
            {
                FString HighResPath = BuildMeshPath(Request.GridX, Request.GridY, Frame, 0);
                if (!MeshCache.Contains(HighResPath))
                {
                    UStaticMesh* HighResMesh = LoadObject<UStaticMesh>(nullptr, *HighResPath, nullptr, LOAD_None, nullptr);
                    if (HighResMesh)
                    {
                        MeshCache.Add(HighResPath, HighResMesh);
                    }
                }
            }

            // Load low-res mesh if needed
            if (Request.bNeedLowLOD)
            {
                FString LowResPath = BuildMeshPath(Request.GridX, Request.GridY, Frame, 1);
                if (!MeshCache.Contains(LowResPath))
                {
                    UStaticMesh* LowResMesh = LoadObject<UStaticMesh>(nullptr, *LowResPath, nullptr, LOAD_None, nullptr);
                    if (LowResMesh)
                    {
                        MeshCache.Add(LowResPath, LowResMesh);
                    }
                }
            }
        }

        // Mark this frame as cached
        CachedFrames.Add(Frame);
    }

    if (bShowDebugVisualization)
    {
        UE_LOG(LogTemp, Log, TEXT("GridLODActor: Selective preload - frames %d-%d, %d cells checked, %d meshes loaded. Cache: %d meshes from %d frames"),
            StartFrame, EndFrame, PreloadRequests.Num(), MeshCache.Num(), MeshCache.Num(), CachedFrames.Num());
    }
}
```

## Performance Analysis

### Memory Savings Example

**Scenario:** 3x8 grid, 3 frames preload
- Total cells: 24
- Frames to preload: 3

**Old approach:**
```
Meshes loaded = 24 cells × 2 LODs × 3 frames = 144 meshes
```

**New approach (typical case - 3 HIGH cells, 8 LOW cells, 13 culled):**
```
HIGH-RES needed = 3 HIGH cells + ~6 adjacent = 9 cells
LOW-RES needed = 8 LOW cells + ~4 adjacent + 3 HIGH cells = 15 cells

Meshes loaded = (9 HIGH + 15 LOW) × 3 frames = 72 meshes
```

**Savings: 50% reduction** in typical case

**Best case (camera stationary):**
- Only current HIGH/LOW cells + immediate neighbors
- Could be 60-70% reduction

**Worst case (camera moving fast through middle):**
- More cells adjacent to LOD transition zones
- Still ~30-40% reduction

### CPU Cost

**Additional cost per frame:**
- Pass 1: Iterate 24 cells → O(n)
- Pass 2: Check neighbors for HIGH/LOW cells → O(n × 8) for 8-connected
- Pass 3: Generate requests → O(n)
- **Total: O(n)** where n = grid size (24 cells)

For 24 cells, this is negligible (~100-200 operations per frame).

### I/O Savings

Fewer meshes loaded = less disk I/O = faster preloading = smoother frame pacing.

## Configuration Options

Add new properties for fine-tuning:

```cpp
/** Use selective preloading (only load meshes cells will likely use) */
UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Streaming")
bool bUseSelectivePreloading = true;

/** Include 8-connected neighbors vs 4-connected (affects preload border size) */
UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Streaming")
bool bUse8ConnectedNeighbors = true;

/** Extra border cells to include in preload (0 = adjacent only, 1 = 2-cell border, etc.) */
UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Streaming", meta = (ClampMin = "0", ClampMax = "3"))
int32 PreloadBorderSize = 0;
```

## Testing Strategy

1. **Visual verification:**
   - Enable debug visualization
   - Move camera around
   - Verify HIGH cells stay HIGH
   - Verify no LOD popping

2. **Cache monitoring:**
   - Log cache size before/after
   - Confirm ~50% reduction
   - Ensure no missing meshes (errors)

3. **Performance:**
   - Profile frame time with/without optimization
   - Measure memory usage
   - Check for any hitches during LOD transitions

4. **Edge cases:**
   - Fast camera movement
   - Camera at grid edge
   - Teleporting camera
   - Frame changes with LOD transitions

## Implementation Checklist

- [ ] Add struct FLODPreloadRequest to header
- [ ] Add GetCellsNeedingPreload() function
- [ ] Add selective preloading properties (bUseSelectivePreloading, etc.)
- [ ] Update PreloadMeshesForFrameRange to use selective loading
- [ ] Add logging for cache statistics
- [ ] Test with camera movement
- [ ] Verify no LOD popping
- [ ] Measure memory savings
- [ ] Profile performance impact
- [ ] Update documentation

## Future Enhancements

1. **Predictive preloading:**
   - Track camera velocity
   - Preload in direction of movement
   - Further optimize based on movement patterns

2. **Distance-based preload priority:**
   - Preload closest cells first
   - Lower priority for cells far from camera
   - Implement async loading with priorities

3. **Adaptive border size:**
   - Increase border when camera moving fast
   - Decrease when stationary
   - Balance between safety and efficiency

4. **Frame-based priority:**
   - Higher priority for Frame+1
   - Lower priority for Frame+2, Frame+3
   - Stagger loads across multiple frames
