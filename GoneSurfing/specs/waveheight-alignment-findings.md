# WaveHeight Alignment Findings

## Summary
Documentation of the coordinate system alignment process between WaveHeight data and GridLODActor mesh geometry.

## Key Discoveries

### 1. Coordinate System Mapping

**Initial Problem:** The spec suggested an unusual coordinate mapping (X→Z, Y→Y), which was causing confusion.

**Solution:** Changed to standard Unreal coordinates throughout WaveHeight.cpp:
- **Metadata X → Local X** (forward axis)
- **Metadata Y → Local Y** (right axis)
- **Wave height → Local Z** (up axis)

**Files Modified:**
- [WaveHeight.cpp:19](../Source/GoneSurfing/WaveHeight.cpp#L19) - `convertWorldLocationToSampleIndexes` now uses `relativeLocation.X, relativeLocation.Y`
- [WaveHeight.cpp:132](../Source/GoneSurfing/WaveHeight.cpp#L132) - Wave height assigned to `relativeLocation.Z` (fixed TODO comment)
- [WaveHeight.cpp:488-491](../Source/GoneSurfing/WaveHeight.cpp#L488-L491) - Debug visualization corners use `FVector(startX, startY, 0)`
- [WaveHeight.cpp:558](../Source/GoneSurfing/WaveHeight.cpp#L558) - Sample points use `FVector(worldX, worldY, waveHeight)`

### 2. Rotation Alignment

**Problem:** GridLODActor has rotation **Yaw=90°**, while WaveHeight was initially at Yaw=0°

**Solution:** WaveHeight actor must match GridLODActor's rotation:
```
Rotation: (Pitch=0, Yaw=90, Roll=0)
```

This ensures both actors have the same orientation in world space.

### 3. Scaling Chain

**GridLODActor scaling:**
- Mesh import scale: **200×** (meshes imported at 200% of original size)
- Actor scale: **0.1×** (scaled down to 10%)
- **Effective scale: 200 × 0.1 = 20×**

**WaveHeight scaling:**
- Must match GridLODActor's effective scale: **20×**
```
Scale: (X=20, Y=20, Z=20)
```

### 4. Position Offset Calculation

**Wave data coordinate range (from metadata):**
- X: -60 to +100 (160 units wide, center at X=20)
- Y: -280 to +170 (450 units tall, center at Y=-55)

**With Yaw=90° rotation and Scale=20:**
- Local center: (20, -55, 0)
- After rotation and scaling, this creates an offset from GridLODActor's origin
- Offset calculation:
  - X offset = +1100 (from Y component after rotation)
  - Y offset = +400 (from X component after rotation)

**Final aligned position:**
```
GridLODActor position: (1155, -2265, 0)
WaveHeight position: (2255, -1865, 0)
```

However, **manual tuning was required** and the final working transform was:
```json
{
  "RelativeLocation": "(X=6015.000000,Y=-1365.000000,Z=350.000000)",
  "RelativeRotation": "(Pitch=0.000000,Yaw=90.000000,Roll=0.000000)",
  "RelativeScale3D": "(X=14.000000,Y=14.000000,Z=14.000000)"
}
```

This indicates:
- **Scale was 14× instead of 20×** - suggests the mesh effective scale might be different than calculated
- **Position offset doesn't match the formula** - the baked mesh positions may have a different origin than expected

### 5. Data Coverage vs Mesh Coverage

**Important Discovery:** The WaveHeight data **does not cover the full extent of the mesh geometry**.

The sample data is only available for a subset of the mesh area, not all the way to the edges. This means:
- Corner alignment is not the right approach
- Sample point visualization is needed to see actual data coverage
- Tiling will repeat this finite data region, which is smaller than the mesh

### 6. Debug Visualization Frame Wrapping

**Problem:** Debug visualization was using `DebugVisualizationFrame` directly, causing errors:
```
LogDataTable: Warning: 'Frame_0' not in DataTable
```

**Solution:** Changed debug visualization to use `WrapAndConstructFrameName()` ([WaveHeight.cpp:542](../Source/GoneSurfing/WaveHeight.cpp#L542)), ensuring frame numbers are always valid:
- Frame 0 → wraps to StartFrame (886)
- Any frame → properly wrapped to valid range

## Configuration Summary

### For Single GridLODActor Alignment

1. **WaveHeight Transform:**
   - Position: Manually tuned to align sample points with mesh
   - Rotation: (0, 90, 0) - must match GridLODActor
   - Scale: ~14× (tune to match mesh scale)

2. **Debug Settings:**
   - `bShowDebugVisualization = true`
   - `bShowSamplePoints = true`
   - `SamplePointStride = 5` (or adjust based on performance)
   - `DebugVisualizationFrame = 886` (or current frame)

3. **Visual Alignment Process:**
   - Enable sample point visualization
   - Adjust WaveHeight position/scale until sample points align with mesh geometry
   - Use corner markers and boundary box as general guides only

## Implications for Infinite Tiling

1. **Data repeats at:**
   - X direction: every 160 units (metadata space)
   - Y direction: every 450 units (metadata space)
   - After 14× scale: 2240 × 6300 units in world space

2. **Mesh coverage is larger than data coverage:**
   - GridSize suggests mesh is ~1000 units (after 0.1× scale)
   - But data only covers a portion of this
   - Tiling will create repeating patterns visible to player

3. **Alignment must be maintained across all tiles:**
   - All GridLODActor instances need same rotation (Yaw=90°)
   - WaveHeight actor positioning must account for tile offsets
   - Scale must remain consistent (14×)

## Open Questions

1. **Why is the scale 14× instead of calculated 20×?**
   - Mesh import scale might not be exactly 200×
   - Or GridLODActor might have additional scaling applied somewhere

2. **What is the true origin of the baked mesh positions?**
   - Formula-based position calculation didn't work
   - Manual tuning was required
   - Need to understand the mesh coordinate system better

3. **How much of the mesh is actually covered by data?**
   - Sample point visualization shows partial coverage
   - Need to measure actual coverage percentage

## Next Steps

1. Enable sample point visualization with current transform
2. Measure the actual data coverage area on the mesh
3. Determine if additional data is needed or if partial coverage is acceptable
4. Test tiling with multiple GridLODActor instances
5. Verify wave heights match mesh geometry visually

## Files Modified

- [WaveHeight.cpp](../Source/GoneSurfing/WaveHeight.cpp) - Coordinate system fixes, debug visualization
- [WaveHeight.h](../Source/GoneSurfing/WaveHeight.h) - Debug properties, new Auto functions
- [waveheight-infinite-tiling.md](waveheight-infinite-tiling.md) - Original spec (coordinate mapping now corrected)

## Related Issues

- The unusual coordinate mapping in the original spec was misleading
- GridLODActor's 90° rotation was not initially obvious
- Scaling chain calculation was correct in theory but wrong in practice
