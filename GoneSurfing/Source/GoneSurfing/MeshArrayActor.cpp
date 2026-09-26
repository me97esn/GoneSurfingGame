// Fill out your copyright notice in the Description page of Project Settings.

#include "MeshArrayActor.h"
#include "SurfLog.h"
#include "NiagaraDataInterfaceActorMeshArray.h"
#include "NiagaraComponent.h"
#include "NiagaraMeshRendererProperties.h"
#include "NiagaraSystem.h"
#include "NiagaraEmitter.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/StaticMesh.h"

AMeshArrayActor::AMeshArrayActor()
	: DataInterfaceParameterName(TEXT("MeshArrayDI"))
	, bAutoUpdateMeshArray(true)
	, WaterController(nullptr)
	, MeshIndexOffset(0)
	, MeshIndexParameterName(TEXT("CurrentFrameMeshIndex"))
	, CurrentMeshIndex(0)
{
	PrimaryActorTick.bCanEverTick = true;

	// Create the Niagara component
	NiagaraComponent = CreateDefaultSubobject<UNiagaraComponent>(TEXT("NiagaraComponent"));
	RootComponent = NiagaraComponent;

	// Set default folder path
	MeshFolderPath = TEXT("/Game/Meshes");
}

void AMeshArrayActor::BeginPlay()
{
	Super::BeginPlay();

	// Load meshes if the array is empty
	if (MeshArray.Num() == 0 && !MeshFolderPath.IsEmpty())
	{
		LoadMeshesFromFolder();
	}
	else if (MeshArray.Num() > 0 && FrameToMeshIndexMap.Num() == 0)
	{
		// Meshes were already loaded (e.g., in editor), but the map wasn't built
		// Build the frame-to-index map from existing meshes
		for (int32 i = 0; i < MeshArray.Num(); ++i)
		{
			if (MeshArray[i])
			{
				int32 FrameNumber = ExtractFrameNumberFromMeshName(MeshArray[i]->GetName());
				if (FrameNumber >= 0)
				{
					FrameToMeshIndexMap.Add(FrameNumber, i);
					UE_LOG(LogSurf, Log, TEXT("MeshArrayActor: Mapped existing mesh '%s' (Frame %d -> Index %d)"),
						*MeshArray[i]->GetName(), FrameNumber, i);
				}
			}
		}
		UE_LOG(LogSurf, Log, TEXT("MeshArrayActor: Built frame map for %d existing meshes"), FrameToMeshIndexMap.Num());
	}

	// Initialize the mesh array on begin play
	UpdateNiagaraMeshArray();
}

void AMeshArrayActor::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	// Update mesh selection from WaterController's Current Frame property
	if (WaterController && NiagaraComponent && MeshArray.Num() > 0)
	{
		// Get the "Current Frame" property from the WaterController
		if (UClass* WaterControllerClass = WaterController->GetClass())
		{
			// Try multiple possible property name variations
			FProperty* CurrentFrameProperty = WaterControllerClass->FindPropertyByName(TEXT("CurrentFrame"));
			if (!CurrentFrameProperty)
			{
				CurrentFrameProperty = WaterControllerClass->FindPropertyByName(TEXT("Current Frame"));
			}
			if (!CurrentFrameProperty)
			{
				CurrentFrameProperty = WaterControllerClass->FindPropertyByName(TEXT("current_frame"));
			}

			if (CurrentFrameProperty)
			{
				// Read the Current Frame value
				int32 CurrentFrameValue = 0;
				if (FIntProperty* IntProperty = CastField<FIntProperty>(CurrentFrameProperty))
				{
					CurrentFrameValue = IntProperty->GetPropertyValue_InContainer(WaterController.Get());
				}
				else if (FFloatProperty* FloatProperty = CastField<FFloatProperty>(CurrentFrameProperty))
				{
					// Handle float to int conversion if needed
					float CurrentFrameFloat = FloatProperty->GetPropertyValue_InContainer(WaterController.Get());
					CurrentFrameValue = FMath::RoundToInt(CurrentFrameFloat);
				}
				else
				{
					static bool bHasLoggedTypeWarning = false;
					if (!bHasLoggedTypeWarning)
					{
						UE_LOG(LogSurf, Warning, TEXT("MeshArrayActor: Found CurrentFrame property but it's not int or float type"));
						bHasLoggedTypeWarning = true;
					}
					return;
				}

				// Apply mesh index offset
				int32 TargetFrame = CurrentFrameValue + MeshIndexOffset;

				// Find the mesh index for this frame number using the map
				if (int32* MeshIndexPtr = FrameToMeshIndexMap.Find(TargetFrame))
				{
					CurrentMeshIndex = *MeshIndexPtr;
				}
				else
				{
					// Frame not found in map, log warning once
					static TSet<int32> LoggedMissingFrames;
					if (!LoggedMissingFrames.Contains(TargetFrame))
					{
						UE_LOG(LogSurf, Warning, TEXT("MeshArrayActor: No mesh found for frame %d (CurrentFrame=%d, Offset=%d)"),
							TargetFrame, CurrentFrameValue, MeshIndexOffset);
						LoggedMissingFrames.Add(TargetFrame);
					}
					return;
				}

				// Set the Niagara user parameter to select the current mesh
				NiagaraComponent->SetVariableInt(MeshIndexParameterName, CurrentMeshIndex);

				// Debug logging to verify mesh index is being set (log on frame change)
				static int32 LastLoggedFrame = -1;
				if (CurrentFrameValue != LastLoggedFrame)
				{
					UE_LOG(LogSurf, Log, TEXT("MeshArrayActor: CurrentFrame=%d, TargetFrame=%d, MeshIndex=%d"),
						CurrentFrameValue, TargetFrame, CurrentMeshIndex);
					LastLoggedFrame = CurrentFrameValue;
				}
			}
			else
			{
				// Only log this warning once to avoid spam
				static bool bHasLoggedWarning = false;
				if (!bHasLoggedWarning)
				{
					UE_LOG(LogSurf, Warning, TEXT("MeshArrayActor: Could not find 'CurrentFrame' or 'Current Frame' property in WaterController class '%s'"), *WaterControllerClass->GetName());

					// List all available properties for debugging
					UE_LOG(LogSurf, Warning, TEXT("MeshArrayActor: Available properties in WaterController:"));
					for (TFieldIterator<FProperty> PropIt(WaterControllerClass); PropIt; ++PropIt)
					{
						FProperty* Property = *PropIt;
						UE_LOG(LogSurf, Warning, TEXT("  - %s (type: %s)"), *Property->GetName(), *Property->GetClass()->GetName());
					}
					bHasLoggedWarning = true;
				}
			}
		}
	}
}

#if WITH_EDITOR
void AMeshArrayActor::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	if (PropertyChangedEvent.Property)
	{
		const FName PropertyName = PropertyChangedEvent.Property->GetFName();

		// Reload meshes if the folder path changes
		if (PropertyName == GET_MEMBER_NAME_CHECKED(AMeshArrayActor, MeshFolderPath))
		{
			LoadMeshesFromFolder();
		}

		// Update mesh array if relevant properties change
		if (PropertyName == GET_MEMBER_NAME_CHECKED(AMeshArrayActor, MeshFolderPath) ||
			PropertyName == GET_MEMBER_NAME_CHECKED(AMeshArrayActor, DataInterfaceParameterName))
		{
			if (bAutoUpdateMeshArray)
			{
				UpdateNiagaraMeshArray();
			}
			else
			{
				bMeshArrayDirty = true;
			}
		}
	}
}
#endif

void AMeshArrayActor::LoadMeshesFromFolder()
{
	MeshArray.Empty();
	FrameToMeshIndexMap.Empty();

	if (MeshFolderPath.IsEmpty())
	{
		UE_LOG(LogSurf, Warning, TEXT("MeshArrayActor: MeshFolderPath is empty"));
		return;
	}

	// Get the Asset Registry module
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	// Create a filter to find static meshes in the specified folder
	FARFilter Filter;
	Filter.PackagePaths.Add(FName(*MeshFolderPath));
	Filter.ClassPaths.Add(UStaticMesh::StaticClass()->GetClassPathName());
	Filter.bRecursivePaths = false; // Set to true if you want to search subfolders

	// Get all assets matching the filter
	TArray<FAssetData> AssetDataList;
	AssetRegistry.GetAssets(Filter, AssetDataList);

	// Sort assets by name
	AssetDataList.Sort([](const FAssetData& A, const FAssetData& B) {
		return A.AssetName.ToString() < B.AssetName.ToString();
	});

	// Load and add each mesh to the array, building the frame-to-index map
	for (const FAssetData& AssetData : AssetDataList)
	{
		UStaticMesh* Mesh = Cast<UStaticMesh>(AssetData.GetAsset());
		if (Mesh)
		{
			int32 MeshIndex = MeshArray.Num();
			MeshArray.Add(Mesh);

			// Extract frame number from mesh name and add to map
			int32 FrameNumber = ExtractFrameNumberFromMeshName(Mesh->GetName());
			if (FrameNumber >= 0)
			{
				FrameToMeshIndexMap.Add(FrameNumber, MeshIndex);
				UE_LOG(LogSurf, Log, TEXT("MeshArrayActor: Loaded mesh '%s' (Frame %d -> Index %d)"), *Mesh->GetName(), FrameNumber, MeshIndex);
			}
			else
			{
				UE_LOG(LogSurf, Warning, TEXT("MeshArrayActor: Could not extract frame number from mesh '%s'"), *Mesh->GetName());
			}
		}
	}

	UE_LOG(LogSurf, Log, TEXT("MeshArrayActor: Loaded %d meshes from '%s', mapped %d frame numbers"), MeshArray.Num(), *MeshFolderPath, FrameToMeshIndexMap.Num());

	// Mark as dirty to trigger update
	bMeshArrayDirty = true;
}

void AMeshArrayActor::UpdateNiagaraMeshArray()
{
	if (!NiagaraComponent || !NiagaraComponent->GetAsset())
	{
		return;
	}

	// Convert TArray<TObjectPtr<UStaticMesh>> to TArray<UStaticMesh*>
	TArray<UStaticMesh*> RawMeshArray;
	RawMeshArray.Reserve(MeshArray.Num());
	for (const TObjectPtr<UStaticMesh>& Mesh : MeshArray)
	{
		RawMeshArray.Add(Mesh.Get());
	}

	// Update the data interface with the mesh array
	UNiagaraDataInterfaceActorMeshArray* MeshArrayDI = GetMeshArrayDataInterface();
	if (MeshArrayDI)
	{
		// Update the data interface with the new mesh array
		MeshArrayDI->UpdateMeshArray(RawMeshArray);
		UE_LOG(LogSurf, Log, TEXT("MeshArrayActor: Updated data interface with %d meshes"), RawMeshArray.Num());
	}
	else
	{
		UE_LOG(LogSurf, Warning, TEXT("MeshArrayActor: Could not find mesh array data interface with name '%s'"), *DataInterfaceParameterName.ToString());
	}

	bMeshArrayDirty = false;
}

void AMeshArrayActor::UpdateMeshRendererAsset()
{
#if WITH_EDITOR
	if (!NiagaraComponent || !NiagaraComponent->GetAsset())
	{
		UE_LOG(LogSurf, Warning, TEXT("MeshArrayActor: No Niagara System assigned"));
		return;
	}

	// Convert TArray<TObjectPtr<UStaticMesh>> to TArray<UStaticMesh*>
	TArray<UStaticMesh*> RawMeshArray;
	RawMeshArray.Reserve(MeshArray.Num());
	for (const TObjectPtr<UStaticMesh>& Mesh : MeshArray)
	{
		RawMeshArray.Add(Mesh.Get());
	}

	// Get the Niagara System asset
	UNiagaraSystem* NiagaraSystem = NiagaraComponent->GetAsset();
	if (!NiagaraSystem)
	{
		return;
	}

	// Mark the asset as modified so it can be saved
	NiagaraSystem->Modify();

	bool bFoundMeshRenderer = false;

	// Iterate through all emitters in the system
	const TArray<FNiagaraEmitterHandle>& EmitterHandles = NiagaraSystem->GetEmitterHandles();
	for (const FNiagaraEmitterHandle& EmitterHandle : EmitterHandles)
	{
		if (FVersionedNiagaraEmitterData* EmitterData = EmitterHandle.GetEmitterData())
		{
			// Find mesh renderer properties
			for (UNiagaraRendererProperties* Renderer : EmitterData->GetRenderers())
			{
				if (UNiagaraMeshRendererProperties* MeshRenderer = Cast<UNiagaraMeshRendererProperties>(Renderer))
				{
					bFoundMeshRenderer = true;

					// Mark the renderer as modified
					MeshRenderer->Modify();

					// Update the mesh renderer's mesh array
					MeshRenderer->Meshes.Empty();
					for (UStaticMesh* Mesh : RawMeshArray)
					{
						if (Mesh)
						{
							FNiagaraMeshRendererMeshProperties MeshProp;
							MeshProp.Mesh = Mesh;
							MeshProp.Scale = FVector(1.0f, 1.0f, 1.0f);
							MeshProp.Rotation = FRotator::ZeroRotator;
							MeshProp.PivotOffset = FVector::ZeroVector;
							MeshProp.PivotOffsetSpace = ENiagaraMeshPivotOffsetSpace::Mesh;
							MeshRenderer->Meshes.Add(MeshProp);
						}
					}

					UE_LOG(LogSurf, Log, TEXT("MeshArrayActor: Updated mesh renderer asset with %d meshes"), MeshRenderer->Meshes.Num());
				}
			}
		}
	}

	if (!bFoundMeshRenderer)
	{
		UE_LOG(LogSurf, Warning, TEXT("MeshArrayActor: No mesh renderer found in Niagara System"));
	}
	else
	{
		// Mark the package as dirty so it can be saved
		NiagaraSystem->MarkPackageDirty();
	}
#else
	UE_LOG(LogSurf, Warning, TEXT("MeshArrayActor: UpdateMeshRendererAsset can only be called in the editor"));
#endif
}

UNiagaraDataInterfaceActorMeshArray* AMeshArrayActor::GetMeshArrayDataInterface() const
{
	if (!NiagaraComponent)
	{
		return nullptr;
	}

	// Create a Niagara variable with the parameter name and the data interface type
	FNiagaraVariable DIVariable(FNiagaraTypeDefinition(UNiagaraDataInterfaceActorMeshArray::StaticClass()), DataInterfaceParameterName);

	// Try to get the data interface from the override parameters
	UNiagaraDataInterfaceActorMeshArray* MeshArrayDI = Cast<UNiagaraDataInterfaceActorMeshArray>(
		NiagaraComponent->GetOverrideParameters().GetDataInterface(DIVariable)
	);

	return MeshArrayDI;
}

int32 AMeshArrayActor::ExtractFrameNumberFromMeshName(const FString& MeshName) const
{
	// Extract the trailing number from mesh names like "Slice0886", "Wave0123", etc.
	// Find the last sequence of digits in the string
	int32 NumberStart = -1;
	int32 NumberEnd = MeshName.Len();

	// Scan from the end to find digits
	for (int32 i = MeshName.Len() - 1; i >= 0; --i)
	{
		if (FChar::IsDigit(MeshName[i]))
		{
			if (NumberStart < 0)
			{
				NumberEnd = i + 1;
			}
			NumberStart = i;
		}
		else if (NumberStart >= 0)
		{
			// We found a non-digit after finding digits, so we have our number
			break;
		}
	}

	if (NumberStart >= 0)
	{
		FString NumberStr = MeshName.Mid(NumberStart, NumberEnd - NumberStart);
		return FCString::Atoi(*NumberStr);
	}

	return -1; // No number found
}
