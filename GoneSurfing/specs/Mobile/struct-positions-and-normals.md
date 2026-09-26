# Spec: struct positions and normals
## Overview
A struct for storing all vertex positions and normals, for displaying purposes.

## Objective
Create a struct for displaying water surface as particles in a Niagara system.

## Requirements

### Functional Requirements
1. **FR-1**: Create a USTRUCT similar to the FWaveFrequenciesDataStruct in WaveHeight. It should have two FVector arrays, one for positions and one for normals
2. **FR-2**: Also generate an example json file for importing into a datatable for this struct.

### Non-Functional Requirements
1. **NFR-1**: The implementation shall be simple and readable
2. **NFR-2**: The code shall follow project coding standards

## Acceptance Criteria

## Implementation Details

### Suggested Approach

## Notes for AI Agent
- Use c++
- Ensure proper UE logging categories are used

## Status
- [x] Specification written
- [x] Implementation complete
- [ ] Tests passing
- [ ] Code reviewed
- [ ] Merged to main

## Implementation Notes

### Implementation (2025-10-15):
- Created `FWavePointsDataStruct` in [WaveHeight.h](../../Source/GoneSurfing/WaveHeight.h:110-120)
- Added two `TArray<FVector>` properties: `Positions` and `Normals`
- Follows the same pattern as existing data structs (inherits from `FTableRowBase`, uses `GENERATED_BODY()`)
- Created example JSON file at [WavePointsData_Example.json](../../Content/Data/WavePointsData_Example.json)

### Key Features:
1. **FWavePointsDataStruct** - USTRUCT for storing wave surface points
   - `Positions` - TArray<FVector> for point world positions
   - `Normals` - TArray<FVector> for surface normals at each point
2. **Blueprint accessible** - Marked with `BlueprintType` and properties with `BlueprintReadWrite`
3. **Data Table compatible** - Inherits from `FTableRowBase` for use in Unreal Data Tables

### How to Use:
1. In the Unreal Editor, create a new Data Table
2. Choose `WavePointsDataStruct` as the row structure
3. Import the example JSON file or manually add rows
4. Each row can contain arrays of positions and corresponding normals
5. Use with the particle system or any other rendering system that needs point data