// Fill out your copyright notice in the Description page of Project Settings.

#pragma once
using namespace std;
#include <utility>
#include <tuple>
#include <vector>
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "WaveHeight.generated.h"



USTRUCT(BlueprintType)
struct FComplex : public FTableRowBase
{
	GENERATED_BODY()
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float re;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float im;

};


USTRUCT(BlueprintType)
struct FEncapsule : public FTableRowBase
{
	GENERATED_BODY()
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	TArray<FComplex> arr;
};

USTRUCT(BlueprintType)
struct FEncapsuleFloat : public FTableRowBase
{
	GENERATED_BODY()
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	TArray<float> arr;
};

USTRUCT(BlueprintType)
struct FEncapsuleInt: public FTableRowBase
{
	GENERATED_BODY()
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	TArray<int> arr;
};

USTRUCT(BlueprintType)
struct FWaveFrequenciesMetadataStruct : public FTableRowBase
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float step_size;
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	int number_of_frequencies_to_include;
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	int number_of_rows_to_include;
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	int len_x;
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	int len_y;
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	int start_trace_y;
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	int start_trace_x;
};

// Unified wave metadata - describes grid dimensions and tiling parameters
USTRUCT(BlueprintType)
struct FWaveUnifiedMetadata : public FTableRowBase
{
	GENERATED_BODY()
public:
	// Tiling dimensions for infinite repetition (matches GridLODActor tiling)
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float tiling_x = 0.0f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float tiling_y = 0.0f;

	// Grid sampling parameters
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float step_size = 0.0f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float grid_start_x = 0.0f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float grid_start_y = 0.0f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	int32 grid_width = 0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	int32 grid_height = 0;

	// Seam blending range for tiling
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float seam_min = 0.0f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float seam_max = 0.0f;

	// Frame range
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	int32 start_frame = 0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	int32 end_frame = 0;
};

USTRUCT(BlueprintType)
struct FWaveFrequenciesDataStruct : public FTableRowBase
{
    GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	TArray<FEncapsule> f;
};

// Unified wave frame data - contains height, normals, and velocity for one animation frame
USTRUCT(BlueprintType)
struct FWaveUnifiedFrameData : public FTableRowBase
{
    GENERATED_BODY()
public:
	// Height values (grid_width * grid_height floats)
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	TArray<float> h;

	// Normal X components
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	TArray<float> nx;

	// Normal Y components
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	TArray<float> ny;

	// Normal Z components
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	TArray<float> nz;

	// Velocity X components
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	TArray<float> vx;

	// Velocity Y components
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	TArray<float> vy;

	// Velocity Z components
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	TArray<float> vz;

	/** Custom on-disk layout: every array is stored as 16-bit fixed point (per-array min + scale)
	 *  instead of float, halving the cooked table. In memory the fields above stay float, so no
	 *  reader changes. Assets saved before this existed still load (tagged-property fallback).
	 *  See WaveDataQuantize.cpp. */
	bool Serialize(FArchive& Ar);
};

template<>
struct TStructOpsTypeTraits<FWaveUnifiedFrameData> : public TStructOpsTypeTraitsBase2<FWaveUnifiedFrameData>
{
	enum { WithSerializer = true };
};

// Keep old struct for backward compatibility (not used with unified format)
USTRUCT(BlueprintType)
struct FWaveSamplesDataStruct : public FTableRowBase
{
    GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	TArray<FEncapsuleInt> f;
};

USTRUCT(BlueprintType)
struct FWavePointsDataStruct2 : public FTableRowBase
{
    GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	TArray<FVector> Positions;

	/** Same 16-bit fixed-point on-disk layout as FWaveUnifiedFrameData: FVector is double precision,
	 *  so this takes each foam point from 24 bytes to 6. See WaveDataQuantize.cpp. */
	bool Serialize(FArchive& Ar);
};

template<>
struct TStructOpsTypeTraits<FWavePointsDataStruct2> : public TStructOpsTypeTraitsBase2<FWavePointsDataStruct2>
{
	enum { WithSerializer = true };
};


UCLASS()
class GONESURFING_API AWaveHeight : public AActor
{
	GENERATED_BODY()
	
public:	
	// Sets default values for this actor's properties
	AWaveHeight();

	// ========== Unified Wave Data ==========

	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(RowType="FWaveUnifiedMetadata"))
	class UDataTable* waveUnifiedMetadata;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(RowType="FWaveUnifiedFrameData"))
	class UDataTable* waveUnifiedData;

	/**
	 * Coordinate scale factor between Unreal and Blender/Data space (per-axis).
	 * - If 1.0: Unreal world coords match Blender 1:1
	 * - If 0.1: Blender coords are scaled down (typical with GridLODActor scale 0.1)
	 * - If 10.0: Blender coords are scaled up
	 * Use different values per axis to test for scale mismatches.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Unified Wave Data")
	FVector CoordinateScale = FVector(1.0f, 1.0f, 1.0f);

	/**
	 * Flip wave heights (multiply Z by -1).
	 * Enable this if waves appear upside down compared to the mesh.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Unified Wave Data")
	bool bFlipWaveHeight = false;

	/**
	 * Velocity scale factor (separate from CoordinateScale for wave height).
	 * Scales velocity from data space to world space.
	 *
	 * Not a UPROPERTY: the value is hardcoded in BeginPlay (see WaveHeight.cpp).
	 * Exposing it as editor-tunable while BeginPlay overwrites the umap value gave
	 * the misleading impression it was adjustable from the editor. Edit the
	 * hardcode in BeginPlay if you need a different scale.
	 */
	FVector VelocityScale = FVector(1.0f, 1.0f, 1.0f);

	/**
	 * Flip wave data along X-axis (mirror horizontally).
	 * Enable this if the wave pattern is backwards compared to the mesh.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Unified Wave Data")
	bool bFlipXAxis = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float waterVelocityCoefficient = 2000.0;

	// ========== Animation Configuration ==========

	/** Reference to the WaterController blueprint actor that provides current frame */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Animation")
	TObjectPtr<AActor> WaterController;

	/** Start frame of the animation sequence (for frame wrapping) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Animation")
	int32 StartFrame = 886;

	/** End frame of the animation sequence (for frame wrapping) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Animation")
	int32 EndFrame = 1078;

	// ========== Infinite Tiling Configuration ==========

	/**
	 * Take the tile step and frame-per-tile from the level's mesh tiling (the placed GridLODActors,
	 * via AInfiniteWaveManager::InferTileLayout) at BeginPlay, overriding WorldOffsetPerTileX/Y and
	 * FrameOffsetPerTileX below. The physics height data and the rendered meshes tile the same loop
	 * down the line; when the two are configured by hand they drift apart — measured 2026-09-17 as
	 * ~27 cm cross-shore + 3 frames per tile, enough that 200 m down the line the board looked like
	 * it rode behind the crest while the physics had it on the face
	 * (specs/wave-mesh-data-registration.md). Falls back to the values below when the level has no
	 * manager (Boards_on_flat_water). BaseOffsetX/Y and ReferenceFrameOffset (the constant part of
	 * the alignment) stay level-authored.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Infinite Tiling")
	bool bDeriveTilingFromWaveManager = true;

	/**
	 * Frame offset increment per tile in the X direction.
	 * When querying wave height at different tile positions, this offset is applied
	 * to create temporal variation. With bDeriveTilingFromWaveManager this is overwritten at
	 * BeginPlay with -FrameOffsetPerActor (the data tile index runs opposite to the mesh position
	 * index, so the sign flips); the value here is the fallback.
	 * Default: 80 frames per tile (same as InfiniteWaveManager default)
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Infinite Tiling")
	int32 FrameOffsetPerTileX = 80;

	/**
	 * Frame offset increment per tile in the Y direction.
	 * Usually 0 since tiling is primarily along the X axis (side-scrolling direction).
	 * Set to non-zero if your wave system tiles in both directions.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Infinite Tiling")
	int32 FrameOffsetPerTileY = 0;

	/**
	 * Reference frame offset for the tile at position (0, 0).
	 * This should match the frame offset of the GridLODActor at the reference position.
	 * Default: 0
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Infinite Tiling")
	int32 ReferenceFrameOffset = 0;

	/**
	 * X-axis world position offset per tile, in this actor's LOCAL frame (the actor is rotated
	 * -90 deg so the data's X axis runs down the line). With bDeriveTilingFromWaveManager this is
	 * overwritten at BeginPlay from the mesh tile step; the value here is the fallback.
	 * Default: 0
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Infinite Tiling")
	float WorldOffsetPerTileX = 0.0f;

	/**
	 * Y-axis world position offset per tile, in this actor's LOCAL frame. With
	 * bDeriveTilingFromWaveManager this is overwritten at BeginPlay from the mesh tile step; the
	 * value here is the fallback.
	 * Default: 8000 cm (80 meters, matching InfiniteWaveManager default)
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Infinite Tiling")
	float WorldOffsetPerTileY = 8000.0f;

	/**
	 * Base X-axis offset applied to all tiles (including tile 0,0).
	 * Use this to fine-tune the alignment in the X direction.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Infinite Tiling")
	float BaseOffsetX = 0.0f;

	/**
	 * Base Y-axis offset applied to all tiles (including tile 0,0).
	 * Use this to fine-tune the alignment in the Y direction.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Infinite Tiling")
	float BaseOffsetY = 0.0f;

	// ========== Wave Height Functions ==========

	tuple<double, FVector> waveHeightAndNormal(FVector location, int32 frame);

	// Internal function that accepts pre-fetched row pointers for better performance
	tuple<double, FVector> waveHeightAndNormalInternal(FVector location, FWaveUnifiedFrameData* FrameData, FWaveUnifiedMetadata* Metadata);

	UFUNCTION(BlueprintCallable, Category="Waves")
	TArray<FVector> calculateWaveLocationAndNormal(FVector location, int32 frame);

	UFUNCTION(BlueprintCallable, Category="Waves")
	FVector calculateWaveVelocity(FVector location, int32 frame, int32 TileIndexX = -999999, int32 TileIndexY = 0);

	// Get wave data (height and normal) around a given location
	// Returns an array of tuples containing height and normal for each point
	TArray<TTuple<double, FVector>> GetWaveDataAroundLocation(FVector centerLocation, int32 frame, float distanceBetweenPoints, int32 gridSizeX, int32 gridSizeY);

	// Helper functions for unified grid lookup
	tuple<float,float> convertWorldToGridIndices(FVector worldPos, FWaveUnifiedMetadata* Metadata);
	int32 CalculateFrameOffsetForPosition(FVector worldPos, FWaveUnifiedMetadata* Metadata);

	// Tile the position projects onto along the tile axis; the tile searches look +-kTileSearchRadius
	// around it. Tiles overlap/gap by (tile spacing - grid extent), so the estimate can be off by one.
	int32 EstimateTileIndex(FVector worldPos) const;
	static constexpr int32 kTileSearchRadius = 3;
	int32 getGridIndex(int32 x, int32 y, int32 width);
	float getHeightAtGridPoint(int32 gridX, int32 gridY, FWaveUnifiedFrameData* FrameData, FWaveUnifiedMetadata* Metadata);
	FVector getNormalAtGridPoint(int32 gridX, int32 gridY, FWaveUnifiedFrameData* FrameData, FWaveUnifiedMetadata* Metadata);
	FVector getVelocityAtGridPoint(int32 gridX, int32 gridY, FWaveUnifiedFrameData* FrameData, FWaveUnifiedMetadata* Metadata);
	FVector getVelocityBilinear(float gridX, float gridY, FWaveUnifiedFrameData* FrameData, FWaveUnifiedMetadata* Metadata);

	// New functions that read frame from WaterController
	UFUNCTION(BlueprintCallable, Category="Waves")
	TArray<FVector> calculateWaveLocationAndNormalAuto(FVector location);

	// Register the data tiling onto the level's mesh tiling (see bDeriveTilingFromWaveManager).
	void DeriveTilingFromWaveManager();

	// Annotate a recorded trace with the water surface under each row (see surf.waveprobe in the .cpp).
	// Exposed so the probe can be re-run from outside the game (RemoteControl) after tiling
	// parameters have been changed live — e.g. to check a candidate registration fix against the
	// rendered meshes without a relaunch. Output goes to <trace>.water.csv (overwritten).
	UFUNCTION(BlueprintCallable, Category="Waves|Debug")
	void ProbeTraceWaterSurface(const FString& TracePath);

	UFUNCTION(BlueprintCallable, Category="Waves")
	FVector calculateWaveVelocityAuto(FVector location);

	/**
	 * Calculate seam blend factor for a world position (for material use).
	 * Returns 0.0 at seam edges, 1.0 in the middle of tiles.
	 * Use this to reduce specular/adjust normals near seams in the material.
	 *
	 * @param WorldPosition The world position to check (usually vertex world position in material)
	 * @param BlendWidth Width in data space units over which to blend (higher = smoother transition)
	 * @return Blend factor from 0.0 (at seam) to 1.0 (away from seam)
	 */
	UFUNCTION(BlueprintCallable, Category="Waves")
	float GetSeamBlendFactor(FVector WorldPosition, float BlendWidth = 2.0f);

	/**
	 * Get seam boundary positions in world space for current tile.
	 * Returns the min and max X positions of seams (in actor's local space).
	 * Useful for debugging or advanced material effects.
	 */
	UFUNCTION(BlueprintCallable, Category="Waves")
	void GetSeamBoundaries(float& OutSeamMinX, float& OutSeamMaxX);

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FVector mockWaveVelocity = FVector(0, 0, 0);

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	bool doMockWaveVelocity = false;

	// ========== Debug Visualization ==========

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	bool bShowDebugVisualization = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	bool bShowDataBoundary = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	bool bShowSamplePoints = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	int32 SamplePointStride = 5;

	/** Start X index for sample point drawing (0 = start from beginning) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	int32 SamplePointStartX = 0;

	/** Number of X samples to draw (0 = draw all remaining) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	int32 SamplePointCountX = 0;

	/** Start Y index for sample point drawing (0 = start from beginning) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	int32 SamplePointStartY = 0;

	/** Number of Y samples to draw (0 = draw all remaining) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	int32 SamplePointCountY = 0;

	/** Draw the STORED surface normal at each sample point, plus its horizontal projection.
	 *  The horizontal projection is the axis wavePenetrationDrag resists along, so this is how you
	 *  check by eye whether that axis points across the face (correct) or along the crest (wrong).
	 *  See specs/wave-interaction-damping-and-redirect.md. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	bool bShowNormalDebug = false;

	/** Length (cm) of the drawn normal arrows. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	float NormalDebugScale = 150.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	float DebugSphereSize = 10.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	int32 DebugVisualizationFrame = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	bool bShowCornerMarkers = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	bool bShowLabels = true;

	/** Show velocity debug arrows */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	bool bShowVelocityDebug = false;

	/** Scale factor for velocity debug arrows (visual only, does not affect actual velocity) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	float VelocityDebugScale = 0.1f;

	/**
	 * Visualize multiple tiles for debugging infinite tiling.
	 * Shows debug spheres for adjacent tiles to verify seamless tiling.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug|Tiling")
	bool bShowMultipleTiles = false;

	/**
	 * Minimum tile index in X direction to visualize (e.g., -1 shows tile to the left)
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug|Tiling")
	int32 DebugTileMinX = -1;

	/**
	 * Maximum tile index in X direction to visualize (e.g., 1 shows tile to the right)
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug|Tiling")
	int32 DebugTileMaxX = 1;

	/**
	 * Minimum tile index in Y direction to visualize
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug|Tiling")
	int32 DebugTileMinY = 0;

	/**
	 * Maximum tile index in Y direction to visualize
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug|Tiling")
	int32 DebugTileMaxY = 0;

	/** Enable verbose logging for BeginPlay initialization and testing */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug|Logging")
	bool bEnableInitLogging = false;

	/** Enable verbose coordinate transformation logging */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug|Logging")
	bool bEnableCoordinateLogging = false;

	/** Enable velocity sampling debug logging */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug|Logging")
	bool bEnableVelocityLogging = false;

protected:
	// Called when the game starts or when spawned
	virtual void BeginPlay() override;

public:
	// Called every frame
	virtual void Tick(float DeltaTime) override;

	// Get current frame from WaterController (public so SharedCalculations can access it)
	int32 GetCurrentFrameFromWaterController() const;

private:
	// Draw debug visualization for data boundary and sample points
	void DrawDebugVisualization();

	// Test function to verify coordinate system with known wave peak location
	void TestKnownLocation();

	// Helper functions for reading from WaterController
	bool ReadIntPropertyFromWaterController(const FName& PropertyName, int32& OutValue) const;
	FName WrapAndConstructFrameName(int32 Frame) const;

};
