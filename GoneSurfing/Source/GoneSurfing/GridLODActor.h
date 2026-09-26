// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StreamableManager.h"
#include "Engine/AssetManager.h"
#include "GridLODActor.generated.h"

/**
 * Preload request for selective mesh loading
 */
USTRUCT()
struct FLODPreloadRequest
{
	GENERATED_BODY()

	int32 GridX;
	int32 GridY;
	bool bNeedHighLOD;
	bool bNeedLowLOD;

	FLODPreloadRequest()
		: GridX(0), GridY(0), bNeedHighLOD(false), bNeedLowLOD(false)
	{
	}
};

/**
 * Grid cell component that tracks its position in the grid
 */
UCLASS()
class GONESURFING_API UGridCellComponent : public UStaticMeshComponent
{
	GENERATED_BODY()

public:
	/** Grid coordinates (X: 0-1, Y: 0 for a 2x1 grid) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grid")
	int32 GridX;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grid")
	int32 GridY;

	/** Current LOD level being displayed */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grid")
	int32 CurrentLODLevel;

	/** Current frame number displayed */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grid")
	int32 CurrentFrame;

	UGridCellComponent()
		: GridX(0)
		, GridY(0)
		, CurrentLODLevel(-1)
		, CurrentFrame(-1)
	{
	}
};

/**
 * Actor that manages a 2x1 grid of meshes with distance-based LOD switching.
 * Chunk 0: Combined left+right edges (always low-res, no wave detail needed)
 * Chunk 1: Center chunk with breaking wave (distance-based LOD switching)
 * Each grid cell displays a unique mesh per frame, with LOD selection based on camera distance.
 */
UCLASS(BlueprintType, Blueprintable)
class GONESURFING_API AGridLODActor : public AActor
{
	GENERATED_BODY()

public:
	AGridLODActor();

	//~ Begin AActor Interface
	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaTime) override;
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
	virtual bool ShouldTickIfViewportsOnly() const override { return true; }
#endif
	//~ End AActor Interface

	// ========== Grid Configuration ==========

	/** Number of grid cells in X direction */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid Configuration")
	int32 GridWidth;

	/** Number of grid cells in Y direction */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid Configuration")
	int32 GridHeight;

	/** Physical size of the entire grid in world units (meters) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid Configuration")
	FVector2D GridSize;

	// ========== Animation Configuration ==========

	/** Reference to the WaterController blueprint actor that provides current frame */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Animation")
	TObjectPtr<AActor> WaterController;

	/** Offset to add to current frame when selecting mesh (for temporal variation between tiles) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Animation")
	int32 FrameOffset;

	/** Start frame of the animation sequence (for looping with frame offset) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Animation")
	int32 StartFrame;

	/** End frame of the animation sequence (for looping with frame offset) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Animation")
	int32 EndFrame;

	// ========== Infinite Tiling Support ==========

	/** Logical world position offset for infinite tiling (shifts which meshes are displayed when actor wraps around) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Infinite Tiling")
	FVector2D WorldPositionOffset;

	// ========== LOD Configuration ==========

	/** Base folder path for meshes (e.g., "/Game/Meshes/GridMeshes") */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD")
	FString BaseMeshFolderPath;

	/** Name of the high-resolution folder (e.g., "high_res") */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD")
	FString HighResFolderName;

	/** Name of the low-resolution folder (e.g., "low_res") */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD")
	FString LowResFolderName;

	/** Material to apply to all grid meshes (optional) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD")
	TObjectPtr<UMaterialInterface> GridMaterial;

	/** Frame to display in editor when not playing */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD")
	int32 EditorPreviewFrame;

	/** LOD level to use in editor preview (0=high, 1=low) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD", meta = (ClampMin = "0", ClampMax = "1"))
	int32 EditorPreviewLOD;

	/** Distance threshold for switching between high and low LOD (meters) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD", meta = (ClampMin = "0.0"))
	float LODDistanceThreshold;

	/** Hysteresis offset to prevent rapid LOD switching (meters). When upgrading to high-LOD, camera must be (Threshold - Hysteresis) close. When downgrading to low-LOD, camera must be (Threshold + Hysteresis) far. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD", meta = (ClampMin = "0.0"))
	float LODHysteresis;

	/** Maximum distance to display grid cells (beyond this, cells are hidden) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD", meta = (ClampMin = "0.0"))
	float MaxVisibleDistance;

	/** Use 2D distance calculation (ignoring Z axis) for flat grids */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD")
	bool bUse2DDistance;

	/** Use distance squared comparisons to avoid sqrt (slight performance optimization) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD")
	bool bUseDistanceSquared;

	/** Force specific grid cells to always use low-LOD (for non-contiguous combined meshes like shore+horizon). Comma-separated GridX indices, e.g., "0,2" */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD")
	FString ForceLowLODCells;

	// ========== Streaming/Performance (Phase 2) ==========

	/** Enable mesh preloading and caching to avoid runtime loading hitches */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Streaming")
	bool bEnableMeshStreaming;

	/** Use selective preloading (only load meshes cells will likely use based on current LOD state) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Streaming")
	bool bUseSelectivePreloading;

	/** Include 8-connected neighbors vs 4-connected (affects preload border size) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Streaming")
	bool bUse8ConnectedNeighbors;

	/** Extra border cells to include in preload (0 = adjacent only, 1 = 2-cell border, etc.) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Streaming", meta = (ClampMin = "0", ClampMax = "3"))
	int32 PreloadBorderSize;

	/** Number of frames to preload ahead of current frame */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Streaming", meta = (ClampMin = "0", ClampMax = "10"))
	int32 PreloadFrameCount;

	/** Number of old frames to keep cached (frames older than this are unloaded) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Streaming", meta = (ClampMin = "0", ClampMax = "10"))
	int32 CacheFrameCount;

	/**
	 * Preload upcoming-frame meshes asynchronously (on the async loading thread) instead of
	 * blocking the game thread with StaticLoadObject. Eliminates the per-frame-advance hitches.
	 * When a mesh isn't cached yet, the cell keeps its previous mesh for a frame or two until the
	 * async load lands, rather than stalling. Disable to fall back to the old synchronous path.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Streaming")
	bool bUseAsyncPreload = true;

	// ========== Seam Blending ==========

	/** Enable automatic seam blending via Material Parameter Collection */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Seam Blending")
	bool bEnableSeamBlending;

	/** Reference to WaveHeight actor for seam calculations */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Seam Blending")
	TObjectPtr<AActor> WaveHeightActor;

	/** Material Parameter Collection to update with seam data (create MPC_WaveSeamBlending asset) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Seam Blending")
	TObjectPtr<class UMaterialParameterCollection> SeamBlendingMPC;

	// ========== Debug/Info ==========

	/** Optional actor to use as camera position for LOD calculations (useful for debugging in Simulate mode) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	TObjectPtr<AActor> DebugCameraActor;

	/** Draw debug visualization of grid cells and LOD states */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	bool bShowDebugVisualization;

	/** Current frame being displayed (read from WaterController) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Debug")
	int32 CurrentFrame;

	/** Number of cells currently in high LOD */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Debug")
	int32 NumHighLODCells;

	/** Number of cells currently in low LOD */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Debug")
	int32 NumLowLODCells;

	/** Number of cells currently hidden/culled */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Debug")
	int32 NumCulledCells;

	/** Enable verbose logging for initialization and mesh loading */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug|Logging")
	bool bEnableInitLogging = false;

	/** Enable verbose logging for LOD changes */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug|Logging")
	bool bEnableLODLogging = false;

	// ========== Public Functions ==========

	/** Initialize or recreate the grid of mesh components */
	UFUNCTION(BlueprintCallable, Category = "Grid", CallInEditor)
	void InitializeGrid();

	/** Preload meshes for current and upcoming frames (Phase 2) */
	UFUNCTION(BlueprintCallable, Category = "Grid")
	void PreloadMeshesForFrameRange(int32 PreloadStartFrame, int32 PreloadEndFrame);

	/** Synchronously preload meshes using StreamableManager (blocks until loaded) */
	UFUNCTION(BlueprintCallable, Category = "Grid")
	void PreloadMeshesSynchronous(int32 FirstFrame, int32 FrameCount);

	/** Update all grid cells with meshes for the current frame */
	UFUNCTION(BlueprintCallable, Category = "Grid")
	void UpdateGridMeshes();

	/** Preload meshes around THIS tile's own current frame, then swap the cells immediately.
	 *
	 *  Every GridLODActor renders a DIFFERENT phase of the wave: GetCurrentFrameFromController()
	 *  adds this tile's FrameOffset (temporal variation between tiles) and wraps it into
	 *  StartFrame..EndFrame. So a caller that jumps the wave clock cannot preload one shared frame
	 *  across all tiles — every tile but the zero-offset one would be left without meshes for the
	 *  frame it actually needs, and would stream them in over the following frames, which reads as
	 *  the wave visibly snapping shortly after level start.
	 *
	 *  The offset lives here, so the preload does too. See specs/deterministic-ride-handoff.md. */
	UFUNCTION(BlueprintCallable, Category = "Grid")
	void PreloadAroundCurrentFrame(int32 FrameCount = 20);

	/** Get the grid cell component at the specified grid coordinates */
	UFUNCTION(BlueprintPure, Category = "Grid")
	UGridCellComponent* GetGridCell(int32 X, int32 Y) const;

	/** Reposition this actor for infinite tiling - updates location, world position offset, and frame offset */
	UFUNCTION(BlueprintCallable, Category = "Infinite Tiling")
	void RepositionToWorldLocation(FVector NewLocation, FVector2D NewWorldPositionOffset, int32 NewFrameOffset);

	/** Update seam blend parameters on all grid cell materials */
	UFUNCTION(BlueprintCallable, Category = "Seam Blending")
	void UpdateSeamBlendParameters();

protected:
	/** Array of grid cell components (size = GridWidth * GridHeight) */
	UPROPERTY()
	TArray<TObjectPtr<UGridCellComponent>> GridCells;

	/** Mesh cache: maps mesh path → loaded mesh (Phase 2 streaming) */
	UPROPERTY()
	TMap<FString, TObjectPtr<UStaticMesh>> MeshCache;

	/** Track which frames are currently loaded in cache */
	TSet<int32> CachedFrames;

	/** Mesh cache keys (BuildMeshPath strings) that have an async load in flight (dedupes requests) */
	TSet<FString> InFlightMeshPaths;

	/** Active async load handles kept alive while streaming; pruned when complete */
	TArray<TSharedPtr<struct FStreamableHandle>> ActivePreloadHandles;

	/** Streamable manager for async asset loading */
	FStreamableManager StreamableManager;

	/** Last frame that triggered mesh loading (for change detection) */
	int32 LastStreamedFrame;

	/** Track initial loading progress (cells loaded during BeginPlay warmup) */
	int32 InitialLoadingProgress;

	/** Whether initial warmup loading is complete */
	bool bInitialWarmupComplete;

	/** Get the current frame number from the WaterController */
	int32 GetCurrentFrameFromController() const;

	/** Calculate the world position of a grid cell center */
	FVector GetGridCellCenterPosition(int32 X, int32 Y) const;

	/** Determine which LOD level to use for a given distance with hysteresis */
	int32 GetLODLevelForDistance(float Distance, int32 CurrentLOD) const;

	/** Get cells that need preloading based on current LOD state (selective preloading) */
	TArray<FLODPreloadRequest> GetCellsNeedingPreload() const;

	/** Load a mesh for a specific grid cell, frame, and LOD level */
	UStaticMesh* LoadGridMesh(int32 X, int32 Y, int32 Frame, int32 LODLevel) const;

	/** Build the full path to a mesh asset */
	FString BuildMeshPath(int32 X, int32 Y, int32 Frame, int32 LODLevel) const;

	/** Convert a package path (from BuildMeshPath) to a full FSoftObjectPath (Package.AssetName) */
	FSoftObjectPath MeshPackagePathToObjectPath(const FString& PackagePath) const;

	/** Kick a single async load for a mesh if it isn't already cached or in flight. Returns true if it is (or becomes) cached now. */
	bool RequestMeshLoadAsync(int32 X, int32 Y, int32 Frame, int32 LODLevel);

	/** Issue one batched async load for the given cache keys, marking the given frames cached on completion */
	void IssueAsyncMeshLoads(const TArray<FString>& CacheKeys, const TArray<int32>& FramesToMark);

	/** Completion callback for an async preload batch: resolves loaded meshes into the cache */
	void OnAsyncMeshLoadComplete(TArray<FString> CacheKeys, TArray<int32> FramesToMark);

	/** Round frame to nearest available frame for given LOD level (respects StartFrame/EndFrame bounds) */
	int32 RoundFrameForLOD(int32 Frame, int32 LODLevel) const;

	/** Update a single grid cell with the appropriate mesh based on distance */
	void UpdateGridCell(UGridCellComponent* Cell, const FVector& CameraPosition, int32 Frame);

	/** Unload meshes from frames that are too old (Phase 2) */
	void UnloadOldFrames(int32 ActiveFrame);

	/** Clear all grid cells */
	void ClearGrid();

	// ---- Visual verification harness (gated by surf.grid.shots; see .cpp) ----
	/** Take screenshots at configured wave frames for sync/async A/B comparison, then quit. */
	void MaybeCaptureVerificationShots(int32 Frame);

	bool bVerifyShotsInit = false;
	TArray<int32> VerifyTargetFrames;
	TSet<int32> VerifyDoneFrames;
	int32 VerifyQuitCountdown = -1;
};
