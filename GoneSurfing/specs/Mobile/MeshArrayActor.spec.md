# Spec: MeshArrayActor - Dynamic Mesh Loading for Niagara Systems

## Overview
This specification describes the MeshArrayActor class, an Unreal Engine 5 actor that automatically loads static meshes from a specified folder, sorts them by name, and provides them to a Niagara particle system via a custom mesh array data interface.

## Objective
Enable dynamic mesh assignment to Niagara systems by automatically discovering and loading meshes from a configurable folder path, eliminating the need for manual mesh assignment in the editor.

## Requirements

### Functional Requirements
1. **FR-1**: The class shall inherit from AActor and be exposed to Blueprints
2. **FR-2**: The class shall have a UPROPERTY MeshFolderPath (FString) to specify the folder containing static meshes
3. **FR-3**: The class shall have a UPROPERTY MeshArray (TArray<TObjectPtr<UStaticMesh>>) to store loaded meshes
4. **FR-4**: The class shall have a UPROPERTY NiagaraComponent (UNiagaraComponent) as the root component
5. **FR-5**: The class shall have a UPROPERTY DataInterfaceParameterName (FName) to specify the data interface parameter name
6. **FR-6**: The class shall have a UPROPERTY bAutoUpdateMeshArray (bool) to control automatic updates
7. **FR-7**: The class shall implement LoadMeshesFromFolder() to discover and load meshes from the specified folder
8. **FR-8**: The class shall sort loaded meshes alphabetically by asset name
9. **FR-9**: The class shall implement UpdateNiagaraMeshArray() to pass meshes to the Niagara data interface
10. **FR-10**: The class shall implement UpdateMeshRendererAsset() (editor-only) to update the Niagara system asset
11. **FR-11**: The class shall implement GetNumMeshes() to return the count of loaded meshes
12. **FR-12**: On BeginPlay, the class shall automatically load meshes if the array is empty and a path is configured
13. **FR-13**: The class shall automatically reload meshes when MeshFolderPath changes in the editor
14. **FR-14**: The class shall have a property: WaterController of type AActor (assigned to WaterController-Datatables-BP at runtime). This is a blueprint class with no C++ code.
15. **FR-15**: Each tick the class shall read the "Current Frame" property from the WaterController and set the Niagara user parameter "CurrentFrameMeshIndex" with the value of Current Frame.
15. **FR-16**: **FR-15** No longer applies. Instead: Each tick The class shall read the "Current Frame" property from the WaterController, and set the property CurrentFrameMeshIndex. It shall calculate current mesh index the following way: The meshes in the MeshFolderPath are named in the following way: Slice0886, Slice0887. The characters can vary, but the number is the frame number. The current mesh index should be chosen so that the current mesh is always the one with a name as the Current frame property in the water controller.
16. **FR-17**: The class shall no longer have the property NumMeshesToDisplay. It should always only display one mesh, the current mesh in **FR-16**.
17. **FR-18**: The class shall no longer have the property MeshIndexIncrement. But it should have a mesh index offset, making it possible to choose the mesh Slice0003 if current frame is 1 and MeshIndexOffset is set to 2.
19. **FR-19**: Start mesh index parameter name and NumMeshIndex parameter name is no longer relevant.

### Non-Functional Requirements
1. **NFR-1**: The implementation shall be simple and readable
2. **NFR-2**: The code shall follow UE5 coding standards
3. **NFR-3**: The class shall use the Asset Registry for efficient mesh discovery
4. **NFR-4**: The class shall provide detailed logging for debugging
5. **NFR-5**: Mesh discovery shall be non-recursive (only immediate folder, not subfolders)

## Acceptance Criteria
- Actor can be placed in levels and configured via the editor
- Meshes are automatically loaded from the specified folder path
- Meshes are sorted alphabetically by name
- Mesh array is passed to Niagara system at runtime
- Property changes in the editor trigger automatic updates when enabled
- Editor-only function can update Niagara system asset permanently

## Implementation Details

### Class Structure
**File**: [MeshArrayActor.h](../../Source/GoneSurfing/MeshArrayActor.h)
**File**: [MeshArrayActor.cpp](../../Source/GoneSurfing/MeshArrayActor.cpp)

### Key Properties

| Property | Type | Access | Default | Description |
|----------|------|--------|---------|-------------|
| `MeshFolderPath` | `FString` | EditAnywhere, BlueprintReadWrite | `/Game/Meshes` | Folder path to load meshes from |
| `MeshArray` | `TArray<TObjectPtr<UStaticMesh>>` | VisibleAnywhere, BlueprintReadOnly | Empty | Array of loaded meshes (sorted by name) |
| `NiagaraComponent` | `TObjectPtr<UNiagaraComponent>` | VisibleAnywhere, BlueprintReadOnly | Created in constructor | Niagara component for particle system |
| `DataInterfaceParameterName` | `FName` | EditAnywhere, BlueprintReadWrite | `MeshArrayDI` | Name of the data interface parameter |
| `bAutoUpdateMeshArray` | `bool` | EditAnywhere, BlueprintReadWrite | `true` | Auto-update on property changes |
| `bMeshArrayDirty` | `bool` | Internal | `false` | Flag for pending updates |
| `WaterController` | `TObjectPtr<AActor>` | EditAnywhere, BlueprintReadWrite | `nullptr` | Reference to the WaterController blueprint actor |
| `MeshIndexOffset` | `int32` | EditAnywhere, BlueprintReadWrite | `0` | Offset to add to current frame when selecting mesh |
| `MeshIndexParameterName` | `FName` | EditAnywhere, BlueprintReadWrite | `CurrentFrameMeshIndex` | Name of the Niagara user parameter for the current mesh index |
| `CurrentMeshIndex` | `int32` | VisibleAnywhere, BlueprintReadOnly | `0` | The current mesh index being displayed |
| `FrameToMeshIndexMap` | `TMap<int32, int32>` | Protected | Empty | Map from frame number to mesh array index for quick lookup |

### Key Methods

#### LoadMeshesFromFolder()
- **Category**: Mesh Array
- **Blueprint Callable**: Yes
- **Behavior**:
  - Clears existing mesh array
  - Uses Asset Registry to discover static meshes in the specified folder
  - Sorts meshes alphabetically by asset name
  - Non-recursive folder search
  - Sets dirty flag for update
  - Logs loaded mesh count and names

#### UpdateNiagaraMeshArray()
- **Category**: Mesh Array
- **Blueprint Callable**: Yes
- **Behavior**:
  - Converts TArray<TObjectPtr<UStaticMesh>> to TArray<UStaticMesh*>
  - Retrieves mesh array data interface from Niagara component
  - Calls UpdateMeshArray() on the data interface
  - Clears dirty flag
  - Logs update status

#### UpdateMeshRendererAsset()
- **Category**: Mesh Array
- **Blueprint Callable**: Yes (CallInEditor)
- **Editor Only**: Yes
- **Behavior**:
  - Iterates through all emitters in the Niagara system
  - Finds mesh renderer properties
  - Updates renderer's mesh array with loaded meshes
  - Sets default mesh properties (scale 1.0, zero rotation/offset)
  - Marks Niagara system asset as modified and dirty
  - Only functional in editor builds

#### GetNumMeshes()
- **Category**: Mesh Array
- **Blueprint Pure**: Yes
- **Returns**: `int32` - Number of loaded meshes

#### GetMeshArrayDataInterface() (Protected)
- **Returns**: `UNiagaraDataInterfaceActorMeshArray*`
- **Behavior**: Retrieves the data interface from Niagara component's override parameters

#### ExtractFrameNumberFromMeshName() (Protected)
- **Returns**: `int32` - The frame number extracted from the mesh name, or -1 if not found
- **Behavior**: Extracts the trailing number from mesh names like "Slice0886" -> 886

### Lifecycle Behavior

#### Constructor
- Creates NiagaraComponent as root component
- Sets DataInterfaceParameterName to "MeshArrayDI"
- Enables bAutoUpdateMeshArray
- Enables tick
- Sets default MeshFolderPath to "/Game/Meshes"

#### BeginPlay()
- Loads meshes if array is empty and path is configured
- Builds FrameToMeshIndexMap from existing meshes if not already built
- Initializes mesh array in Niagara system

#### Tick(float DeltaTime)
- Reads the "Current Frame" property from the WaterController (supports int and float types)
- Applies MeshIndexOffset to the current frame value
- Looks up the mesh index from FrameToMeshIndexMap using the target frame number
- Sets the Niagara user parameter (default: "CurrentFrameMeshIndex") with the resolved mesh index
- Logs warnings if WaterController, CurrentFrame property, or frame mapping is not found

#### PostEditChangeProperty() (Editor Only)
- Monitors MeshFolderPath changes � Triggers LoadMeshesFromFolder()
- Monitors MeshFolderPath or DataInterfaceParameterName changes � Updates mesh array (or sets dirty flag)

### Dependencies

**Required Modules**:
- CoreMinimal
- Engine
- Niagara
- AssetRegistry

**Required Classes**:
- `UNiagaraDataInterfaceActorMeshArray` - Custom data interface for mesh arrays
- `UNiagaraComponent` - Niagara particle system component
- `UNiagaraMeshRendererProperties` - Mesh renderer configuration
- `UNiagaraSystem` - Niagara system asset
- `UStaticMesh` - Static mesh asset type

### Suggested Approach
1. Place AMeshArrayActor in the level
2. Assign a Niagara System to the NiagaraComponent
3. Ensure the Niagara System has a UNiagaraDataInterfaceActorMeshArray with the correct parameter name
4. Set MeshFolderPath to the desired mesh folder (e.g., "/Game/Meshes/WaveBlocks")
5. Meshes are automatically loaded and passed to the Niagara system
6. Optionally use UpdateMeshRendererAsset() in editor to permanently update the Niagara asset

## Notes for AI Agent
- Uses C++
- Requires UNiagaraDataInterfaceActorMeshArray custom data interface
- Mesh discovery is non-recursive (only immediate folder)
- Meshes are sorted alphabetically by asset name
- UpdateMeshRendererAsset() modifies the Niagara system asset permanently (editor only)
- Auto-update can be disabled for manual control via bAutoUpdateMeshArray
- Uses proper UE5 logging (LogTemp category)

## Status
- [x] Specification written
- [x] Implementation complete
- [ ] Tests passing
- [ ] Code reviewed
- [ ] Merged to main

## Implementation Notes

### Initial Implementation:
- Created [MeshArrayActor.h](../../Source/GoneSurfing/MeshArrayActor.h)
- Created [MeshArrayActor.cpp](../../Source/GoneSurfing/MeshArrayActor.cpp)
- Implemented automatic mesh loading from folder paths
- Integrated with Niagara via custom data interface

### Key Features:
1. **Automatic Mesh Discovery**: Uses Asset Registry to find all static meshes in a folder
2. **Alphabetical Sorting**: Meshes are sorted by asset name for predictable ordering
3. **Dynamic Updates**: Supports runtime and editor-time updates
4. **Niagara Integration**: Passes meshes to Niagara system via UNiagaraDataInterfaceActorMeshArray
5. **Editor-Friendly**: Auto-updates on property changes, with manual control option
6. **Asset Modification**: Can permanently update Niagara system assets in the editor

### How to Use in Editor:
1. Create a Niagara System with a mesh renderer
2. Add a UNiagaraDataInterfaceActorMeshArray data interface to the system (default name: "MeshArrayDI")
3. Add AMeshArrayActor to your level
4. Assign the Niagara System to the actor's NiagaraComponent
5. Set MeshFolderPath to your mesh folder (e.g., "/Game/Meshes/WaveBlocks")
6. Meshes are automatically loaded and sorted
7. The Niagara system receives the mesh array at runtime
8. Optionally call UpdateMeshRendererAsset() to permanently update the Niagara asset

### Usage Example:
```cpp
// In C++ or Blueprint
AMeshArrayActor* MeshActor = GetWorld()->SpawnActor<AMeshArrayActor>();
MeshActor->MeshFolderPath = TEXT("/Game/Meshes/WaveBlocks");
MeshActor->LoadMeshesFromFolder();
```

## Metadata
- **Created**: 2025-11-10
- **Author**: Development Team
- **Priority**: Medium
- **Estimated Effort**: Completed
- **Related Components**: Niagara, Asset Registry, Static Meshes
