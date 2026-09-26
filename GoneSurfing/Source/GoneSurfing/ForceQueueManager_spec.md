# ForceQueueManager Specification

## Overview
A static utility class that provides shared logic for enqueueing forces and spreading them over multiple frames. This allows both Buoyancy and FluidDynamics (and any future systems) to spread forces across frames without duplicating code.

## Purpose
- Eliminate code duplication between Buoyancy and FluidDynamics force spreading logic
- Provide a reusable system for spreading forces over multiple frames
- Keep location logic separate - forces are applied at the actor's **current** location when retrieved, not where it was when enqueued

## Design Philosophy
**Key Insight**: The surfboard and its components (Buoyancy, FluidDynamics) move every frame. Forces should be applied at the actor's **current location** when the force chunk is retrieved, not at the location where the force was originally enqueued.

Therefore:
- The queue stores **only force vectors** (magnitude and direction)
- **No location data** is stored in the queue
- Each actor applies retrieved forces at its own current location

## Data Structure
Each class using the ForceQueueManager will own:
```cpp
std::deque<std::deque<FVector>> ForceQueue;
```

- **Outer deque**: Contains multiple independent force applications
- **Inner deque**: Contains chunks of a single force, applied one per frame

### Static Helper Class
```cpp
class ForceQueueManager
{
public:
    // Enqueue a force to be spread over multiple frames
    static void EnqueueForce(
        std::deque<std::deque<FVector>>& queue,
        FVector force,
        int numberOfChunks
    );

    // Get forces to apply this frame (one chunk from each active force)
    static TArray<FVector> GetEnqueuedForces(
        std::deque<std::deque<FVector>>& queue
    );
};
```

## Implementation Details

### EnqueueForce
1. Divide the input force by `numberOfChunks` to get `forceChunk`
2. Create an inner deque
3. Fill it with `numberOfChunks` copies of `forceChunk`
4. Push the inner deque to the outer queue

### GetEnqueuedForces
1. Create an empty output array
2. Iterate through all inner queues in the outer queue
3. For each non-empty inner queue:
   - Pop the front `FVector` and add to output array
4. Remove any inner queues that became empty
5. Return the output array

## Migration Plan

### Phase 1: Create ForceQueueManager
- Create `ForceQueueManager.h` and `ForceQueueManager.cpp`
- Implement static methods

### Phase 2: Update Buoyancy
- Keep `ForceQueue` as `std::deque<std::deque<FVector>>` (no change needed)
- Update `enqueForce()` to call `ForceQueueManager::EnqueueForce()`
- Update `getEnquedForces()` to call `ForceQueueManager::GetEnqueuedForces()`
- Maintain existing function signatures - no breaking changes

### Phase 3: Update FluidDynamics
- Add `std::deque<std::deque<FVector>> ForceQueue;` member
- Add `enqueForce()` and `getEnquedForces()` methods that wrap ForceQueueManager calls
- Apply retrieved forces at the FluidDynamics actor's current location

## Usage Example

### In Buoyancy
```cpp
void ABuoyancy::enqueForce(FVector forceToEnque, int number_of_chunks)
{
    ForceQueueManager::EnqueueForce(ForceQueue, forceToEnque, number_of_chunks);
}

TArray<FVector> ABuoyancy::getEnquedForces()
{
    return ForceQueueManager::GetEnqueuedForces(ForceQueue);
}
```

The calling code then applies these forces at the Buoyancy actor's current location.

### In FluidDynamics
```cpp
void AFluidDynamics::enqueForce(FVector force, int numberOfChunks)
{
    ForceQueueManager::EnqueueForce(ForceQueue, force, numberOfChunks);
}

TArray<FVector> AFluidDynamics::getEnquedForces()
{
    return ForceQueueManager::GetEnqueuedForces(ForceQueue);
}
```

The calling code then applies these forces at the FluidDynamics actor's current location.

## Benefits
1. **No code duplication**: Logic exists in one place
2. **Consistent behavior**: Both systems spread forces identically
3. **Maintainability**: Bug fixes and improvements happen in one location
4. **Extensibility**: Other systems can easily adopt force spreading
5. **Backward compatibility**: Buoyancy's existing API remains unchanged
6. **Correct location handling**: Forces are always applied at the actor's current location, not stale historical positions

## Questions to Resolve
1. Should ForceQueueManager be a class with static methods or a namespace with free functions?
2. Do we want any debug logging in ForceQueueManager, or leave that to the calling code?
