# WaveHeight Infinite Tiling Specification

## Overview
Enable WaveHeight to provide wave data for any world position by repeating/tiling the finite data table, similar to how GridLODActor repeats mesh cells to create an endless ocean.

## Current Behavior
- WaveHeight stores wave samples in a finite data table
- Data table covers a bounded region defined by metadata (start_trace_x/y, len_x/y, step_size)
- Queries outside this region return 0 or invalid data ([WaveHeight.cpp:280-298](../Source/GoneSurfing/WaveHeight.cpp#L280-L298))

## Desired Behavior
- WaveHeight should tile/repeat infinitely in X and Y directions
- Any world position should map to a valid sample by wrapping coordinates using modulo
- Maintains seamless continuity across tile boundaries
- Frame offset allows temporal tiling (already partially supported)

## Data Table Structure

### Metadata (FWaveSamplesMetadataStruct)
```cpp
- step_size: Distance between samples (e.g., 3.0 units)
- len_x: Number of samples in X direction
- len_y: Number of samples in Y direction
- start_trace_x: Starting X coordinate in world space
- start_trace_y: Starting Y coordinate in world space
- multiplier: Scale factor for stored integer values
```

### Coverage Example
If metadata specifies:
- start_trace_x = 0, start_trace_y = 0
- len_x = 100, len_y = 200
- step_size = 3.0

Then the data table covers:
- X range: [0, 300) world units (100 samples * 3.0 step_size)
- Y range: [0, 600) world units (200 samples * 3.0 step_size)

## Implementation Design

### New Function: convertCoordToSampleIndexWithTiling

**Location:** [WaveHeight.cpp](../Source/GoneSurfing/WaveHeight.cpp)

**Signature:**
```cpp
tuple<float, float> AWaveHeight::convertCoordToSampleIndexWithTiling(float x, float y)
```

**Algorithm:**
1. Calculate table dimensions in world space:
   - `tableWorldWidth = len_x * step_size`
   - `tableWorldHeight = len_y * step_size`

2. Transform world coordinates to table's local space:
   - `localX = x - start_trace_x`
   - `localY = y - start_trace_y`

3. Apply modulo wrapping (handle negative coordinates):
   ```cpp
   localX = fmod(localX, tableWorldWidth);
   if (localX < 0) localX += tableWorldWidth;

   localY = fmod(localY, tableWorldHeight);
   if (localY < 0) localY += tableWorldHeight;
   ```

4. Convert to sample indices:
   - `sampleIndexX = localX / step_size`
   - `sampleIndexY = localY / step_size`

5. Return `(sampleIndexX, sampleIndexY)`

**Edge Cases:**
- Negative coordinates: fmod can return negative values, add table dimension to wrap correctly
- Exactly on boundary: fmod(300, 300) = 0, correctly wraps to start
- Very large coordinates: fmod handles this efficiently

### Integration Points

**1. Replace convertWorldLocationToSampleIndexes ([WaveHeight.cpp:15](../Source/GoneSurfing/WaveHeight.cpp#L15))**
```cpp
tuple<float, float> AWaveHeight::convertWorldLocationToSampleIndexes(FVector location)
{
    FVector relativeLocation = GetRootComponent()->GetComponentTransform().InverseTransformPosition(location);
    return convertCoordToSampleIndexWithTiling(relativeLocation.Z, relativeLocation.Y);  // Changed
}
```

**2. Update convertCoordToSampleIndex to use tiling ([WaveHeight.cpp:21](../Source/GoneSurfing/WaveHeight.cpp#L21))**
- Option A: Replace implementation with tiling logic
- Option B: Keep both functions (non-tiling for legacy, tiling as new)
- Recommendation: Replace to ensure all code paths use tiling

**3. Remove bounds checks in calcSurroundingSamples ([WaveHeight.cpp:280-298](../Source/GoneSurfing/WaveHeight.cpp#L280-L298))**
- Current code returns 0 for out-of-bounds
- With tiling, all coordinates are valid
- Still need bounds check for sample array access (after wrapping)

### Coordinate System Alignment

**Problem:** GridLODActor meshes and WaveHeight data don't align
- Different scales (mesh cell size ≠ wave step_size)
- Different origins (mesh world position ≠ wave start_trace_x/y)
- They have the same orientation though.

**Solution Options:**

#### Option A: Transform Configuration (Recommended)
Add properties to WaveHeight actor:
```cpp
UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tiling")
FVector TileScale = FVector(1.0f, 1.0f, 1.0f);

UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tiling")
FVector TileOffset = FVector(0.0f, 0.0f, 0.0f);
```

Apply in convertCoordToSampleIndexWithTiling:
```cpp
// Apply scale and offset before tiling calculation
localX = (x * TileScale.X) - (start_trace_x + TileOffset.X);
localY = (y * TileScale.Y) - (start_trace_y + TileOffset.Y);
```

#### Option B: Blueprint-Level Alignment
- Position WaveHeight actor in world to match GridLODActor origin
- Scale WaveHeight actor transform to match mesh scale
- Simpler but less flexible

#### Option C: Automatic Scale Detection
- Calculate scale from GridLODActor's GridSize vs WaveHeight's table dimensions
- Requires reference to GridLODActor (coupling)
- Not recommended

### Frame Tiling

**Current:** Frame offset is manually specified
**Enhancement:** Auto-wrap frame numbers using modulo

Add to relevant functions:
```cpp
int32 AWaveHeight::WrapFrameNumber(int32 Frame) const
{
    if (TotalFrames <= 0) return Frame;

    int32 wrapped = Frame % TotalFrames;
    if (wrapped < 0) wrapped += TotalFrames;
    return wrapped;
}
```

Apply in waveHeightAndNormal and calculateWaveVelocity before data table lookup.

## Debug Visualization

### Purpose
Before implementing tiling, we need a way to visualize the finite WaveHeight data region to:
1. See the exact boundaries of the data table in world space
2. Understand the coordinate system (which axis is X, which is Y)
3. Align the WaveHeight actor with GridLODActor by visual inspection
4. Verify scale and offset calculations are correct

### Features

**Boundary Visualization:**
- Draw a rectangular box showing the finite data region boundaries
- Use the metadata (start_trace_x/y, len_x/y, step_size) to calculate corners
- Transform corners to world space using actor's transform
- Draw vertical lines at corners for visibility

**Corner Markers:**
- Large colored spheres at the four corners:
  - Red: Origin (start_x, start_y)
  - Green: (start_x, end_y)
  - Blue: (end_x, end_y)
  - Yellow: (end_x, start_y)
- Helps identify orientation and alignment

**Sample Point Visualization:**
- Optional: Draw spheres at sample locations
- Use stride parameter to avoid drawing too many points (e.g., every 5th sample)
- Color-code by wave height (blue=low, green=mid, red=high)
- Show actual wave data for a specific frame

**Labels:**
- Display dimensions (world units and sample count)
- Show step_size
- Label corners with coordinates
- Show number of points drawn

### Implementation

**Properties (in WaveHeight.h):**
```cpp
UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
bool bShowDebugVisualization = false;

UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
bool bShowDataBoundary = true;

UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
bool bShowSamplePoints = false;

UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
int32 SamplePointStride = 5;

UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
float DebugSphereSize = 10.0f;

UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
int32 DebugVisualizationFrame = 0;

UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
bool bShowCornerMarkers = true;

UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
bool bShowLabels = true;
```

**Function:**
```cpp
void DrawDebugVisualization();  // Called from Tick() when enabled
```

### Usage Workflow

1. **Enable visualization:**
   - Select WaveHeight actor in editor
   - Set `bShowDebugVisualization = true`
   - Set `bShowDataBoundary = true`
   - Set `bShowCornerMarkers = true`

2. **See the data region:**
   - Start PIE or Simulate
   - Cyan box shows the finite data region
   - Colored spheres mark corners

3. **Align with GridLODActor:**
   - Move/rotate/scale WaveHeight actor
   - Watch the cyan boundary box align with the water mesh
   - Corner markers should align with mesh grid cells

4. **Verify sample data (optional):**
   - Enable `bShowSamplePoints = true`
   - Set `SamplePointStride` to reduce density (5-10 recommended)
   - Set `DebugVisualizationFrame` to desired frame
   - See actual wave heights as colored spheres

5. **Record alignment parameters:**
   - Once aligned, note the actor's Position, Rotation, Scale
   - These become the basis for TileScale/TileOffset calculations

### Coordinate System Notes

WaveHeight uses an unusual coordinate mapping:
- X in metadata → Z in Unreal world space
- Y in metadata → Y in Unreal world space
- Wave height → X in Unreal world space (vertical axis)

The debug visualization accounts for this by constructing corners as:
```cpp
FVector LocalCorner = FVector(0, startY, startX);  // (height=0, Y, Z)
```

## Testing Strategy

### Unit Tests
1. **Wrapping Logic:**
   - Position (0, 0) → Sample (0, 0)
   - Position (300, 0) → Sample (0, 0) [wraps to start]
   - Position (150, 0) → Sample (50, 0) [mid-tile]
   - Position (-10, 0) → Sample (96.67, 0) [negative wraps]

2. **Continuity:**
   - Query position (299, 0) and (301, 0)
   - Heights should be nearly identical (seamless tiling)

3. **Scale/Offset:**
   - With TileScale = (2, 2), position (600, 0) → Sample (0, 0)
   - With TileOffset = (10, 0), position (10, 0) → Sample (0, 0)

### Visual Tests
1. **Endless Ocean:**
   - Move camera far from origin (1000+ units)
   - Water animation should continue seamlessly
   - No visible tiling artifacts at boundaries

2. **Alignment Verification:**
   - Place debug spheres at GridLODActor cell corners
   - Query WaveHeight at same positions
   - Verify height values match visual mesh

3. **Performance:**
   - Test with camera moving at high speeds
   - Verify no hitches from tiling calculations
   - fmod is fast but profile if needed

## Implementation Checklist

- [ ] Add convertCoordToSampleIndexWithTiling function
- [ ] Update convertWorldLocationToSampleIndexes to use tiling
- [ ] Update/replace convertCoordToSampleIndex with tiling logic
- [ ] Remove out-of-bounds early returns in calcSurroundingSamples
- [ ] Add TileScale and TileOffset properties (Option A)
- [ ] Add WrapFrameNumber function and apply to frame lookups
- [ ] Update header file with new function declarations
- [ ] Test wrapping logic with various coordinates
- [ ] Test negative coordinate handling
- [ ] Verify continuity across tile boundaries
- [ ] Align WaveHeight with GridLODActor visually
- [ ] Document configuration parameters in Blueprint

## Configuration Example

For a setup where:
- GridLODActor cell size: 100x100 units
- WaveHeight step_size: 3 units, len_x=100, len_y=200

Configuration:
```
WaveHeight Actor:
- Position: Match GridLODActor origin
- TileScale: (1.0, 1.0, 1.0) // Adjust if mesh scale differs
- TileOffset: (0.0, 0.0, 0.0) // Fine-tune alignment
```

Expected result: Wave heights repeat every 300x600 units, aligned with GridLODActor mesh tiles.

## Future Enhancements

1. **Seamless Tiling:**
   - Ensure data table edges match (periodic boundary conditions)
   - May require regenerating simulation data with wrapping

2. **Multiple Tile Variations:**
   - Support multiple data tables for variety
   - Randomly select which tile to use per region

3. **LOD for Wave Data:**
   - Use lower resolution samples for distant queries
   - Match GridLODActor's LOD system

4. **Spatial Hashing:**
   - Cache frequently queried regions
   - Optimize if profiling shows performance issues
