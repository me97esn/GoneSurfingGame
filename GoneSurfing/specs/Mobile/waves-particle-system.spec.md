# Spec: Waves particle system

## Overview
This is a spec for generating code to display the water as a particle system

## Objective
Create a C++ Actor that reads wave data and updates or spawns gpu particles with location and orientation according to the wave data
## Requirements

### Functional Requirements
1. **FR-1**: The class shall have a UPROPERTY GPU Particle System
2. **FR-2**: The class shall have a UPROPERTY WaveHeight.
3. **FR-3**: The Tick function should call a not yet implemented function in the WaveHeight class that fetches wave data around a given location, with a customizable distance between data points.
This functions returns an array of tuple<double, FVector>, where the double is z height of the water in the given location, the FVector is the normal of the water surface in this location.
 It should make the particle system place a particle in the location where x is the world x location of the actor, y is the world locations of the actor, and z is the double from the wave height functions.
 It should also orient the particles to face the normal os the water surface.
4. The class should have the UPROPERTY WaveDataRowName, used when calling WaveHeight

### Non-Functional Requirements
1. **NFR-1**: The implementation shall be simple and readable
2. **NFR-2**: The code shall follow project coding standards

## Acceptance Criteria


## Implementation Details

## Notes for AI Agent
- This spec is implementation-agnostic but suggests C++ for Unreal Engine
- Implement as C++
- Ensure proper UE logging categories are used
- Do not create the Particle system. This is already done in the editor.
- Use Niagara system with custom data interface
- Feed the waveData array to the Niagara GPU-based particle system
- Use Niagara Data interface Array float 3 when passing the data to the Niagara system. 

## Status
- [x] Specification written
- [x] Implementation complete
- [ ] Tests passing
- [ ] Code reviewed
- [ ] Merged to main

## Implementation Notes

### Initial Implementation (2025-10-10):
- Added `GetWaveDataAroundLocation` method to [WaveHeight.h](../../Source/GoneSurfing/WaveHeight.h:152)
- Created [WaveParticleSystemActor.h](../../Source/GoneSurfing/WaveParticleSystemActor.h)
- Created [WaveParticleSystemActor.cpp](../../Source/GoneSurfing/WaveParticleSystemActor.cpp)

### Updated Implementation with Niagara Integration (2025-10-10):
- Replaced generic ParticleSystemComponent with **UNiagaraComponent**
- Added Niagara module to [GoneSurfing.Build.cs](../../Source/GoneSurfing/GoneSurfing.Build.cs:11)
- Implemented array-based data feeding to Niagara via **Array Float3** (Vector) parameters
- Uses `UNiagaraDataInterfaceArrayFunctionLibrary::SetNiagaraArrayVector()` for optimal data transfer
- Added configurable parameter names for Niagara user parameters

### Key Features:
1. **WaveHeight class** has `GetWaveDataAroundLocation()` that returns wave heights and normals in a grid
2. **WaveParticleSystemActor** has configurable grid size and sampling distance via UPROPERTYs:
   - `GridSizeX` and `GridSizeY` - control the number of particles
   - `DistanceBetweenPoints` - controls spacing between particles
   - `WaveDataRowName` - the data table row name for wave lookup
3. **Tick function** fetches wave data and sends it to Niagara every frame via two Vector arrays:
   - `PositionArray` - FVector array with particle positions (x, y, z)
   - `NormalArray` - FVector array with surface normals for orientation
4. **Niagara Integration**: Data is fed to GPU via `UNiagaraDataInterfaceArrayFunctionLibrary::SetNiagaraArrayVector()` calls

### How to Use in Editor:
1. Create a Niagara System with the following **User Parameters** (Niagara Array Vector/Float3):
   - `PositionArray` (type: Niagara.ArrayVector or Niagara.ArrayFloat3)
   - `NormalArray` (type: Niagara.ArrayVector or Niagara.ArrayFloat3)
2. In your Niagara emitter, read these arrays using "Get Niagara Array Vector" or "Get Niagara Array Vector Value" nodes
3. Use the `PositionArray` to set particle positions (read by index)
4. Use the `NormalArray` to orient particles (read by index)
5. Add WaveParticleSystemActor to your level
6. Assign the Niagara System asset to the NiagaraComponent in the editor
7. Set the WaveHeight reference to your AWaveHeight actor
8. Configure grid size and spacing as needed

