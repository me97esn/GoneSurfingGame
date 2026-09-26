# LOD Grid System Specification

## Overview
Distance-based Level of Detail (LOD) system for grid-based stop-motion mesh animation on mobile devices.

## Current System
- **MeshArrayActor**: Loads one mesh at a time for stop-motion animation. Uses Niagara for this.
- **Problem**: High-resolution meshes cause performance issues (10 FPS on mobile)
- **Current Performance**: 50 FPS with low-resolution meshes, 10 FPS with high-resolution meshes
- **Bottleneck**: Polygon count

## Requirements

### Mesh Organization
- **Grid Layout**: 3×8 grid of mesh cells (24 total cells)
- **Grid Size**: ~100m × 100m world space
- **Cell Size**: ~33m × 12.5m per cell
- **Animation Frames**: ~200 frames
- **Mesh Naming**: `{x}_{y}_mesh_{frame}.obj` (e.g., `0_0_mesh_800.obj`)
- **LOD Folders**:
  - `high_res/` - Full resolution meshes
  - `low_res/` - Reduced resolution meshes
  - `med_res/` - (Future) Medium resolution meshes

### Performance Constraints
- Each grid cell uses a **unique mesh** per frame (no instancing opportunity)
- **Minimum draw calls**: 24 per frame (one per visible grid cell) if all of the meshes are in the cameras view. Meshes outside of the view can be skipped.
- **Moving camera**: LOD levels must update dynamically every frame
- **Target platform**: Mobile devices
- **Target performance**: 35-45 FPS (vs current 10 FPS with high-res)

## Technical Design

### Architecture
**Custom Grid Actor** with 24 StaticMeshComponent children arranged in 3×8 grid.

### Per-Frame Update Logic
- The animation does not start on frame 0. It should read the start and end frame from the WaterController (Which is a blueprint). Look at how MeshArrayActor does this.

```
For each frame:
  1. Get current animation frame number from the Watercontroller similar to how MeshArrayActor does.
  2. Get camera world position

  For each grid cell (0-23):
    3. Calculate grid cell center position in world space
    4. Calculate distance from camera to grid cell center
    5. Determine LOD level based on distance
    6. Determine visibility based on distance
    7. Determine visibility based on camera look direction and camera FOV.
    7. Load appropriate mesh from LOD folder
    8. Update StaticMeshComponent
```

### LOD Selection Algorithm (Phase 1: Two LOD Levels)

```
Distance Thresholds:
  - LOD_DISTANCE_THRESHOLD: 50m (tunable)
  - MAX_VISIBLE_DISTANCE: 150m (tunable)

Per Grid Cell:
  if (distance > MAX_VISIBLE_DISTANCE):
    -> Disable component (not rendered)
  else if (distance < LOD_DISTANCE_THRESHOLD):
    -> folder = "high_res/"
  else:
    -> folder = "low_res/"

  mesh_path = "{folder}{x}_{y}_mesh_{frame}.obj"
```

### LOD Selection Algorithm (Phase 2: Three LOD Levels - Future)

```
Distance Thresholds:
  - LOD_THRESHOLD_HIGH: 30m (tunable)
  - LOD_THRESHOLD_MED: 80m (tunable)
  - MAX_VISIBLE_DISTANCE: 150m (tunable)

Per Grid Cell:
  if (distance > MAX_VISIBLE_DISTANCE):
    -> Disable component
  else if (distance < LOD_THRESHOLD_HIGH):
    -> folder = "high_res/"
  else if (distance < LOD_THRESHOLD_MED):
    -> folder = "med_res/"
  else:
    -> folder = "low_res/"
```

### Distance Calculation

```
Grid Cell Center Position:
  - Grid origin: Actor's root position
  - Cell size: grid_total_size / grid_dimensions
  - Cell center = origin + (x * cell_width, y * cell_height, 0) + (cell_width/2, cell_height/2, 0)

Distance Calculation:
  - 2D distance since Z doesn't matter: sqrt((cx-px)² + (cy-py)²)
```

## Component Structure

### GridLODActor (Custom Actor)
- **Root Component**: SceneComponent
- **Children**: 24 StaticMeshComponent instances
- **Properties**:
  - `float LODDistanceThreshold` - Distance for high/low switch (default: 50m)
  - `float MaxVisibleDistance` - Distance beyond which cells are hidden (default: 150m)
  - `FString HighResFolder` - Path to high-res meshes
  - `FString LowResFolder` - Path to low-res meshes
  - `int32 GridWidth` - Grid columns (3)
  - `int32 GridHeight` - Grid rows (8)
  - `FVector2D GridSize` - Physical size in meters (100, 100)

### Grid Cell Component
Each of 24 StaticMeshComponent instances represents one grid cell:
- **Position**: Set in constructor based on grid layout
- **Grid Coordinates**: Stored as metadata (x: 0-2, y: 0-7)
- **Current LOD**: Tracked for optimization (avoid reloading same mesh)

## Optimizations

### 1. Mesh Caching
- **Avoid redundant loads**: Track current mesh per component, only update if LOD or frame changes
- **Pre-loading**: Consider loading next frame's meshes asynchronously

### 2. Distance Calculation Optimization
- **Calculate once**: Store camera position, reuse for all 24 cells
- **2D vs 3D**: grid is flat, use 2D distance (cheaper)
- **Distance squared**: For threshold comparisons, use squared distance to avoid sqrt()

### 3. LOD Hysteresis (Future Enhancement)
- **Problem**: Camera movement near threshold causes rapid LOD switching (popping)
- **Solution**: Use different thresholds for switching up vs down
  ```
  Switching to high-res: distance < 45m
  Switching to low-res: distance > 55m
  (10m hysteresis band)
  ```

### 4. Frustum Culling (Future Enhancement)
- Disable cells outside camera frustum
- Could reduce visible cells from 24 to 12-16
- Requires camera frustum calculation

## Memory Management

### Current Frame Strategy (Phase 1)
- Load all 24 meshes for current frame on-demand
- Rely on Unreal's asset caching
- Total memory: 24 meshes × 2 possible LODs = up to 48 meshes in memory

### Streaming Strategy (Phase 2 - Future)
- **Active frame**: Keep current frame's meshes loaded (24 meshes, all LODs)
- **Pre-load buffer**: Load next 2-3 frames in background
- **Unload policy**: Unload frames >3 frames old
- **Async loading**: Use `LoadObject<UStaticMesh>()` async variant

## Performance Targets

### Expected Distribution (50m threshold, moving camera)
- **High-res cells**: 6-10 cells (nearest to camera)
- **Low-res cells**: 8-12 cells (mid-distance)
- **Culled cells**: 4-8 cells (beyond 150m)

### Performance Estimate
- **Current (all high-res)**: 10 FPS
- **Current (all low-res)**: 50 FPS
- **With LOD system**: 35-45 FPS target

## Tunable Parameters

### Distance Thresholds
These must be tuned on target mobile devices:

| Parameter | Initial Value | Expected Range | Purpose |
|-----------|---------------|----------------|---------|
| LODDistanceThreshold | 50m | 30-70m | High/low switch point |
| MaxVisibleDistance | 150m | 100-200m | Culling distance |
| LODThresholdHigh | 30m | 20-40m | High/med switch (future) |
| LODThresholdMed | 80m | 60-100m | Med/low switch (future) |

### Tuning Process
1. Start with default values
2. Test on target mobile device
3. Observe:
   - Visual quality (is transition noticeable?)
   - Performance (FPS in typical gameplay)
   - Pop-in artifacts
4. Adjust thresholds iteratively
5. Consider different values for different device tiers

## Implementation Phases

### Phase 1: Basic Two-LOD System
- [x] Technical specification
- [ ] Create GridLODActor class
- [ ] Implement 24 StaticMeshComponent grid layout
- [ ] Implement distance calculation
- [ ] Implement two-LOD selection logic
- [ ] Implement mesh loading/switching
- [ ] Add tunable parameters (exposed to Blueprint)
- [ ] Test on mobile device
- [ ] Tune LOD thresholds

### Phase 2: Three-LOD System (Future)
- [ ] Add med_res folder support
- [ ] Update LOD selection logic for three levels
- [ ] Add additional threshold parameters
- [ ] Re-tune thresholds

### Phase 3: Advanced Optimizations (Future)
- [ ] LOD hysteresis
- [ ] Async mesh loading/streaming
- [ ] Frustum culling
- [ ] Frame prediction/pre-loading

## Testing Strategy

### Performance Testing
- Measure FPS on target mobile devices
- Test with camera at various distances/angles
- Profile draw calls, poly count, memory usage

### Visual Quality Testing
- Check for visible LOD transitions/popping
- Verify high-res is used where quality matters
- Ensure low-res is acceptable at distance

### Stress Testing
- Rapid camera movement
- Animation frame changes during camera movement
- Edge cases (very close, very far)

## Success Criteria
- ✅ Achieve 35-45 FPS on target mobile device
- ✅ Maintain visual quality for nearby meshes
- ✅ No noticeable popping during normal camera movement
- ✅ System is tunable via exposed parameters
- ✅ Easy to add third LOD level in future

## Open Questions
1. Camera movement patterns - are there common angles/positions to optimize for?
2. Animation playback speed - does frame rate affect LOD switching frequency?
3. Multiple grid actors - will there be multiple 3×8 grids in the scene?
4. Device targeting - specific mobile devices to optimize for?
