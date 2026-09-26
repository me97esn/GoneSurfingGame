// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Engine/DataTable.h"
#include "NiagaraDataChannelAccessor.h"
#include "NiagaraDataChannel.h"
#include "WaveHeight.h"
#include "ParticleSystemsController.generated.h"

class USoundBase;
class USoundAttenuation;
class UAudioComponent;

// Configuration for a single data channel with its own frame offset
USTRUCT(BlueprintType)
struct FDataChannelConfig
{
	GENERATED_BODY()

	// The Niagara Data Channel to write to
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Data Channel")
	UNiagaraDataChannelAsset* DataChannelAsset = nullptr;

	// Reference to the GridLodActor whose frame offset should be used
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Data Channel")
	TObjectPtr<AActor> GridLodActor = nullptr;

	// Name of the frame offset property to read from GridLodActor (default: "FrameOffset")
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Data Channel")
	FName FrameOffsetPropertyName = FName("FrameOffset");
};

UCLASS()
class GONESURFING_API AParticleSystemsController : public AActor
{
	GENERATED_BODY()

public:
	// Sets default values for this actor's properties
	AParticleSystemsController();

	// Set the GridLodActor reference for a specific data channel (called from Blueprint at runtime)
	UFUNCTION(BlueprintCallable, Category = "Particle System Controller")
	void SetDataChannelGridLodActor(int32 ChannelIndex, AActor* GridLodActor);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave Data")
	class UDataTable* WhiteWaterPointsDataTable;

	// Array of data channel configurations (each can have different frame offset)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Niagara Data Channels")
	TArray<FDataChannelConfig> DataChannels;

	// Data Channel parameter names
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Niagara Data Channel")
	FName PositionParamName = FName("Position");

	// Scale to apply to particle positions
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Niagara Data")
	FVector ParticlePositionScale = FVector(1.0f, 1.0f, 1.0f);

	// Maximum number of particles to send to each data channel (0 = no limit, useful for mobile performance)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Niagara Data", meta = (ClampMin = "0"))
	int32 MaxParticlesPerChannel = 0;

	// Expected particle spawn count per Niagara system (used to pad missing particles to (0,0,0))
	// This should match the particle count configured in your Niagara spawn module
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Niagara Data", meta = (ClampMin = "0"))
	int32 ParticleSpawnCount = 1000;

	// Camera reference for distance culling (falls back to player camera if null)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particle Culling")
	TObjectPtr<AActor> CameraActor = nullptr;

	// Maximum distance from camera to render particles in cm (0 = no distance culling)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particle Culling")
	float MaxParticleDistanceFromCamera = 0.0f;

	// Maximum total particles across ALL data channels (applied after distance culling, 0 = no limit)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particle Culling", meta = (ClampMin = "0"))
	int32 MaxTotalParticles = 3000;

	// Niagara component transform scale (must match Niagara system scale)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particle Culling")
	FVector NiagaraSystemScale = FVector(200.0f, -200.0f, 200.0f);

	// Niagara component local offset from GridLodActor (must match Niagara component location)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particle Culling")
	FVector NiagaraSystemOffset = FVector(68800.0f, 8100.0f, 1900.0f);

	// GridLodActor scale (affects Niagara component world transform)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particle Culling")
	FVector GridLodActorScale = FVector(0.1f, 0.1f, 0.1f);

	// Niagara component local rotation (must match Niagara component rotation)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particle Culling")
	FRotator NiagaraSystemRotation = FRotator(0.0f, 0.0f, 90.0f);

	// Enable debug visualization (spheres, lines, coordinate axes)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	bool bShowDebugVisualization = false;

	// Enable verbose per-frame log spam (camera target, global-limit summary, padding count)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	bool bDebugLogging = false;

	// Wave-break sound-emitter placement preview: per tile, collapse the whitewater point cloud into
	// candidate emitter positions (centroid + down-line leading/trailing edge) and draw them each frame.
	// Lets you play the whole loop and eyeball whether the emitter tracks the peeling break. Draw-only,
	// no audio; off by default so it never affects normal play or snapshot tests.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	bool bShowBreakPointDebug = false;

	// With bShowBreakPointDebug: also draw the raw foam points (off = spheres only, so the rendered
	// wave underneath stays visible; the wave-geometry debug uses this).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	bool bBreakPointDebugDrawFoam = true;

	// With bShowBreakPointDebug: draw the centroid/front spheres (off = cluster only, for GetCachedBreaks).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	bool bBreakPointDebugDrawSpheres = true;

	// Minimum foam-point count in a tile for it to count as "a wave is breaking here". Below this the
	// tile is treated as a gap between waves and no emitter sphere is drawn (emitter would be silent).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug", meta = (ClampMin = "0"))
	int32 BreakPointMinFoamCount = 30;

	// Lifetime (seconds) of the break-point debug draws. A positive value persists across a pause (the
	// last-drawn frame stays on screen while paused), so you can freeze and inspect. Higher = more trail
	// during live play as the front moves; lower = cleaner. Tune to taste.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug", meta = (ClampMin = "0.0"))
	float BreakPointDebugDrawDuration = 0.5f;

	// Size (cm) of the 2-D cells the foam is rasterised into for break clustering. Roughly the smallest
	// gap between two breaks that will be resolved as separate. ~400 = 4 m. Larger = fewer, coarser breaks.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug", meta = (ClampMin = "50.0"))
	float BreakClusterCellSize = 400.0f;

	// A foam cell counts as a dense "core" (vs the sparse trail joining two breaks) only if its point count
	// is at least this fraction of the densest cell. Higher = splits breaks more aggressively; lower = merges.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float BreakClusterDensityFrac = 0.25f;

	// ---- Wave Crash Audio (Layer 2, deterministic breaking-wave sound) ----
	// One looping mono crash voice per tile, positioned at that tile's loudest foam cluster (the same
	// clustering the debug spheres use), volume from its foam count, spatialised by WaveCrashAttenuation.
	// Off by default. See specs/surf-audio.md.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave Crash Audio")
	bool bWaveCrashAudioEnabled = false;

	// Looping crash MetaSound (a whitewater roar) whose graph reads the CrashIntensity input.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave Crash Audio")
	TObjectPtr<USoundBase> WaveCrashSound;

	// Attenuation asset for 3-D panning/falloff. Without one the crash plays 2-D (no spatialisation).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave Crash Audio")
	TObjectPtr<USoundAttenuation> WaveCrashAttenuation;

	// Break foam-point count that maps to full CrashIntensity (1.0).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave Crash Audio", meta = (ClampMin = "1.0"))
	float WaveCrashFoamForFull = 300.0f;

	// Per-update lerp toward the target intensity (0..1); lower = smoother/slower. Avoids clicks as breaks
	// appear, vanish, or the loudest cluster switches.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave Crash Audio", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float WaveCrashSmoothing = 0.15f;

	// MetaSound input the controller writes with the 0..1 crash loudness.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave Crash Audio")
	FName WaveCrashIntensityParam = FName("CrashIntensity");

	// Re-cluster a tile's foam for the audio only every N ticks (tiles staggered), i.e. ~10 Hz at 60 fps.
	// The crash target only has to move as fast as the foam does (wave-data rate), and the voice intensity
	// is lerped every tick regardless (WaveCrashSmoothing), so this is inaudible. MEASURED 2026-09-13 on
	// the Pixel 10: clustering every tile every tick cost 2.3 ms of the controller's 3.0 ms tick, ~22% of
	// all game-thread work. 1 = every tick (the old behaviour). The debug draw always clusters every tick.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave Crash Audio", meta = (ClampMin = "1"))
	int32 WaveCrashClusterIntervalTicks = 6;

	// Reference to the WaterController blueprint actor (reads CurrentFrame, StartFrame, EndFrame)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave Data")
	TObjectPtr<AActor> WaterController;

	// How often to update the data channel (1 = every frame, 2 = every other frame, etc.)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Performance")
	int32 UpdateFrequency = 1;

protected:
	// Called when the game starts or when spawned
	virtual void BeginPlay() override;

public:
	// Called every frame
	virtual void Tick(float DeltaTime) override;

	/** World-space positions of the white-water particles kept this update, cached for read-only
	 *  consumers (e.g. the wave-radar HUD). Refreshed each UpdateNiagaraParameters run. */
	const TArray<FVector>& GetWhiteWaterWorldPoints() const { return CachedWhiteWaterWorldPoints; }

	// ---- Wave-break clustering + wave-crash audio (Layer 2) ----
	// One breaking area: its dense-foam centroid (roar body), its down-line front (+Y, the crash), and
	// its foam-point count (loudness). Produced by ClusterFoam; consumed by the debug draw and the audio.
	struct FFoamBreakCluster
	{
		FVector Centroid = FVector::ZeroVector;
		FVector Front = FVector::ZeroVector;
		int32 Count = 0;
	};

	/** Per data channel (tile), the breaks from the last re-cluster. Filled while the crash audio is on
	 *  or bShowBreakPointDebug is set (every tick then); read by the wave-geometry debug
	 *  (specs/broken-wave-no-consequences.md T0) for the impact-point candidates. */
	const TArray<TArray<FFoamBreakCluster>>& GetCachedBreaks() const { return WaveCrashCachedBreaks; }

private:

	// 2-D dense-core flood-fill of a tile's world-space foam points into distinct breaks (see .cpp).
	static void ClusterFoam(const TArray<FVector>& Points, double CellSize, float DensityFrac,
	                        int32 MinClusterPoints, TArray<FFoamBreakCluster>& OutClusters);

	// Drive tile ChannelIndex's looping crash voice from its loudest break (spawns the voice lazily).
	void UpdateWaveCrashVoice(int32 ChannelIndex, const TArray<FFoamBreakCluster>& Breaks);

	// One persistent looping crash voice per data channel (tile), created on demand.
	UPROPERTY(Transient)
	TArray<TObjectPtr<UAudioComponent>> WaveCrashVoices;

	// Per-voice smoothed CrashIntensity (parallel to WaveCrashVoices) — lerped to avoid clicks.
	TArray<float> WaveCrashVoiceIntensity;

	// Per-channel breaks from the last re-cluster; fed to UpdateWaveCrashVoice on the ticks in between.
	TArray<TArray<FFoamBreakCluster>> WaveCrashCachedBreaks;

	// Set each tick: true when the water audio must be silent (world pause or an active replay).
	// Read by UpdateWaveCrashVoice so the crash target is forced to 0.
	bool bWaveCrashMuted = false;

	// World-space white-water points (see getter). Populated per kept particle in UpdateSingleDataChannel.
	TArray<FVector> CachedWhiteWaterWorldPoints;
	// Particle data with distance for global culling
	struct FParticleWithDistance
	{
		FVector Position;          // Data channel position
		float DistanceSquared;     // Distance to camera
		int32 ChannelIndex;        // Which data channel this belongs to
	};

	// Updates all data channels with data from the data table
	void UpdateNiagaraParameters();

	// Updates a single data channel with data for a specific frame
	// Returns array of particles with distances for global culling
	void UpdateSingleDataChannel(const FDataChannelConfig& ChannelConfig,
	                              int32 ChannelIndex,
	                              int32 CurrentFrame,
	                              int32 StartFrame,
	                              int32 EndFrame,
	                              const FVector& CameraLocation,
	                              TArray<FParticleWithDistance>& OutParticles);

	// Helper function to write data to a specific data channel
	void WriteDataToChannel(UNiagaraDataChannelAsset* ChannelAsset,
	                       const TArray<FVector>& Positions);

	// Get camera location for distance culling (from CameraActor or player camera)
	FVector GetCameraLocation() const;

	// Read an integer property from the WaterController by name
	bool ReadIntPropertyFromWaterController(const FName& PropertyName, int32& OutValue) const;

	// Read frame offset from a GridLodActor by property name
	bool ReadFrameOffsetFromGridLodActor(AActor* GridLodActor, const FName& PropertyName, int32& OutFrameOffset) const;

	// Search parameters for data channel writes
	FNiagaraDataChannelSearchParameters SearchParams;

	// Frame counter for throttling updates
	int32 FrameCounter = 0;
};
