# GridLODActor Implementation Specification

## Overview

`GridLODActor` is a high-performance water mesh animation system for Unreal Engine 5 that displays pre-computed water simulation frames using a grid of static meshes with dynamic LOD (Level of Detail) switching.

**Last Updated:** December 2024

---

## Architecture

### Core Components

1. **Grid System**: 8×3 grid (24 cells) of `UGridCellComponent` (custom StaticMeshComponent)
2. **Mesh Streaming**: Smart caching system with synchronous loading using `StaticLoadObject`
3. **LOD System**: Distance-based switching between high-resolution and low-resolution meshes
4. **Frame Animation**: 192 frames (886-1078) of pre-computed water simulation
5. **Infinite Tiling**: Support for multiple GridLODActor instances with frame offsets

---

## Mesh Loading Strategy

### Loading Mechanism

**Method:** `StaticLoadObject` with `WaitForStreaming()`
- **Why:** Ensures meshes are fully loaded AND compiled before use
- **Alternative tried:** `LoadObject` (lazy loads), `StreamableManager` (path format issues)
- **Key insight:** `WaitForStreaming()` forces rendering data compilation, preventing "Preparing static mesh" delays

### Cache Architecture

```
MeshCache (TMap<FString, TObjectPtr<UStaticMesh>>)
├── Key: Mesh path (e.g., "/Game/Waves/chunks_ratio_0_03/2_1_mesh_1025")
└── Value: Loaded UStaticMesh pointer

CachedFrames (TSet<int32>)
└── Tracks which frames have been fully cached
```

### Loading Phases

#### 1. OnConstruction (Editor Only)
```cpp
#if WITH_EDITOR
if (!GetWorld()->IsGameWorld())
{
    // Preload 50 frames starting from StartFrame
    PreloadMeshesSynchronous(StartFrame, 50);

    // Set preview mesh for editor viewport
    // ...
}
#endif
```

**Purpose:** Pre-warm cache before pressing Play in editor
**Result:** ~2,400 meshes loaded (50 frames × 24 cells × 2 LODs)
**Memory:** ~156 MB per actor

#### 2. BeginPlay (Runtime Fallback)
```cpp
if (MeshCache.Num() < 100)  // Cache empty or too small
{
    PreloadMeshesSynchronous(GetCurrentFrameFromController(), 20);
}
```

**Purpose:** Ensure smooth startup if OnConstruction cache wasn't preserved
**Result:** ~960 meshes loaded (20 frames × 24 cells × 2 LODs)
**Memory:** ~62 MB per actor

#### 3. Runtime Streaming (During Gameplay)
```cpp
UpdateGridMeshes() → PreloadMeshesForFrameRange()
```

**Purpose:** Load frames ahead of current playback position
**Frames ahead:** 10 frames (`PreloadFrameCount`)
**Method:** Calls `LoadGridMesh()` which uses `StaticLoadObject`

---

## Frame Wrapping Logic

### Animation Range
- **StartFrame:** 886
- **EndFrame:** 1078
- **Total frames:** 193

### Wrapping Implementation
```cpp
int32 AnimationLength = EndFrame - StartFrame + 1;  // 193

if (Frame > EndFrame)
{
    Frame = StartFrame + ((Frame - StartFrame) % AnimationLength);
}
```

**Example:** Frame 1089 wraps to frame 896
- `1089 > 1078` → wrap
- `StartFrame + ((1089 - 886) % 193)` = `886 + 10` = `896`

---

## Memory Management

### Cache Window (Rolling Cache)

```
CacheFrameCount = 20 frames
```

**Behavior:**
- Keeps meshes for 20 frames behind current position
- Old frames outside window are unloaded by `UnloadOldFrames()`
- Prevents unlimited memory growth

**Memory per actor:**
- Active cache: 20 frames × 24 cells × 2 LODs = 960 meshes (~62 MB)
- Preload buffer: 10 frames ahead = 480 meshes (~31 MB)
- **Peak during gameplay:** ~93 MB per actor

### Multi-Actor Setup (5 Instances)

**Frame Offsets:**
- Actor 1: Frame 886
- Actor 2: Frame 981 (offset +95)
- Actor 3: Frame 1076 (offset +190, wraps to ~890)
- Etc.

**Total Memory:**
- 5 actors × 93 MB = **~465 MB during gameplay**
- Initial load: 5 actors × 156 MB = **~780 MB**

---

## LOD System

### Distance-Based LOD Selection

```cpp
int32 GetLODLevelForDistance(float Distance) const
{
    if (Distance < HighLODDistanceThreshold)
        return 0;  // High-res (chunks_ratio_0_03)
    else if (Distance < LowLODDistanceThreshold)
        return 1;  // Low-res (chunks_ratio_0_005)
    else
        return -1; // Culled (hidden)
}
```

### LOD Thresholds (Distance Squared)
- **HighLODDistanceThreshold:** 100,000,000 units² (10,000 units)
- **LowLODDistanceThreshold:** 250,000,000 units² (15,811 units)
- **Culling:** Beyond low LOD threshold

### Mesh Paths
- **High-res (LOD 0):** `/Game/Waves/chunks_ratio_0_03/` (~73 KB per mesh)
- **Low-res (LOD 1):** `/Game/Waves/chunks_ratio_0_005/` (~57 KB per mesh)

---

## Performance Optimizations

### 1. Low-Res Frame Skipping

```cpp
// Only update low-res meshes every other frame
if (LODLevel == 1 && Frame % 2 == 0)
{
    bSkipLowResUpdate = true;
    Cell->CurrentFrame = Frame; // Keep previous mesh visible
}
```

**Rationale:** Low-res meshes are far away, updating every other frame is imperceptible
**Savings:** ~50% reduction in low-res mesh loading

### 2. Selective Preloading

Only preloads meshes for cells that need them based on current LOD state:
```cpp
TArray<FLODPreloadRequest> GetCellsNeedingPreload()
```

**Rules:**
- HIGH LOD cell → preload HIGH meshes for this cell + adjacent cells
- LOW LOD cell → preload LOW meshes for this cell + adjacent cells
- Culled cell → don't preload

### 3. Synchronous Loading with Compilation

```cpp
UStaticMesh* Mesh = Cast<UStaticMesh>(StaticLoadObject(...));
Mesh->WaitForStreaming();  // Force render data compilation
```

**Why:** Prevents deferred "Preparing static mesh" delays during first render
**When:** Used in `PreloadMeshesSynchronous()` and on-demand loading

---

## Infinite Tiling Support

### Frame Offset System

Each GridLODActor can have:
```cpp
int32 FrameOffset;  // Offset added to base frame number
FVector2D WorldPositionOffset;  // Spatial offset for mesh vertices
```

**Example Setup (5 actors):**
```
Actor 1: FrameOffset = 0,   WorldPositionOffset = (0, 0)
Actor 2: FrameOffset = 80,  WorldPositionOffset = (8000, 0)
Actor 3: FrameOffset = 160, WorldPositionOffset = (16000, 0)
Actor 4: FrameOffset = 48,  WorldPositionOffset = (0, 3000)
Actor 5: FrameOffset = 128, WorldPositionOffset = (8000, 3000)
```

### WorldPositionOffset in Shader

Applied in material shader to offset mesh vertex positions:
```
VertexPosition += WorldPositionOffset
```

This creates seamless tiling without mesh duplication.

---

## Key Properties (BlueprintReadWrite)

### Grid Configuration
```cpp
int32 GridWidth = 8;           // Cells horizontally
int32 GridHeight = 3;          // Cells vertically
float CellSize = 1000.0f;      // Size of each cell (cm)
```

### LOD Configuration
```cpp
float HighLODDistanceThreshold = 100000000.0f;  // Distance² for high LOD
float LowLODDistanceThreshold = 250000000.0f;   // Distance² for low LOD
bool bUse2DDistance = true;                     // Ignore Z axis
bool bUseDistanceSquared = true;                // Skip sqrt calculation
```

### Streaming Configuration
```cpp
bool bEnableMeshStreaming = true;     // Enable smart caching
int32 PreloadFrameCount = 10;         // Frames to load ahead
int32 CacheFrameCount = 20;           // Frames to keep in cache
int32 PreloadBorderSize = 0;          // Extra cell border for preloading
bool bUseSelectivePreloading = true;  // Only preload needed LODs
```

### Animation Configuration
```cpp
int32 StartFrame = 886;       // First frame of animation
int32 EndFrame = 1078;        // Last frame of animation
int32 FrameOffset = 0;        // Offset for this actor instance
```

### Infinite Tiling
```cpp
FVector2D WorldPositionOffset = FVector2D::ZeroVector;  // Spatial offset
```

---

## Performance Characteristics

### PC/Editor
- **Startup:** ~2 seconds (loading 50 frames)
- **Initial frames:** Smooth (cached)
- **After frame 50:** Possible slight stutter as new frames stream
- **Memory:** ~465 MB for 5 actors

### Android (Same Settings)
- **Startup:** ~3-5 seconds (loading 50 frames + compilation)
- **Performance:** Similar to PC with mobile-optimized settings
- **Memory:** ~465 MB for 5 actors (acceptable for modern phones)

---

## Debugging Features

### Logging
- `LogTemp` channel for all mesh loading operations
- Frame wrapping diagnostics
- Cache state tracking

### Key Log Messages
```
"GridLODActor: OnConstruction preloading X frames"
"GridLODActor: BeginPlay - Cache has X meshes, current frame is Y"
"GridLODActor: Mesh not in cache, loading on-demand"
"GridLODActor: Synchronous preload complete - X meshes loaded"
```

### Debug Visualization
```cpp
bool bShowDebugVisualization = false;  // Draw colored boxes for LOD states
```

When enabled:
- **Green:** High LOD
- **Yellow:** Low LOD
- **Red:** Culled

---

## File Structure

### Source Files
- `GridLODActor.h` - Class declaration, properties, function signatures
- `GridLODActor.cpp` - Implementation, mesh loading, LOD logic

### Mesh Assets
```
Content/Waves/
├── chunks_ratio_0_03/     # High-res meshes (LOD 0)
│   └── X_Y_mesh_FRAME.uasset
└── chunks_ratio_0_005/    # Low-res meshes (LOD 1)
    └── X_Y_mesh_FRAME.uasset
```

**Naming Convention:**
- `X` = Grid cell X coordinate (0-7)
- `Y` = Grid cell Y coordinate (0-2)
- `FRAME` = Frame number (886-1078)

**Example:** `2_1_mesh_1025.uasset` = Cell (2,1), Frame 1025

---

## Future Optimization Opportunities

### For Android
1. **Reduce grid size:** 8×3 → 6×2 (12 cells instead of 24)
2. **Fewer actors:** 5 → 3 instances
3. **Lower LOD quality:** Force all cells to low-res on mobile
4. **Tighter LOD distances:** Transition to low-res sooner
5. **Async loading:** Use async loading with loading screen

### General
1. **Mesh compression:** Further compress mesh assets
2. **LOD chain:** Add LOD2, LOD3 for even lower resolution at distance
3. **Texture streaming:** Add texture LODs if meshes have textures
4. **Mesh LOD generation:** Auto-generate simplified meshes in editor

---

## Known Issues & Solutions

### Issue 1: "Preparing static mesh" Delay
**Problem:** Meshes loaded but not compiled, causing freeze on first render
**Solution:** Added `WaitForStreaming()` after `StaticLoadObject()`

### Issue 2: Cache Not Persisting from OnConstruction to BeginPlay
**Problem:** Cache cleared when transitioning editor → play mode
**Solution:** Added fallback preloading in BeginPlay if cache empty

### Issue 3: Frame Numbers Beyond EndFrame
**Problem:** Preloading or streaming tried to load frame 1083 (> EndFrame 1078)
**Solution:** Implemented frame wrapping in all loading functions

### Issue 4: "Fast Forward" Playback After Freeze
**Problem:** Frame numbers accumulate during freeze, then animation speeds up
**Solution:** Use time-based frame calculation or pause WaterController during loading

---

## Integration with WaterController

GridLODActor reads current frame from a WaterController blueprint:
```cpp
int32 GetCurrentFrameFromController() const
{
    FProperty* CurrentFrameProperty = WaterController->GetClass()->FindPropertyByName("CurrentFrame");
    // ... read property value
}
```

**Expected Blueprint Property:**
- Name: `CurrentFrame` (int32)
- Updates every frame to drive animation

---

## Testing Checklist

- [ ] PC editor performance with 5 actors
- [ ] Android device performance with 5 actors
- [ ] Memory usage stays under 500 MB
- [ ] No "Preparing static mesh" delays
- [ ] Smooth animation for first 50 frames
- [ ] Frame wrapping works correctly at boundaries
- [ ] LOD transitions are seamless
- [ ] Cache unloading works (no memory leak)
- [ ] Multiple instances with frame offsets
- [ ] WorldPositionOffset creates seamless tiling

---

## Version History

**v1.0 (December 2024)**
- Initial implementation with StaticLoadObject
- Platform-unified settings (PC and Android same)
- Frame wrapping support
- 50-frame OnConstruction preload
- 20-frame rolling cache
- Low-res frame skipping optimization
