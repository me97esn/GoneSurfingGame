// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "NiagaraComponent.h"
#include "MeshArrayActor.generated.h"

class UNiagaraDataInterfaceActorMeshArray;

/**
 * Actor that automatically loads meshes from a configurable folder path,
 * sorts them by name, and passes them to a Niagara System using a mesh array data interface.
 * Selects the current mesh based on frame number extracted from mesh names.
 */
UCLASS(BlueprintType, Blueprintable)
class GONESURFING_API AMeshArrayActor : public AActor
{
	GENERATED_BODY()

public:
	AMeshArrayActor();

	/** Folder path to load meshes from (e.g. "/Game/Meshes/WaveBlocks") */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mesh Array")
	FString MeshFolderPath;

	/** Array of meshes loaded from the folder (sorted by name) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Mesh Array")
	TArray<TObjectPtr<UStaticMesh>> MeshArray;

	/** The Niagara component that will receive the mesh array */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Niagara")
	TObjectPtr<UNiagaraComponent> NiagaraComponent;

	/** Name of the data interface parameter in the Niagara system (default: "MeshArrayDI") */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Niagara")
	FName DataInterfaceParameterName;

	/** If true, automatically update the mesh array when folder path changes in the editor */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mesh Array")
	bool bAutoUpdateMeshArray;

	/** Reference to the WaterController blueprint actor */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mesh Array")
	TObjectPtr<AActor> WaterController;

	/** Offset to add to current frame when selecting mesh (e.g., if current frame is 1 and offset is 2, mesh with frame 3 will be selected) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mesh Array")
	int32 MeshIndexOffset;

	/** Name of the CurrentMeshIndex user parameter in the Niagara system (default: "CurrentFrameMeshIndex") */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Niagara")
	FName MeshIndexParameterName;

	/** The current mesh index being displayed (based on frame number from mesh name) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Mesh Array")
	int32 CurrentMeshIndex;

	//~ Begin AActor Interface
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaTime) override;
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif
	//~ End AActor Interface

	/** Load meshes from the configured folder path and sort them by name */
	UFUNCTION(BlueprintCallable, Category = "Mesh Array")
	void LoadMeshesFromFolder();

	/** Update the mesh array in the Niagara system */
	UFUNCTION(BlueprintCallable, Category = "Mesh Array")
	void UpdateNiagaraMeshArray();

	/** Update the mesh renderer's mesh list in the Niagara System asset (Editor only) */
	UFUNCTION(BlueprintCallable, Category = "Mesh Array", CallInEditor)
	void UpdateMeshRendererAsset();

	/** Get the number of meshes in the array */
	UFUNCTION(BlueprintPure, Category = "Mesh Array")
	int32 GetNumMeshes() const { return MeshArray.Num(); }

protected:
	/** Get the data interface from the Niagara component */
	UNiagaraDataInterfaceActorMeshArray* GetMeshArrayDataInterface() const;

	/** Extract frame number from mesh name (e.g., "Slice0886" -> 886) */
	int32 ExtractFrameNumberFromMeshName(const FString& MeshName) const;

	/** Whether the mesh array has changed and needs updating */
	bool bMeshArrayDirty;

	/** Map from frame number to mesh array index for quick lookup */
	TMap<int32, int32> FrameToMeshIndexMap;
};
