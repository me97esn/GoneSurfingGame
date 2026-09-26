# Spec: wave display mobile
## Overview
Create an actor made for fetching data and sending to a Niagara particle system

## Objective
Since Alembic isn't supported on mobile, I need another way to display the ocean. This uses particles.

## Requirements

### Functional Requirements
1. **FR-1**: Create an Actor in C++. It should have property 'rowname' of type FName.
2. **FR-2**: It should have a property of type FWavePointsDataStruct2.
### Non-Functional Requirements
3. **FR-3**: It should have a reference to a Niagara Actor
4. **FR-4**: Every Tick: tt should fetch a row from the FWavePointsDataStruct2 using the rowname as key. It should send the 'Positions', 'Normals' and 'Scales' to the Niagara actor which has user parameters of these names.
5. **FR-5**: **FR-3** and **FR-4** are no longer valid.
6. **FR-6**: It should use the property NumberOfSlices to read different frames from the data table. It should the same number of frames as numberOfSlices. It should use the properties RowName and FrameIncrement to know which frames to read. If the frame to read is higher then the end frame, it should wrap around and continue from the start frame. The start frame and end frame should be properties that is set in a BP inheriting from the C== class WaveDisplayMobile.
6. **FR-7**: It should add a scale and offset to each of the locations. For the first slice, it should use the ParticlePositionOffset together with ParticlePositionScale to calculate how much offset to add to the position. For the second slide, it should also include the index of the slice.
7. **FR-8**: For the first slice, it should use the current frame when deciding which frame data to fetch. It should construct the RowName (Which now no longer is a property but now a variable scoped inside of the loop) by combining "Frame_" with currentFrame, andjusted by current slice.
8. **FR-9**: It should adjust the offset of each of the positions, so that the position for all of the particles in the second slice is also offset with sliceOffset.
9. **FR-10**: It should read the world position of the Niagara system, and add it's position to the FinalPositions.
10. **FR-11**: **FR-6** no longer applies. NumOfSlices should always be 1. But it should have a int FrameOffset, with a default value of 0. This should be used when deciding which frame to read. If FrameOffset is 1 and currend frame is 1, frame 2 should be read from the data table.
11. **FR-12**: **FR-7** only partly applies: it should no longer have a ParticlePositionOffset property. And no second slide/slice.
12. **FR-13**: **FR-9** no longer applies.
13. **FR-14**: It should Read current frame, start frame and end frame from WaterController the same way as MeshArrayActor does (WaterController is a Blueprint)

## Acceptance Criteria
- Actor can be placed in levels and configured via the editor
- Data is read from a DataTable and written to a Niagara Data Channel
- CurrentFrame, StartFrame, EndFrame are read from WaterController blueprint
- FrameOffset can be used to offset which frame is read
- Particle positions are scaled by ParticlePositionScale
- Niagara system world position is added to particle positions

## Implementation Details

### Class Structure
**File**: [WaveDisplayMobile.h](../../Source/GoneSurfing/WaveDisplayMobile.h)
**File**: [WaveDisplayMobile.cpp](../../Source/GoneSurfing/WaveDisplayMobile.cpp)

### Key Properties

| Property | Type | Access | Default | Description |
|----------|------|--------|---------|-------------|
| `WhiteWaterPointsDataTable` | `UDataTable*` | EditAnywhere, BlueprintReadWrite | `nullptr` | DataTable containing wave point data |
| `WhiteWaterDataChannelAsset` | `UNiagaraDataChannelAsset*` | EditAnywhere, BlueprintReadWrite | `nullptr` | Niagara Data Channel to write to |
| `PositionParamName` | `FName` | EditAnywhere, BlueprintReadWrite | `Position` | Data Channel parameter name for position |
| `NormalParamName` | `FName` | EditAnywhere, BlueprintReadWrite | `Normal` | Data Channel parameter name for normal |
| `ScaleParamName` | `FName` | EditAnywhere, BlueprintReadWrite | `Scale` | Data Channel parameter name for scale |
| `ParticlePositionScale` | `FVector` | EditAnywhere, BlueprintReadWrite | `(1,1,1)` | Scale to apply to particle positions |
| `NiagaraSystemActor` | `TObjectPtr<AActor>` | EditAnywhere, BlueprintReadWrite | `nullptr` | Niagara system actor whose world position is added to particles |
| `WaterController` | `TObjectPtr<AActor>` | EditAnywhere, BlueprintReadWrite | `nullptr` | WaterController blueprint (reads CurrentFrame, StartFrame, EndFrame) |
| `FrameOffset` | `int32` | EditAnywhere, BlueprintReadWrite | `0` | Offset to add to current frame when reading data |
| `UpdateFrequency` | `int32` | EditAnywhere, BlueprintReadWrite | `1` | How often to update (1 = every frame) |

### Key Methods

#### UpdateNiagaraParameters() (Private)
- Reads CurrentFrame, StartFrame, EndFrame from WaterController
- Applies FrameOffset to determine target frame
- Wraps frame number if it exceeds bounds
- Fetches row from DataTable using "Frame_{number}" format
- Applies ParticlePositionScale to positions
- Adds NiagaraSystemActor world position to final positions
- Writes data to Data Channel

#### WriteDataToChannel() (Private)
- Creates a UNiagaraDataChannelWriter
- Writes Position, Normal, Scale for each particle

#### ReadIntPropertyFromWaterController() (Private)
- Helper to read int properties from WaterController blueprint
- Supports both int and float property types

### Lifecycle Behavior

#### Constructor
- Enables tick
- Initializes pointers to nullptr

#### BeginPlay()
- Configures search parameters for data channel writes

#### Tick(float DeltaTime)
- Calls UpdateNiagaraParameters()

## Notes for AI Agent
- Use c++
- Ensure proper UE logging categories are used

## Status
- [x] Specification written
- [x] Implementation complete
- [ ] Tests passing
- [ ] Code reviewed
- [ ] Merged to main

## Metadata
- **Created**: 2025-11-10
- **Author**: Development Team
- **Priority**: Medium
- **Estimated Effort**: Completed
- **Related Components**: Niagara, DataTables, Data Channels
