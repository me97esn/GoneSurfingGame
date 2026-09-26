// Fill out your copyright notice in the Description page of Project Settings.

#include "ParticleSystemsController.h"
#include "SurfboardPawn.h"
#include "DrawDebugHelpers.h"
#include "Components/AudioComponent.h"
#include "Sound/SoundBase.h"
#include "Sound/SoundAttenuation.h"
#include "Kismet/GameplayStatics.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

// Define logging category for this actor
DEFINE_LOG_CATEGORY_STATIC(LogParticleSystemsController, Log, All);

// Sets default values
AParticleSystemsController::AParticleSystemsController()
{
	// Set this actor to call Tick() every frame
	PrimaryActorTick.bCanEverTick = true;
	// Keep ticking while paused so the wave-crash voices can be muted on pause (see Tick).
	PrimaryActorTick.bTickEvenWhenPaused = true;

	// Initialize pointers to nullptr
	WhiteWaterPointsDataTable = nullptr;
	WaterController = nullptr;
}

// Called when the game starts or when spawned
void AParticleSystemsController::BeginPlay()
{
	Super::BeginPlay();

	// Configure search parameters for data channel
	// In UE 5.5, Data Channels use an "islands" spatial system
	// We need to provide a location for the island
	SearchParams.bOverrideLocation = true;
	SearchParams.Location = GetActorLocation();

	UE_LOG(LogParticleSystemsController, Log, TEXT("ParticleSystemsController: BeginPlay - Actor location for data channel: %s"), *GetActorLocation().ToString());
}

void AParticleSystemsController::SetDataChannelGridLodActor(int32 ChannelIndex, AActor* GridLodActor)
{
	if (!DataChannels.IsValidIndex(ChannelIndex))
	{
		UE_LOG(LogParticleSystemsController, Warning, TEXT("SetDataChannelGridLodActor: Invalid channel index %d (have %d channels)"), ChannelIndex, DataChannels.Num());
		return;
	}

	DataChannels[ChannelIndex].GridLodActor = GridLodActor;
	UE_LOG(LogParticleSystemsController, Log, TEXT("SetDataChannelGridLodActor: Channel %d now references %s"),
		ChannelIndex, GridLodActor ? *GridLodActor->GetName() : TEXT("None"));
}

// Called every frame
void AParticleSystemsController::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	// Release wave-crash voices if the feature was toggled off, so they don't stick sounding.
	if (!bWaveCrashAudioEnabled && WaveCrashVoices.Num() > 0)
	{
		for (TObjectPtr<UAudioComponent>& V : WaveCrashVoices)
		{
			if (V) { V->Stop(); V->DestroyComponent(); }
			V = nullptr;
		}
		WaveCrashVoices.Reset();
		WaveCrashVoiceIntensity.Reset();
	}

	// Mute the wave-crash voices while the game is paused OR during a replay. On a true world pause the
	// wave is frozen so we also skip the Niagara/cluster update; during a replay the white-water must keep
	// animating to match the ride, so we still update it and let UpdateWaveCrashVoice honour bWaveCrashMuted.
	// Music (a separate ambient actor) keeps playing either way.
	const bool bWorldPaused = (GetWorld() && GetWorld()->IsPaused());
	bool bReplaying = false;
	if (const ASurfboardPawn* Pawn = Cast<ASurfboardPawn>(UGameplayStatics::GetPlayerPawn(this, 0)))
	{
		bReplaying = Pawn->bReplayActive;
	}
	bWaveCrashMuted = bWorldPaused || bReplaying;

	if (bWorldPaused)
	{
		for (int32 i = 0; i < WaveCrashVoices.Num(); ++i)
		{
			if (WaveCrashVoices[i] && WaveCrashVoiceIntensity.IsValidIndex(i))
			{
				WaveCrashVoiceIntensity[i] = 0.0f;
				WaveCrashVoices[i]->SetFloatParameter(WaveCrashIntensityParam, 0.0f);
			}
		}
		return;
	}

	// Update Niagara parameters every frame
	TRACE_CPUPROFILER_EVENT_SCOPE(ParticleSystemsController_Update);
	UpdateNiagaraParameters();
}

FVector AParticleSystemsController::GetCameraLocation() const
{
	// Use the configured CameraActor if set
	if (CameraActor)
	{
		return CameraActor->GetActorLocation();
	}

	// Fall back to player camera
	UWorld* World = GetWorld();
	if (World && World->GetFirstPlayerController())
	{
		APlayerController* PC = World->GetFirstPlayerController();
		if (PC && PC->PlayerCameraManager)
		{
			return PC->PlayerCameraManager->GetCameraLocation();
		}
	}

	// No camera available, return zero
	return FVector::ZeroVector;
}

bool AParticleSystemsController::ReadIntPropertyFromWaterController(const FName& PropertyName, int32& OutValue) const
{
	if (!WaterController)
	{
		return false;
	}

	UClass* WaterControllerClass = WaterController->GetClass();
	if (!WaterControllerClass)
	{
		return false;
	}

	// Try to find the property by name
	FProperty* Property = WaterControllerClass->FindPropertyByName(PropertyName);
	if (!Property)
	{
		// Try with space instead of no space (e.g., "Current Frame" vs "CurrentFrame")
		FString PropertyNameStr = PropertyName.ToString();
		// Try camelCase version
		PropertyNameStr.ReplaceInline(TEXT(" "), TEXT(""));
		Property = WaterControllerClass->FindPropertyByName(FName(*PropertyNameStr));
	}

	if (Property)
	{
		if (FIntProperty* IntProperty = CastField<FIntProperty>(Property))
		{
			OutValue = IntProperty->GetPropertyValue_InContainer(WaterController.Get());
			return true;
		}
		else if (FFloatProperty* FloatProperty = CastField<FFloatProperty>(Property))
		{
			float FloatValue = FloatProperty->GetPropertyValue_InContainer(WaterController.Get());
			OutValue = FMath::RoundToInt(FloatValue);
			return true;
		}
	}

	return false;
}

bool AParticleSystemsController::ReadFrameOffsetFromGridLodActor(AActor* GridLodActor, const FName& PropertyName, int32& OutFrameOffset) const
{
	if (!GridLodActor)
	{
		return false;
	}

	UClass* GridLodActorClass = GridLodActor->GetClass();
	if (!GridLodActorClass)
	{
		return false;
	}

	// Try to find the property by name
	FProperty* Property = GridLodActorClass->FindPropertyByName(PropertyName);
	if (!Property)
	{
		// Try with space instead of no space (e.g., "Frame Offset" vs "FrameOffset")
		FString PropertyNameStr = PropertyName.ToString();
		// Try camelCase version
		PropertyNameStr.ReplaceInline(TEXT(" "), TEXT(""));
		Property = GridLodActorClass->FindPropertyByName(FName(*PropertyNameStr));
	}

	if (Property)
	{
		if (FIntProperty* IntProperty = CastField<FIntProperty>(Property))
		{
			OutFrameOffset = IntProperty->GetPropertyValue_InContainer(GridLodActor);
			return true;
		}
		else if (FFloatProperty* FloatProperty = CastField<FFloatProperty>(Property))
		{
			float FloatValue = FloatProperty->GetPropertyValue_InContainer(GridLodActor);
			OutFrameOffset = FMath::RoundToInt(FloatValue);
			return true;
		}
	}

	return false;
}

void AParticleSystemsController::UpdateNiagaraParameters()
{
	// Throttle updates for performance (optional)
	FrameCounter++;
	if (FrameCounter % UpdateFrequency != 0)
	{
		return;
	}

	// Check required assets
	if (!WhiteWaterPointsDataTable)
	{
		return;
	}

	if (DataChannels.Num() == 0)
	{
		static bool bHasLoggedWarning = false;
		if (!bHasLoggedWarning)
		{
			UE_LOG(LogParticleSystemsController, Warning, TEXT("ParticleSystemsController: No data channels configured!"));
			bHasLoggedWarning = true;
		}
		return;
	}

	// Read CurrentFrame, StartFrame, EndFrame from WaterController
	int32 CurrentFrame = 0;
	int32 StartFrame = 0;
	int32 EndFrame = 0;

	if (!WaterController)
	{
		static bool bHasLoggedWarning = false;
		if (!bHasLoggedWarning)
		{
			UE_LOG(LogParticleSystemsController, Warning, TEXT("ParticleSystemsController: WaterController is not set!"));
			bHasLoggedWarning = true;
		}
		return;
	}

	// Read the properties from WaterController
	if (!ReadIntPropertyFromWaterController(FName("CurrentFrame"), CurrentFrame))
	{
		static bool bHasLoggedWarning = false;
		if (!bHasLoggedWarning)
		{
			UE_LOG(LogParticleSystemsController, Warning, TEXT("ParticleSystemsController: Could not read 'CurrentFrame' from WaterController"));
			bHasLoggedWarning = true;
		}
		return;
	}

	if (!ReadIntPropertyFromWaterController(FName("StartFrame"), StartFrame))
	{
		static bool bHasLoggedWarning = false;
		if (!bHasLoggedWarning)
		{
			UE_LOG(LogParticleSystemsController, Warning, TEXT("ParticleSystemsController: Could not read 'StartFrame' from WaterController"));
			bHasLoggedWarning = true;
		}
		return;
	}

	if (!ReadIntPropertyFromWaterController(FName("EndFrame"), EndFrame))
	{
		static bool bHasLoggedWarning = false;
		if (!bHasLoggedWarning)
		{
			UE_LOG(LogParticleSystemsController, Warning, TEXT("ParticleSystemsController: Could not read 'EndFrame' from WaterController"));
			bHasLoggedWarning = true;
		}
		return;
	}

	// Validate frame range
	const int32 TotalFrames = (EndFrame >= StartFrame) ? (EndFrame - StartFrame + 1) : 0;
	if (TotalFrames <= 0)
	{
		UE_LOG(LogParticleSystemsController, Warning, TEXT("ParticleSystemsController: Invalid frame range (StartFrame: %d, EndFrame: %d)"), StartFrame, EndFrame);
		return;
	}

	// Get camera location once for all channels
	const FVector CameraLocation = GetCameraLocation();
	const bool bUseDistanceCulling = (MaxParticleDistanceFromCamera > 0.0f);
	const bool bUseTotalParticleLimit = (MaxTotalParticles > 0);

	// Debug: Log camera info
	if (bDebugLogging)
	{
		static int32 CameraLogCounter = 0;
		if (CameraLogCounter < 3 || CameraLogCounter % 60 == 0)
		{
			UE_LOG(LogParticleSystemsController, Display, TEXT("Camera Debug: CameraActor=%s, Location=%s"),
				CameraActor ? *CameraActor->GetName() : TEXT("NULL (using player camera)"),
				*CameraLocation.ToString());
		}
		CameraLogCounter++;
	}

	// DEBUG DRAWING: Visualize the culling sphere around the camera
	if (bShowDebugVisualization && bUseDistanceCulling && GetWorld())
	{
		// Draw a sphere at camera location showing the culling distance
		DrawDebugSphere(
			GetWorld(),
			CameraLocation,
			MaxParticleDistanceFromCamera,
			32,                           // Segments
			FColor::Green,                // Color: Green = particles visible inside
			false,                        // Persistent lines
			-1.0f,                        // Lifetime (one frame)
			0,                            // Depth priority
			5.0f                          // Thickness
		);

		// Draw a point at camera location
		DrawDebugPoint(
			GetWorld(),
			CameraLocation,
			20.0f,                        // Size
			FColor::Red,                  // Color
			false,
			-1.0f
		);
	}

	// Phase 1: Distance culling per region
	TArray<FParticleWithDistance> AllParticles;

	// Refresh the world-space white-water cache (read by the wave-radar HUD). Filled per channel below.
	CachedWhiteWaterWorldPoints.Reset();

	for (int32 ChannelIndex = 0; ChannelIndex < DataChannels.Num(); ++ChannelIndex)
	{
		const FDataChannelConfig& ChannelConfig = DataChannels[ChannelIndex];

		TArray<FParticleWithDistance> ChannelParticles;
		UpdateSingleDataChannel(ChannelConfig, ChannelIndex, CurrentFrame, StartFrame, EndFrame,
		                       CameraLocation, ChannelParticles);

		// If no global limiting, write directly to channel
		if (!bUseTotalParticleLimit)
		{
			TArray<FVector> Positions;
			Positions.Reserve(ChannelParticles.Num());
			for (const FParticleWithDistance& Particle : ChannelParticles)
			{
				Positions.Add(Particle.Position);
			}

			if (Positions.Num() > 0)
			{
				WriteDataToChannel(ChannelConfig.DataChannelAsset, Positions);
			}
		}
		else
		{
			// Collect for global limiting
			AllParticles.Append(ChannelParticles);
		}
	}

	// Phase 2: Global particle limiting (if enabled)
	if (bUseTotalParticleLimit && AllParticles.Num() > 0)
	{
		// Sort by distance (closest first) if we need to limit
		if (AllParticles.Num() > MaxTotalParticles)
		{
			AllParticles.Sort([](const FParticleWithDistance& A, const FParticleWithDistance& B) {
				return A.DistanceSquared < B.DistanceSquared;
			});

			// Keep only the closest particles
			AllParticles.SetNum(MaxTotalParticles);
		}

		// Split particles back into their respective channels
		TArray<TArray<FVector>> ChannelPositions;
		ChannelPositions.SetNum(DataChannels.Num());

		for (const FParticleWithDistance& Particle : AllParticles)
		{
			if (ChannelPositions.IsValidIndex(Particle.ChannelIndex))
			{
				ChannelPositions[Particle.ChannelIndex].Add(Particle.Position);
			}
		}

		// Write to each channel
		for (int32 ChannelIndex = 0; ChannelIndex < DataChannels.Num(); ++ChannelIndex)
		{
			if (ChannelPositions[ChannelIndex].Num() > 0)
			{
				WriteDataToChannel(DataChannels[ChannelIndex].DataChannelAsset,
				                  ChannelPositions[ChannelIndex]);
			}
		}

		// Log summary
		if (bDebugLogging)
		{
			static int32 LogCounter = 0;
			if (LogCounter % 60 == 0 || LogCounter < 5)
			{
				UE_LOG(LogParticleSystemsController, Log,
					TEXT("ParticleSystemsController: Global limiting applied - Total particles: %d (limited to %d), Distance culling: %s"),
					AllParticles.Num() + (AllParticles.Num() < MaxTotalParticles ? 0 : 1), // Approximate original count
					AllParticles.Num(),
					bUseDistanceCulling ? TEXT("ON") : TEXT("OFF"));
			}
			LogCounter++;
		}
	}
}

void AParticleSystemsController::UpdateSingleDataChannel(const FDataChannelConfig& ChannelConfig,
                                                          int32 ChannelIndex,
                                                          int32 CurrentFrame,
                                                          int32 StartFrame,
                                                          int32 EndFrame,
                                                          const FVector& CameraLocation,
                                                          TArray<FParticleWithDistance>& OutParticles)
{
	// Check if this channel is configured properly
	if (!ChannelConfig.DataChannelAsset)
	{
		return;
	}

	// Read the current frame offset from the GridLodActor (dynamically syncs with actor)
	int32 DynamicFrameOffset = 0;
	if (ChannelConfig.GridLodActor)
	{
		if (!ReadFrameOffsetFromGridLodActor(ChannelConfig.GridLodActor.Get(),
		                                      ChannelConfig.FrameOffsetPropertyName,
		                                      DynamicFrameOffset))
		{
			static TMap<AActor*, bool> LoggedWarnings;
			if (!LoggedWarnings.Contains(ChannelConfig.GridLodActor.Get()))
			{
				UE_LOG(LogParticleSystemsController, Warning,
					TEXT("ParticleSystemsController: Could not read property '%s' from GridLodActor '%s'"),
					*ChannelConfig.FrameOffsetPropertyName.ToString(),
					*ChannelConfig.GridLodActor->GetName());
				LoggedWarnings.Add(ChannelConfig.GridLodActor.Get(), true);
			}
			// Use 0 as fallback
			DynamicFrameOffset = 0;
		}
	}
	else
	{
		// No GridLodActor reference - log warning once per config
		static bool bHasLoggedWarning = false;
		if (!bHasLoggedWarning)
		{
			UE_LOG(LogParticleSystemsController, Warning,
				TEXT("ParticleSystemsController: Data channel has no GridLodActor reference, using frame offset 0"));
			bHasLoggedWarning = true;
		}
	}

	// Apply FrameOffset to determine which frame to read
	int32 TargetFrame = CurrentFrame + DynamicFrameOffset;

	// Wrap around if we exceed EndFrame
	while (TargetFrame > EndFrame)
	{
		TargetFrame = StartFrame + (TargetFrame - EndFrame - 1);
	}
	while (TargetFrame < StartFrame)
	{
		TargetFrame = EndFrame - (StartFrame - TargetFrame - 1);
	}

	// Construct the row name for this frame using "Frame_{number}" format
	FName RowName = FName(*FString::Printf(TEXT("Frame_%d"), TargetFrame));

	// Fetch the data for this frame
	FWavePointsDataStruct2* WhiteWaterData = WhiteWaterPointsDataTable->FindRow<FWavePointsDataStruct2>(RowName, TEXT("WhiteWater"));

	if (!WhiteWaterData)
	{
		static TSet<int32> LoggedMissingFrames;
		if (!LoggedMissingFrames.Contains(TargetFrame))
		{
			UE_LOG(LogParticleSystemsController, Warning, TEXT("ParticleSystemsController: Could not find row '%s' in WhiteWaterPointsDataTable!"), *RowName.ToString());
			LoggedMissingFrames.Add(TargetFrame);
		}
		return;
	}

	// Get GridLodActor world position for distance calculations
	const FVector GridLodActorPos = ChannelConfig.GridLodActor ? ChannelConfig.GridLodActor->GetActorLocation() : FVector::ZeroVector;

	// Check if we should do distance culling
	const bool bUseDistanceCulling = (MaxParticleDistanceFromCamera > 0.0f);
	const float MaxDistanceSquared = MaxParticleDistanceFromCamera * MaxParticleDistanceFromCamera;

	// Build Niagara component's world transform using proper FTransform composition
	// Parent transform: GridLodActor (world position + scale)
	FTransform ParentTransform(FQuat::Identity, GridLodActorPos, GridLodActorScale);

	// Child transform: Niagara component (local offset + rotation + scale relative to parent)
	FTransform ChildLocalTransform(NiagaraSystemRotation, NiagaraSystemOffset, NiagaraSystemScale);

	// Compose transforms: ChildLocalTransform * ParentTransform = ComponentWorldTransform
	FTransform ComponentWorldTransform = ChildLocalTransform * ParentTransform;

	// DEBUG DRAWING: Visualize the Niagara component origin and coordinate system
	if (bShowDebugVisualization)
	{
		static int32 DebugDrawCounter = 0;
		if ((DebugDrawCounter < 5 || DebugDrawCounter % 60 == 0) && GetWorld())
		{
			// Draw coordinate axes at the Niagara component origin
			FVector ComponentOrigin = ComponentWorldTransform.GetLocation();
			DrawDebugCoordinateSystem(
				GetWorld(),
				ComponentOrigin,
				ComponentWorldTransform.Rotator(),
				1000.0f,                      // Axis length
				false,
				-1.0f,                        // Lifetime (one frame)
				0,
				10.0f                         // Thickness
			);

			// Draw a box at the component origin
			DrawDebugBox(
				GetWorld(),
				ComponentOrigin,
				FVector(100.0f),              // Extent
				ComponentWorldTransform.GetRotation(),
				FColor::Cyan,                 // Color
				false,
				-1.0f,
				0,
				5.0f
			);

			// Draw GridLodActor position
			DrawDebugSphere(
				GetWorld(),
				GridLodActorPos,
				200.0f,                       // Radius
				12,                           // Segments
				FColor::Yellow,               // Color
				false,
				-1.0f,
				0,
				5.0f
			);
		}
		DebugDrawCounter++;
	}

	// Wave-break clustering — shared by the debug spheres (bShowBreakPointDebug) and the wave-crash audio
	// (bWaveCrashAudioEnabled). Collapse this tile's whitewater point cloud into distinct breaking areas:
	// rasterise the foam into fixed 2-D cells, keep only cells that clear a fraction of the peak (drops the
	// sparse trail joining two breaks), then flood-fill those dense CORE cells into components. One component
	// = one real break (centroid = roar body, +Y edge = advancing front). Uses the RAW frame data,
	// independent of the distance culling / global limiting below.
	//
	// The audio path re-clusters only every WaveCrashClusterIntervalTicks ticks (tiles staggered so at most
	// one tile clusters per tick) and not at all while muted (replay / pause): UpdateWaveCrashVoice forces the
	// target to 0 then anyway. In between it is fed the cached breaks. See the header for the measurement.
	if (WaveCrashCachedBreaks.Num() != DataChannels.Num()) { WaveCrashCachedBreaks.SetNum(DataChannels.Num()); }
	const bool bClusterForAudio = bWaveCrashAudioEnabled && !bWaveCrashMuted &&
		(WaveCrashClusterIntervalTicks <= 1 || ((FrameCounter + ChannelIndex) % WaveCrashClusterIntervalTicks) == 0);
	if ((bShowBreakPointDebug || bClusterForAudio) && GetWorld())
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(ParticleSystemsController_ClusterFoam);
		const int32 FoamCount = WhiteWaterData->Positions.Num();

		// World-space foam for this frame (peel travels down-line along world +Y).
		TArray<FVector> Pts;
		Pts.Reserve(FoamCount);
		for (int32 i = 0; i < FoamCount; ++i)
		{
			Pts.Add(ComponentWorldTransform.TransformPosition(WhiteWaterData->Positions[i] * ParticlePositionScale));
		}

		// Cluster into breaks (empty when below the "any break at all" foam threshold).
		TArray<FFoamBreakCluster> Breaks;
		if (FoamCount >= BreakPointMinFoamCount)
		{
			ClusterFoam(Pts, FMath::Max(50.0, (double)BreakClusterCellSize), BreakClusterDensityFrac,
			            FMath::Max(5, BreakPointMinFoamCount / 3), Breaks);
		}

		if (bShowBreakPointDebug)
		{
			// Faint dots = the actual foam, so the emitter positions can be judged against it.
			if (bBreakPointDebugDrawFoam)
			{
				for (const FVector& P : Pts)
				{
					DrawDebugPoint(GetWorld(), P, 5.0f, FColor(110, 110, 120), false, BreakPointDebugDrawDuration);
				}
			}
			// GREEN = each break's centroid (roar body, bigger = louder); RED = its +Y front (crash).
			int32 Largest = 0;
			for (const FFoamBreakCluster& B : Breaks)
			{
				const float Radius = 90.0f + 90.0f * FMath::Clamp((float)B.Count / 200.0f, 0.0f, 1.0f);
				if (bBreakPointDebugDrawSpheres)
				{
					DrawDebugSphere(GetWorld(), B.Centroid, Radius, 16, FColor::Green, false, BreakPointDebugDrawDuration, 0, 4.0f);
					DrawDebugSphere(GetWorld(), B.Front,     80.0f,  12, FColor::Red,   false, BreakPointDebugDrawDuration, 0, 4.0f);
				}
				Largest = FMath::Max(Largest, B.Count);
			}
			if (Breaks.Num() == 0 && FoamCount >= BreakPointMinFoamCount)
			{
				// Foam present but too diffuse to cluster — fall back to the mean so the tile isn't blank.
				FVector Mean = FVector::ZeroVector;
				for (const FVector& P : Pts) { Mean += P; }
				if (Pts.Num() > 0)
				{
					DrawDebugSphere(GetWorld(), Mean / Pts.Num(), 110.0f, 16, FColor::Green, false, BreakPointDebugDrawDuration, 0, 3.0f);
				}
			}
			if (GEngine)
			{
				const FString TileName = ChannelConfig.GridLodActor ? ChannelConfig.GridLodActor->GetName() : FString(TEXT("tile"));
				const FColor Col = (FoamCount >= BreakPointMinFoamCount && Breaks.Num() > 0) ? FColor::Green : FColor(90, 90, 90);
				GEngine->AddOnScreenDebugMessage((uint64)(2000 + ChannelIndex), BreakPointDebugDrawDuration, Col,
					FString::Printf(TEXT("[break] %-16s f=%4d  foam=%4d  breaks=%d (max %d)"),
						*TileName, TargetFrame, FoamCount, Breaks.Num(), Largest));
			}
		}

		// Cached for the audio, and for the wave-geometry debug (WaveGeoDebugSubsystem) when the break
		// draw is on: it reads the +Y fronts as the impact-point candidates.
		if ((bWaveCrashAudioEnabled || bShowBreakPointDebug) && WaveCrashCachedBreaks.IsValidIndex(ChannelIndex))
		{
			WaveCrashCachedBreaks[ChannelIndex] = MoveTemp(Breaks);
		}
	}
	if (bWaveCrashAudioEnabled && WaveCrashCachedBreaks.IsValidIndex(ChannelIndex))
	{
		UpdateWaveCrashVoice(ChannelIndex, WaveCrashCachedBreaks[ChannelIndex]);
	}

	// Process particles with distance culling
	TRACE_CPUPROFILER_EVENT_SCOPE(ParticleSystemsController_CullParticles);
	const int32 SourceParticleCount = WhiteWaterData->Positions.Num();
	int32 ParticlesBeforeCulling = 0;
	int32 ParticlesAfterCulling = 0;

	for (int32 i = 0; i < SourceParticleCount; ++i)
	{
		// Apply per-channel limiting first if enabled
		if (MaxParticlesPerChannel > 0 && ParticlesBeforeCulling >= MaxParticlesPerChannel)
		{
			break;
		}

		ParticlesBeforeCulling++;

		// Get particle position (apply ParticlePositionScale)
		FVector DataChannelPos = WhiteWaterData->Positions[i] * ParticlePositionScale;

		// Calculate distance culling check - cull particles that are TOO FAR
		bool bKeepParticle = true;
		float DistanceSquared = 0.0f;

		if (bUseDistanceCulling)
		{
			// Transform particle to world space to calculate distance
			FVector ParticleWorldPos = ComponentWorldTransform.TransformPosition(DataChannelPos);
			DistanceSquared = FVector::DistSquared(ParticleWorldPos, CameraLocation);

			// Cull if distance is GREATER than max distance (keep close particles, cull far ones)
			bKeepParticle = (DistanceSquared <= MaxDistanceSquared);
		}

		if (bKeepParticle)
		{
			FParticleWithDistance Particle;
			Particle.Position = DataChannelPos;
			Particle.DistanceSquared = DistanceSquared;
			Particle.ChannelIndex = ChannelIndex;
			OutParticles.Add(Particle);
			ParticlesAfterCulling++;

			// Cache the world-space position for read-only consumers (wave-radar HUD). The per-channel
			// ComponentWorldTransform is only valid here, so we compute it now rather than post-hoc.
			CachedWhiteWaterWorldPoints.Add(ComponentWorldTransform.TransformPosition(DataChannelPos));
		}
	}

	// Debug logging with sample particle positions
	static int32 LogCounter = 0;
	if (LogCounter < 5 || LogCounter % 60 == 0)
	{
		// UE_LOG(LogParticleSystemsController, Log,
		// 	TEXT("ParticleSystemsController: Channel[%d] Frame=%d, GridLodActor=%s, GridPos=%s, CameraPos=%s, Particles: %d->%d (distance culling: %s)"),
		// 	ChannelIndex, TargetFrame,
		// 	ChannelConfig.GridLodActor ? *ChannelConfig.GridLodActor->GetName() : TEXT("None"),
		// 	*GridLodActorPos.ToString(),
		// 	*CameraLocation.ToString(),
		// 	ParticlesBeforeCulling, ParticlesAfterCulling,
		// 	bUseDistanceCulling ? TEXT("ON") : TEXT("OFF"));

		// Log multiple sample particles for debugging distance calculations
		if (OutParticles.Num() > 0)
		{
			// UE_LOG(LogParticleSystemsController, Log, TEXT("  Sample particles (showing first 3):"));
			// UE_LOG(LogParticleSystemsController, Log, TEXT("  MaxDistance=%.2f"), MaxParticleDistanceFromCamera);

			int32 SamplesToLog = FMath::Min(3, OutParticles.Num());
			for (int32 SampleIdx = 0; SampleIdx < SamplesToLog; ++SampleIdx)
			{
				const FParticleWithDistance& SampleParticle = OutParticles[SampleIdx];
				// Use the correct ComponentWorldTransform to convert to world space for logging
				FVector SampleWorldPos = ComponentWorldTransform.TransformPosition(SampleParticle.Position);
				float SampleDist = FMath::Sqrt(SampleParticle.DistanceSquared);

				// UE_LOG(LogParticleSystemsController, Log,
				// 	TEXT("    [%d] DataChannelPos=%s, CalcWorldPos=%s, Distance=%.2f"),
				// 	SampleIdx,
				// 	*SampleParticle.Position.ToString(),
				// 	*SampleWorldPos.ToString(),
				// 	SampleDist);

				// DEBUG DRAWING: Draw sample particle positions in world space
				if (bShowDebugVisualization && GetWorld())
				{
					// Draw particle as a small sphere
					DrawDebugSphere(
						GetWorld(),
						SampleWorldPos,
						50.0f,                    // Radius
						8,                        // Segments
						FColor::Magenta,          // Color: Magenta for sample particles
						false,
						-1.0f,
						0,
						2.0f
					);

					// Draw line from particle to camera
					DrawDebugLine(
						GetWorld(),
						SampleWorldPos,
						CameraLocation,
						FColor::Orange,           // Color
						false,
						-1.0f,
						0,
						1.0f                      // Thickness
					);
				}
			}
		}
	}
	LogCounter++;
}

void AParticleSystemsController::WriteDataToChannel(
	UNiagaraDataChannelAsset* ChannelAsset,
	const TArray<FVector>& Positions)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(ParticleSystemsController_WriteDataToChannel);
	if (!ChannelAsset)
	{
		UE_LOG(LogParticleSystemsController, Warning, TEXT("ParticleSystemsController: ChannelAsset is null!"));
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		UE_LOG(LogParticleSystemsController, Warning, TEXT("ParticleSystemsController: World is null!"));
		return;
	}

	// Determine how many particles to write
	// If ParticleSpawnCount is set, always write that many particles (padding with 0,0,0 if needed)
	// This ensures consistent particle count matching the Niagara spawn count
	const int32 ActualParticleCount = Positions.Num();
	const int32 NumParticlesToWrite = (ParticleSpawnCount > 0) ? ParticleSpawnCount : ActualParticleCount;

	// Early exit only if we have no spawn count configured and no positions
	if (NumParticlesToWrite == 0)
	{
		UE_LOG(LogParticleSystemsController, Verbose, TEXT("ParticleSystemsController: No positions to write."));
		return;
	}

	// Log if we're padding particles
	if (bDebugLogging)
	{
		static int32 PaddingLogCounter = 0;
		if (ActualParticleCount < NumParticlesToWrite && (PaddingLogCounter < 3 || PaddingLogCounter % 60 == 0))
		{
			UE_LOG(LogParticleSystemsController, Log,
				TEXT("ParticleSystemsController: Padding particles - Frame has %d particles, padding to %d with (0,0,0)"),
				ActualParticleCount, NumParticlesToWrite);
		}
		PaddingLogCounter++;
	}

	UE_LOG(LogParticleSystemsController, Verbose, TEXT("ParticleSystemsController: Attempting to write %d particles to channel '%s' at location %s"),
		NumParticlesToWrite, *ChannelAsset->GetName(), *SearchParams.Location.ToString());

	// Create writer for this frame
	UNiagaraDataChannelWriter* Writer = UNiagaraDataChannelLibrary::WriteToNiagaraDataChannel(
		World,
		ChannelAsset,
		SearchParams,
		NumParticlesToWrite,
		false,  // bVisibleToGame
		true,   // bVisibleToCPU
		true,   // bVisibleToGPU
		TEXT("WaveDisplay")
	);

	if (!Writer)
	{
		UE_LOG(LogParticleSystemsController, Error, TEXT("ParticleSystemsController: Failed to create data channel writer for asset '%s' with %d particles! Check: 1) Data Channel has correct variables defined, 2) Data Channel 'Mode' is set to 'Publish', 3) Niagara system is reading from this channel"),
			*ChannelAsset->GetName(), NumParticlesToWrite);
		return;
	}

	// Write all particle positions
	// Note: Data Channels don't have a batch write API, so we must write element-by-element
	for (int32 i = 0; i < NumParticlesToWrite; ++i)
	{
		// Use actual position if available, otherwise use (0,0,0) for padding
		FVector PositionToWrite = (i < ActualParticleCount) ? Positions[i] : FVector::ZeroVector;
		Writer->WritePosition(PositionParamName, i, PositionToWrite);
	}
}

void AParticleSystemsController::ClusterFoam(const TArray<FVector>& Pts, double CellSize, float DensityFrac,
                                             int32 MinClusterPoints, TArray<FFoamBreakCluster>& OutClusters)
{
	// 2-D dense-core flood-fill: rasterise the foam into fixed CellSize cells in the horizontal plane, keep
	// only cells whose count clears DensityFrac × the peak cell (drops the sparse trail that would otherwise
	// merge two breaks), then flood-fill (8-connected) those core cells into components. Each component that
	// clears MinClusterPoints becomes one break. Player-validated placement; see specs/surf-audio.md.
	OutClusters.Reset();
	if (Pts.Num() == 0)
	{
		return;
	}

	double MinX = TNumericLimits<double>::Max(), MaxX = -TNumericLimits<double>::Max();
	double MinY = TNumericLimits<double>::Max(), MaxY = -TNumericLimits<double>::Max();
	for (const FVector& P : Pts)
	{
		MinX = FMath::Min(MinX, P.X); MaxX = FMath::Max(MaxX, P.X);
		MinY = FMath::Min(MinY, P.Y); MaxY = FMath::Max(MaxY, P.Y);
	}

	const int32 GridW = FMath::Clamp((int32)((MaxX - MinX) / CellSize) + 1, 1, 96);
	const int32 GridH = FMath::Clamp((int32)((MaxY - MinY) / CellSize) + 1, 1, 96);
	const int32 NumCells = GridW * GridH;

	TArray<int32> CellCount; CellCount.Init(0, NumCells);
	TArray<FVector> CellSum; CellSum.Init(FVector::ZeroVector, NumCells);
	int32 PeakCell = 0;
	for (const FVector& P : Pts)
	{
		const int32 ix = FMath::Clamp((int32)((P.X - MinX) / CellSize), 0, GridW - 1);
		const int32 iy = FMath::Clamp((int32)((P.Y - MinY) / CellSize), 0, GridH - 1);
		const int32 c = ix + iy * GridW;
		CellCount[c]++;
		CellSum[c] += P;
		PeakCell = FMath::Max(PeakCell, CellCount[c]);
	}

	const int32 CoreFloor = FMath::Max(2, (int32)(DensityFrac * PeakCell));

	TArray<bool> Visited; Visited.Init(false, NumCells);
	TArray<int32> Stack;
	for (int32 Start = 0; Start < NumCells; ++Start)
	{
		if (Visited[Start] || CellCount[Start] < CoreFloor)
		{
			continue;
		}

		Stack.Reset();
		Stack.Push(Start);
		Visited[Start] = true;
		int32 CompPts = 0;
		FVector CompSum = FVector::ZeroVector;
		FVector FrontPos = FVector::ZeroVector;
		double FrontY = -TNumericLimits<double>::Max();
		while (Stack.Num() > 0)
		{
			const int32 c = Stack.Pop();
			CompPts += CellCount[c];
			CompSum += CellSum[c];
			const FVector CellPos = CellSum[c] / CellCount[c];
			if (CellPos.Y > FrontY) { FrontY = CellPos.Y; FrontPos = CellPos; }

			const int32 cx = c % GridW, cy = c / GridW;
			for (int32 dy = -1; dy <= 1; ++dy)
			{
				for (int32 dx = -1; dx <= 1; ++dx)
				{
					if (dx == 0 && dy == 0) { continue; }
					const int32 nx = cx + dx, ny = cy + dy;
					if (nx < 0 || nx >= GridW || ny < 0 || ny >= GridH) { continue; }
					const int32 n = nx + ny * GridW;
					if (!Visited[n] && CellCount[n] >= CoreFloor)
					{
						Visited[n] = true;
						Stack.Push(n);
					}
				}
			}
		}

		if (CompPts >= MinClusterPoints)
		{
			FFoamBreakCluster Cluster;
			Cluster.Centroid = CompSum / CompPts;
			Cluster.Front = FrontPos;
			Cluster.Count = CompPts;
			OutClusters.Add(Cluster);
		}
	}
}

void AParticleSystemsController::UpdateWaveCrashVoice(int32 ChannelIndex, const TArray<FFoamBreakCluster>& Breaks)
{
	if (!WaveCrashSound)
	{
		return;
	}

	// Keep the per-tile voice arrays sized to the channel (tile) count.
	const int32 NumTiles = DataChannels.Num();
	if (WaveCrashVoices.Num() != NumTiles) { WaveCrashVoices.SetNum(NumTiles); }
	if (WaveCrashVoiceIntensity.Num() != NumTiles) { WaveCrashVoiceIntensity.SetNum(NumTiles); }
	if (!WaveCrashVoices.IsValidIndex(ChannelIndex))
	{
		return;
	}

	// Spawn this tile's looping voice on first use.
	if (!WaveCrashVoices[ChannelIndex])
	{
		UAudioComponent* AC = NewObject<UAudioComponent>(this);
		if (!AC)
		{
			return;
		}
		AC->bAutoActivate = false;
		AC->bAllowSpatialization = true;
		AC->SetSound(WaveCrashSound);
		if (WaveCrashAttenuation) { AC->AttenuationSettings = WaveCrashAttenuation; }
		AC->RegisterComponent();
		AC->Play();
		WaveCrashVoices[ChannelIndex] = AC;
		WaveCrashVoiceIntensity[ChannelIndex] = 0.0f;
	}
	UAudioComponent* Voice = WaveCrashVoices[ChannelIndex];
	if (!Voice)
	{
		return;
	}

	// The loudest break in this tile drives the voice (position + intensity). One voice per tile for now;
	// a simultaneous second break in the same tile is not yet voiced (see specs/surf-audio.md open questions).
	const FFoamBreakCluster* Best = nullptr;
	for (const FFoamBreakCluster& B : Breaks)
	{
		if (!Best || B.Count > Best->Count) { Best = &B; }
	}

	float Target = 0.0f;
	if (!bWaveCrashMuted && Best && Best->Count >= BreakPointMinFoamCount)
	{
		Target = FMath::Clamp((float)Best->Count / FMath::Max(1.0f, WaveCrashFoamForFull), 0.0f, 1.0f);
		Voice->SetWorldLocation(Best->Centroid);
	}

	// Smooth toward the target so voices don't click as breaks appear/vanish/switch. Kept playing at 0
	// intensity in the gaps (the MetaSound gains to silence) rather than stopped, to avoid restart clicks.
	float& Cur = WaveCrashVoiceIntensity[ChannelIndex];
	Cur = FMath::Lerp(Cur, Target, FMath::Clamp(WaveCrashSmoothing, 0.0f, 1.0f));
	Voice->SetFloatParameter(WaveCrashIntensityParam, Cur);
}
