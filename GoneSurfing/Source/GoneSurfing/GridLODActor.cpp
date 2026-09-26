// Fill out your copyright notice in the Description page of Project Settings.

#include "GridLODActor.h"
#include "SurfLog.h"
#include "SurfDebug.h"
#include "Engine/StaticMesh.h"
#include "Camera/PlayerCameraManager.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetMaterialLibrary.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialParameterCollectionInstance.h"
#include "WaveHeight.h"
#include "HAL/IConsoleManager.h"
#include "UnrealClient.h"           // FScreenshotRequest
#include "HAL/PlatformMisc.h"

#if WITH_EDITOR
#include "Editor.h"
#include "EditorViewportClient.h"
#endif

// ------------------------------------------------------------------------------------------------
// Visual verification harness (no-op unless -GridShots is passed on the command line).
// Lets you A/B the async vs synchronous mesh loading in a single build and confirm the waves are
// identical: force the mode with -GridAsync, then screenshot at specific WAVE FRAME numbers
// (so sync and async captures line up frame-for-frame regardless of timing/hitches). Config is read
// from the command line (not -ExecCmds) so it applies before the BeginPlay warmup preload.
//   -GridAsync=0                     -> synchronous (old behaviour)
//   -GridAsync=1                     -> asynchronous (new behaviour)
//   -GridShots=950,1000,1050         -> screenshot at those wave frames, then quit
// Files land in Saved/Screenshots/Windows/ named wave_<sync|async>_f<frame>.
// ------------------------------------------------------------------------------------------------

AGridLODActor::AGridLODActor()
	: GridWidth(2)
	, GridHeight(1)
	, GridSize(FVector2D(100.0f, 100.0f))
	, WaterController(nullptr)
	, FrameOffset(0)
	, StartFrame(886)
	, EndFrame(1078)
	, WorldPositionOffset(FVector2D::ZeroVector)
	, BaseMeshFolderPath(TEXT("/Game/Waves"))
	, HighResFolderName(TEXT("chunks_ratio_0_03"))
	, LowResFolderName(TEXT("chunks_ratio_0_005"))
	, GridMaterial(nullptr)
	, EditorPreviewFrame(900)
	, EditorPreviewLOD(1)  // Default to low-res in editor
	, LODDistanceThreshold(3000.0f)  // 30 meters - only nearby chunks are high-res
	, LODHysteresis(500.0f)  // 5 meters - prevents rapid switching at threshold boundary
	, MaxVisibleDistance(150.0f)
	, bUse2DDistance(true)
	, bUseDistanceSquared(true)
	, ForceLowLODCells(TEXT("0,2"))  // Force edge chunks (horizon + beach) to low-LOD by default
	, bEnableMeshStreaming(true)
	, bUseSelectivePreloading(true)
	, bUse8ConnectedNeighbors(true)
	, PreloadBorderSize(0)
	, PreloadFrameCount(10)  // Preload 10 frames ahead (mobile-optimized, same for PC and Android)
	, CacheFrameCount(20)    // Keep only 20 frames in cache (~62MB per actor, ~310MB for 5 actors)
	, bEnableSeamBlending(false)
	, WaveHeightActor(nullptr)
	, SeamBlendingMPC(nullptr)
	, bShowDebugVisualization(false)
	, CurrentFrame(0)
	, NumHighLODCells(0)
	, NumLowLODCells(0)
	, NumCulledCells(0)
	, LastStreamedFrame(-1)
	, InitialLoadingProgress(0)
	, bInitialWarmupComplete(false)
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;
	PrimaryActorTick.bTickEvenWhenPaused = false;
	PrimaryActorTick.TickGroup = TG_PostUpdateWork;  // Tick after physics

	// Enable ticking in all modes (including simulate)
	SetActorTickEnabled(true);

	// Create a default root component
	USceneComponent* RootComp = CreateDefaultSubobject<USceneComponent>(TEXT("RootComponent"));
	RootComponent = RootComp;
}

void AGridLODActor::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);

	// Initialize grid when actor is constructed (works in both editor and game)
	InitializeGrid();

#if WITH_EDITOR
	// Load editor preview meshes (visible in editor viewport)
	if (!GetWorld()->IsGameWorld())
	{
		// Preload frames for smooth startup (platform-specific)
		// This runs in editor before Play, so blocking is acceptable
		int32 AnimationLength = EndFrame - StartFrame + 1;

		// Preload 50 frames (mobile-optimized, same for PC and Android for easier testing)
		int32 EditorPreloadFrames = FMath::Min(50, AnimationLength);
		UE_LOG(LogSurf, Warning, TEXT("GridLODActor: OnConstruction preloading %d frames starting from %d"), EditorPreloadFrames, StartFrame);
		PreloadMeshesSynchronous(StartFrame, EditorPreloadFrames);

		// Now set the preview mesh for the current frame
		for (UGridCellComponent* Cell : GridCells)
		{
			if (Cell)
			{
				// Apply frame rounding based on preview LOD level
				int32 ActualPreviewFrame = RoundFrameForLOD(EditorPreviewFrame, EditorPreviewLOD);

				UStaticMesh* PreviewMesh = LoadGridMesh(Cell->GridX, Cell->GridY, ActualPreviewFrame, EditorPreviewLOD);
				if (PreviewMesh)
				{
					Cell->SetStaticMesh(PreviewMesh);
					// IMPORTANT: Set current LOD and frame to match what was loaded
					// This prevents unnecessary reloading when Simulate starts
					Cell->CurrentLODLevel = EditorPreviewLOD;
					Cell->CurrentFrame = ActualPreviewFrame;

					// Apply material if set
					if (GridMaterial)
					{
						Cell->SetMaterial(0, GridMaterial);
					}
				}
			}
		}
	}
#endif
}

void AGridLODActor::BeginPlay()
{
	Super::BeginPlay();

	// Verification harness config, read from the command line so it's available BEFORE the warmup
	// preload below (unlike -ExecCmds, which run after BeginPlay). See the harness block near the
	// top of this file.  -GridAsync=0|1  -GridShots=920,980,1040
	{
		int32 AsyncVal = -1;
		if (FParse::Value(FCommandLine::Get(), TEXT("-GridAsync="), AsyncVal) && AsyncVal >= 0)
		{
			bUseAsyncPreload = (AsyncVal != 0);
			UE_LOG(LogSurf, Warning, TEXT("GridLODActor: -GridAsync override -> bUseAsyncPreload=%d"), bUseAsyncPreload ? 1 : 0);
		}

		FString ShotsStr;
		// bShouldStopOnSeparator=false so the comma-separated list isn't truncated at the first comma.
		if (FParse::Value(FCommandLine::Get(), TEXT("-GridShots="), ShotsStr, false))
		{
			TArray<FString> Tokens;
			ShotsStr.ParseIntoArray(Tokens, TEXT(","), true);
			for (const FString& Tok : Tokens)
			{
				const FString Trimmed = Tok.TrimStartAndEnd();
				if (Trimmed.IsNumeric()) { VerifyTargetFrames.AddUnique(FCString::Atoi(*Trimmed)); }
			}
			VerifyTargetFrames.Sort();
		}
		bVerifyShotsInit = true;
		if (FrameOffset == 0 && VerifyTargetFrames.Num() > 0)
		{
			UE_LOG(LogSurf, Warning, TEXT("GridLODActor: verification capture armed for %d frame(s), mode=%s"),
				VerifyTargetFrames.Num(), bUseAsyncPreload ? TEXT("async") : TEXT("sync"));
		}
	}

	UE_LOG(LogSurf, Display, TEXT("GridLODActor: BeginPlay called - GridCells.Num() = %d"), GridCells.Num());

	// Initialize the grid if it hasn't been created yet
	if (GridCells.Num() == 0)
	{
		UE_LOG(LogSurf, Warning, TEXT("GridLODActor: Grid not initialized, calling InitializeGrid from BeginPlay"));
		InitializeGrid();
	}
	else
	{
		UE_LOG(LogSurf, Log, TEXT("GridLODActor: Grid already initialized with %d cells"), GridCells.Num());
	}

	// Check if cache was preserved from OnConstruction
	int32 CurrentFrameNumber = GetCurrentFrameFromController();
	UE_LOG(LogSurf, Display, TEXT("GridLODActor: BeginPlay - Cache has %d meshes, current frame is %d (OnConstruction should have preloaded ~4800 meshes)"),
		MeshCache.Num(), CurrentFrameNumber);

	// If cache is empty (wasn't preserved from OnConstruction), do synchronous preloading
	// This happens because Unreal clears non-serialized data when transitioning to Play mode
	if (MeshCache.Num() < 100)  // Less than 100 meshes means cache is essentially empty
	{
		UE_LOG(LogSurf, Warning, TEXT("GridLODActor: Cache is empty/small (%d meshes), doing synchronous preload starting from frame %d (this will cause a brief freeze)"),
			MeshCache.Num(), CurrentFrameNumber);
		// Preload 20 frames (mobile-optimized, same for PC and Android for easier testing)
		PreloadMeshesSynchronous(CurrentFrameNumber, 20);
		UE_LOG(LogSurf, Warning, TEXT("GridLODActor: After preload, cache now has %d meshes"), MeshCache.Num());
	}

	// Perform initial mesh update using cached meshes
	UpdateGridMeshes();

	UE_LOG(LogSurf, Display, TEXT("GridLODActor: BeginPlay complete"));
}

void AGridLODActor::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	// Update grid meshes every frame based on camera position and current frame
	UpdateGridMeshes();

	// Update seam blending parameters if enabled
	if (bEnableSeamBlending)
	{
		UpdateSeamBlendParameters();
	}
}

#if WITH_EDITOR
void AGridLODActor::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	if (PropertyChangedEvent.Property)
	{
		const FName PropertyName = PropertyChangedEvent.Property->GetFName();

		// Reinitialize grid if dimensions change
		if (PropertyName == GET_MEMBER_NAME_CHECKED(AGridLODActor, GridWidth) ||
			PropertyName == GET_MEMBER_NAME_CHECKED(AGridLODActor, GridHeight) ||
			PropertyName == GET_MEMBER_NAME_CHECKED(AGridLODActor, GridSize))
		{
			InitializeGrid();
		}
	}
}
#endif

void AGridLODActor::InitializeGrid()
{
	UE_LOG(LogSurf, Warning, TEXT("GridLODActor: InitializeGrid called"));

	// Clear existing grid
	ClearGrid();

	// Validate grid dimensions
	if (GridWidth <= 0 || GridHeight <= 0)
	{
		UE_LOG(LogSurf, Error, TEXT("GridLODActor: Invalid grid dimensions (%dx%d)"), GridWidth, GridHeight);
		return;
	}

	if (GridSize.X <= 0 || GridSize.Y <= 0)
	{
		UE_LOG(LogSurf, Error, TEXT("GridLODActor: Invalid grid size (%.1f, %.1f)"), GridSize.X, GridSize.Y);
		return;
	}

	// Calculate cell size
	const float CellWidth = GridSize.X / GridWidth;
	const float CellHeight = GridSize.Y / GridHeight;

	UE_LOG(LogSurf, Log, TEXT("GridLODActor: Cell size = %.1f x %.1f"), CellWidth, CellHeight);

	// Create grid cells
	const int32 TotalCells = GridWidth * GridHeight;
	GridCells.Reserve(TotalCells);

	for (int32 Y = 0; Y < GridHeight; ++Y)
	{
		for (int32 X = 0; X < GridWidth; ++X)
		{
			// Create a new grid cell component
			FString ComponentName = FString::Printf(TEXT("GridCell_%d_%d"), X, Y);
			UGridCellComponent* CellComponent = NewObject<UGridCellComponent>(
				this,
				UGridCellComponent::StaticClass(),
				FName(*ComponentName)
			);

			if (CellComponent)
			{
				// Set grid coordinates
				CellComponent->GridX = X;
				CellComponent->GridY = Y;
				CellComponent->CurrentLODLevel = -1;
				CellComponent->CurrentFrame = -1;

				// Disable distance field generation to avoid runtime build hitches
				CellComponent->bAffectDistanceFieldLighting = false;
				CellComponent->bAffectDynamicIndirectLighting = false;

				// Disable collision - meshes are display only
				CellComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
				CellComponent->SetCollisionProfileName(TEXT("NoCollision"));

				// Disable shadow casting if not needed (optional - improves performance)
				CellComponent->SetCastShadow(false);

				// Register and attach to root
				CellComponent->RegisterComponent();
				CellComponent->AttachToComponent(RootComponent, FAttachmentTransformRules::KeepRelativeTransform);

				// Don't set position - meshes have positions baked in from the simulation
				// The mesh geometry itself is positioned in world space, so we keep the component at origin
				CellComponent->SetRelativeLocation(FVector::ZeroVector);

				UE_LOG(LogSurf, Log, TEXT("GridLODActor: Created cell [%d,%d] at origin (mesh has baked-in position)"),
					X, Y);

				// Add to array
				GridCells.Add(CellComponent);
			}
			else
			{
				UE_LOG(LogSurf, Error, TEXT("GridLODActor: Failed to create cell component [%d,%d]"), X, Y);
			}
		}
	}

	UE_LOG(LogSurf, Warning, TEXT("GridLODActor: Successfully initialized %d grid cells (%dx%d)"), GridCells.Num(), GridWidth, GridHeight);
}

void AGridLODActor::PreloadMeshesForFrameRange(int32 PreloadStartFrame, int32 PreloadEndFrame)
{
	if (!bEnableMeshStreaming)
	{
		return;
	}

	// Get selective preload list based on current LOD state
	TArray<FLODPreloadRequest> PreloadRequests = GetCellsNeedingPreload();

	int32 MeshesLoaded = 0;
	int32 AnimationLength = EndFrame - StartFrame + 1;

	// Calculate how many frames to preload (with wrapping)
	int32 FrameCount = PreloadEndFrame - PreloadStartFrame + 1;

	// Prune finished async handles so the array doesn't grow unbounded.
	ActivePreloadHandles.RemoveAll([](const TSharedPtr<FStreamableHandle>& H)
	{
		return !H.IsValid() || H->HasLoadCompleted() || H->WasCanceled();
	});

	// Cache keys for meshes we still need to load this pass (async batch below).
	TArray<FString> PendingCacheKeys;
	TArray<int32> PendingFrames;

	// Preload meshes for the specified frame range (with frame wrapping)
	for (int32 i = 0; i < FrameCount; ++i)
	{
		// Wrap frame within StartFrame..EndFrame range
		int32 Frame = PreloadStartFrame + i;
		if (Frame > EndFrame)
		{
			Frame = StartFrame + ((Frame - StartFrame) % AnimationLength);
		}

		// Skip if this frame is already fully cached
		if (CachedFrames.Contains(Frame))
		{
			continue;
		}

		// Apply frame rounding for each LOD level
		int32 HighResFrame = RoundFrameForLOD(Frame, 0);
		int32 LowResFrame = RoundFrameForLOD(Frame, 1);

		bool bFrameFullyResident = true;

		// Only load meshes for cells that need them (selective preloading)
		for (const FLODPreloadRequest& Request : PreloadRequests)
		{
			auto ConsiderMesh = [&](int32 InFrame, int32 InLOD)
			{
				const FString CacheKey = BuildMeshPath(Request.GridX, Request.GridY, InFrame, InLOD);
				if (MeshCache.Contains(CacheKey))
				{
					return; // already resident
				}
				bFrameFullyResident = false;

				if (!bUseAsyncPreload)
				{
					// Legacy synchronous path (blocks the game thread)
					LoadGridMesh(Request.GridX, Request.GridY, InFrame, InLOD);
					MeshesLoaded++;
					return;
				}

				// Async path: queue once, deduped against in-flight requests
				if (!InFlightMeshPaths.Contains(CacheKey))
				{
					InFlightMeshPaths.Add(CacheKey);
					PendingCacheKeys.Add(CacheKey);
					PendingFrames.AddUnique(Frame);
					MeshesLoaded++;
				}
			};

			if (Request.bNeedHighLOD) { ConsiderMesh(HighResFrame, 0); }
			if (Request.bNeedLowLOD)  { ConsiderMesh(LowResFrame, 1); }
		}

		// Synchronous path resolves immediately, so the frame is resident now.
		// Async path marks frames cached in OnAsyncMeshLoadComplete instead.
		if (!bUseAsyncPreload || bFrameFullyResident)
		{
			CachedFrames.Add(Frame);
		}
	}

	// Kick a single batched async request for everything we queued this pass.
	if (PendingCacheKeys.Num() > 0)
	{
		IssueAsyncMeshLoads(PendingCacheKeys, PendingFrames);
	}
}

FSoftObjectPath AGridLODActor::MeshPackagePathToObjectPath(const FString& PackagePath) const
{
	// BuildMeshPath returns a package path like "/Game/Waves/high_res/2_1_mesh_1025".
	// A soft object path needs the asset appended: ".../2_1_mesh_1025.2_1_mesh_1025".
	int32 SlashIndex = INDEX_NONE;
	FString AssetName = PackagePath;
	if (PackagePath.FindLastChar(TEXT('/'), SlashIndex))
	{
		AssetName = PackagePath.RightChop(SlashIndex + 1);
	}
	return FSoftObjectPath(PackagePath + TEXT(".") + AssetName);
}

void AGridLODActor::IssueAsyncMeshLoads(const TArray<FString>& CacheKeys, const TArray<int32>& FramesToMark)
{
	TArray<FSoftObjectPath> SoftPaths;
	SoftPaths.Reserve(CacheKeys.Num());
	for (const FString& Key : CacheKeys)
	{
		SoftPaths.Add(MeshPackagePathToObjectPath(Key));
	}

	TWeakObjectPtr<AGridLODActor> WeakThis(this);
	TSharedPtr<FStreamableHandle> Handle = StreamableManager.RequestAsyncLoad(
		SoftPaths,
		FStreamableDelegate::CreateLambda([WeakThis, CacheKeys, FramesToMark]()
		{
			if (AGridLODActor* Self = WeakThis.Get())
			{
				Self->OnAsyncMeshLoadComplete(CacheKeys, FramesToMark);
			}
		}),
		FStreamableManager::AsyncLoadHighPriority);

	if (Handle.IsValid())
	{
		ActivePreloadHandles.Add(Handle);
	}
}

void AGridLODActor::OnAsyncMeshLoadComplete(TArray<FString> CacheKeys, TArray<int32> FramesToMark)
{
	for (const FString& CacheKey : CacheKeys)
	{
		InFlightMeshPaths.Remove(CacheKey);

		if (MeshCache.Contains(CacheKey))
		{
			continue; // resolved by another path already
		}

		UStaticMesh* Mesh = Cast<UStaticMesh>(MeshPackagePathToObjectPath(CacheKey).ResolveObject());
		if (Mesh)
		{
			MeshCache.Add(CacheKey, Mesh);
		}
		else
		{
			UE_LOG(LogSurf, Warning, TEXT("GridLODActor '%s': async load completed but mesh did not resolve: '%s'"),
				*GetName(), *CacheKey);
		}
	}

	// A frame is considered cached once its queued meshes have landed. This is approximate
	// (a later batch may still add meshes for the same frame), but the per-mesh cache/in-flight
	// dedup keeps re-scans cheap, so imprecision only costs a harmless re-check.
	for (int32 Frame : FramesToMark)
	{
		CachedFrames.Add(Frame);
	}
}

bool AGridLODActor::RequestMeshLoadAsync(int32 X, int32 Y, int32 Frame, int32 LODLevel)
{
	const FString CacheKey = BuildMeshPath(X, Y, Frame, LODLevel);
	if (MeshCache.Contains(CacheKey))
	{
		return true; // already resident
	}

	// Queue once; CachedFrames bookkeeping is left to the frame-range preloader.
	if (!InFlightMeshPaths.Contains(CacheKey))
	{
		InFlightMeshPaths.Add(CacheKey);
		IssueAsyncMeshLoads({ CacheKey }, TArray<int32>());
	}
	return false;
}

void AGridLODActor::MaybeCaptureVerificationShots(int32 Frame)
{
	// Only run in an actual game world, and only when targets are configured.
	UWorld* World = GetWorld();
	if (!World || !World->IsGameWorld())
	{
		return;
	}

	// Targets are parsed once in BeginPlay (from -GridShots). Nothing to do if disabled.
	if (VerifyTargetFrames.Num() == 0)
	{
		return;
	}

	// Once every target has been captured, wait a few frames for the screenshots to flush, then quit.
	if (VerifyDoneFrames.Num() >= VerifyTargetFrames.Num())
	{
		if (VerifyQuitCountdown < 0)
		{
			VerifyQuitCountdown = 5;
		}
		else if (VerifyQuitCountdown == 0)
		{
			UE_LOG(LogSurf, Warning, TEXT("GridLODActor: verification capture complete - quitting."));
			FPlatformMisc::RequestExit(false);
			VerifyQuitCountdown = -2; // don't request again
		}
		else if (VerifyQuitCountdown > 0)
		{
			--VerifyQuitCountdown;
		}
		return;
	}

	// Fire a shot for any target the wave frame has reached and we haven't captured yet.
	for (int32 Target : VerifyTargetFrames)
	{
		if (Frame >= Target && !VerifyDoneFrames.Contains(Target))
		{
			VerifyDoneFrames.Add(Target);
			const FString ShotName = FString::Printf(TEXT("wave_%s_f%d"),
				bUseAsyncPreload ? TEXT("async") : TEXT("sync"), Target);
			FScreenshotRequest::RequestScreenshot(ShotName, /*bShowUI*/ false, /*bAddUniqueSuffix*/ false);
			UE_LOG(LogSurf, Warning, TEXT("GridLODActor: captured '%s' at wave frame %d"), *ShotName, Frame);
		}
	}
}

void AGridLODActor::PreloadMeshesSynchronous(int32 FirstFrame, int32 FrameCount)
{
	if (!bEnableMeshStreaming)
	{
		return;
	}

	UE_LOG(LogSurf, Warning, TEXT("GridLODActor: Starting synchronous preload of %d frames starting from %d (animation range: %d to %d)"),
		FrameCount, FirstFrame, StartFrame, EndFrame);

	int32 AnimationLength = EndFrame - StartFrame + 1;
	int32 MeshesLoaded = 0;

	// Load meshes using StaticLoadObject for truly synchronous loading (with frame wrapping)
	for (int32 i = 0; i < FrameCount; ++i)
	{
		// Wrap frame within StartFrame..EndFrame range
		int32 Frame = FirstFrame + i;
		if (Frame > EndFrame)
		{
			Frame = StartFrame + ((Frame - StartFrame) % AnimationLength);
		}

		for (UGridCellComponent* Cell : GridCells)
		{
			if (Cell)
			{
				// Apply frame rounding for each LOD level
				int32 HighResFrame = RoundFrameForLOD(Frame, 0);
				int32 LowResFrame = RoundFrameForLOD(Frame, 1);

				// Build paths for both LOD levels using rounded frames
				FString HighResPath = BuildMeshPath(Cell->GridX, Cell->GridY, HighResFrame, 0);
				FString LowResPath = BuildMeshPath(Cell->GridX, Cell->GridY, LowResFrame, 1);

				// Load high-res mesh
				UStaticMesh* HighResMesh = Cast<UStaticMesh>(StaticLoadObject(UStaticMesh::StaticClass(), nullptr, *HighResPath));
				if (HighResMesh)
				{
					// Wait for mesh to finish compiling/streaming
					HighResMesh->WaitForStreaming();
					MeshCache.Add(HighResPath, HighResMesh);
					MeshesLoaded++;
				}

				// Load low-res mesh
				UStaticMesh* LowResMesh = Cast<UStaticMesh>(StaticLoadObject(UStaticMesh::StaticClass(), nullptr, *LowResPath));
				if (LowResMesh)
				{
					// Wait for mesh to finish compiling/streaming
					LowResMesh->WaitForStreaming();
					MeshCache.Add(LowResPath, LowResMesh);
					MeshesLoaded++;
				}
			}
		}
	}

	// Mark frames as cached (with frame wrapping)
	for (int32 i = 0; i < FrameCount; ++i)
	{
		int32 Frame = FirstFrame + i;
		if (Frame > EndFrame)
		{
			Frame = StartFrame + ((Frame - StartFrame) % AnimationLength);
		}
		CachedFrames.Add(Frame);
	}

	UE_LOG(LogSurf, Warning, TEXT("GridLODActor: Synchronous preload complete - %d meshes loaded, cache now has %d meshes from %d frames"),
		MeshesLoaded, MeshCache.Num(), CachedFrames.Num());
}

void AGridLODActor::UnloadOldFrames(int32 ActiveFrame)
{
	if (!bEnableMeshStreaming)
	{
		return;
	}

	// Build list of frames to remove (frames outside the cache window)
	// Keep frames within [ActiveFrame - CacheFrameCount, ActiveFrame + PreloadFrameCount]
	TArray<int32> FramesToRemove;
	for (int32 CachedFrame : CachedFrames)
	{
		// Remove frames that are too far behind OR too far ahead
		if (CachedFrame < ActiveFrame - CacheFrameCount || CachedFrame > ActiveFrame + PreloadFrameCount)
		{
			FramesToRemove.Add(CachedFrame);
		}
	}

	// Remove old frames from cache
	for (int32 FrameToRemove : FramesToRemove)
	{
		// Remove all meshes for this frame
		for (int32 Y = 0; Y < GridHeight; ++Y)
		{
			for (int32 X = 0; X < GridWidth; ++X)
			{
				FString HighResPath = BuildMeshPath(X, Y, FrameToRemove, 0);
				FString LowResPath = BuildMeshPath(X, Y, FrameToRemove, 1);

				MeshCache.Remove(HighResPath);
				MeshCache.Remove(LowResPath);
			}
		}

		CachedFrames.Remove(FrameToRemove);
	}

	// if (FramesToRemove.Num() > 0)
	// {
	// 	UE_LOG(LogSurf, Log, TEXT("GridLODActor: Unloaded %d old frames. Cache now contains %d meshes from %d frames"),
	// 		FramesToRemove.Num(), MeshCache.Num(), CachedFrames.Num());
	// }
}

void AGridLODActor::UpdateGridMeshes()
{
	// Check if grid is initialized
	if (GridCells.Num() == 0)
	{
		static bool bLoggedOnce = false;
		if (!bLoggedOnce)
		{
			UE_LOG(LogSurf, Warning, TEXT("GridLODActor: UpdateGridMeshes called but grid has no cells. Call InitializeGrid first."));
			bLoggedOnce = true;
		}
		return;
	}

	// Get current frame from water controller (includes FrameOffset)
	const int32 Frame = GetCurrentFrameFromController();
	CurrentFrame = Frame;

	// Optional per-advance log of the WaterController's frame, gated by surf.debug.flags=frame.
	// Only the grid cell with FrameOffset==0 logs, so the value matches the WaterController's
	// raw CurrentFrame and we don't get one line per cell per tick.
	if (FrameOffset == 0)
	{
		static int32 LastLoggedFrame = INT_MIN;
		if (Frame != LastLoggedFrame && SurfDebug::IsFlagSet(TEXT("frame")))
		{
			UE_LOG(LogSurf, Display, TEXT("WaterFrame: %d"), Frame);
			LastLoggedFrame = Frame;
		}

		// Visual verification harness (no-op unless surf.grid.shots is set).
		MaybeCaptureVerificationShots(Frame);
	}

	// Phase 2: Mesh streaming - preload upcoming frames and unload old ones
	if (bEnableMeshStreaming && Frame != LastStreamedFrame)
	{
		// Preload current frame + next N frames
		int32 PreloadEnd = Frame + PreloadFrameCount;
		PreloadMeshesForFrameRange(Frame, PreloadEnd);

		// Unload old frames
		UnloadOldFrames(Frame);

		LastStreamedFrame = Frame;
	}

	// Get camera position
	FVector CameraPosition = FVector::ZeroVector;
	bool bFoundCamera = false;

	// Priority 1: Check if DebugCameraActor is set (for debugging in Simulate mode)
	if (DebugCameraActor)
	{
		CameraPosition = DebugCameraActor->GetActorLocation();
		bFoundCamera = true;
	}
	// Priority 2: Check PlayerCameraManager (runtime/play mode)
	else if (APlayerCameraManager* CameraManager = UGameplayStatics::GetPlayerCameraManager(GetWorld(), 0))
	{
		CameraPosition = CameraManager->GetCameraLocation();
		bFoundCamera = true;
	}
#if WITH_EDITOR
	// Priority 3: Try editor viewport camera (editor/simulate mode)
	else if (GEditor && GEditor->GetActiveViewport())
	{
		FViewport* ActiveViewport = GEditor->GetActiveViewport();
		if (ActiveViewport && ActiveViewport->GetClient())
		{
			FEditorViewportClient* ViewportClient = static_cast<FEditorViewportClient*>(ActiveViewport->GetClient());
			if (ViewportClient)
			{
				CameraPosition = ViewportClient->GetViewLocation();
				bFoundCamera = true;
			}
		}
	}
#endif

	if (!bFoundCamera)
	{
		static bool bLoggedOnce = false;
		if (!bLoggedOnce)
		{
			UE_LOG(LogSurf, Warning, TEXT("GridLODActor: No camera found - skipping LOD update"));
			bLoggedOnce = true;
		}
		return;
	}

	// Reset debug counters
	NumHighLODCells = 0;
	NumLowLODCells = 0;
	NumCulledCells = 0;

	// Update each grid cell
	for (UGridCellComponent* Cell : GridCells)
	{
		if (Cell)
		{
			UpdateGridCell(Cell, CameraPosition, Frame);
		}
	}

	// Log debug info periodically
	if (bShowDebugVisualization)
	{
		static float DebugLogTimer = 0.0f;
		static int32 LastHighCount = -1;
		static int32 LastLowCount = -1;
		DebugLogTimer += 0.016f; // Approximate frame time

		// Log whenever LOD distribution changes OR every 2 seconds
		bool bDistributionChanged = (NumHighLODCells != LastHighCount || NumLowLODCells != LastLowCount);
	}
}

UGridCellComponent* AGridLODActor::GetGridCell(int32 X, int32 Y) const
{
	if (X < 0 || X >= GridWidth || Y < 0 || Y >= GridHeight)
	{
		return nullptr;
	}

	const int32 Index = Y * GridWidth + X;
	if (Index >= 0 && Index < GridCells.Num())
	{
		return GridCells[Index];
	}

	return nullptr;
}

void AGridLODActor::RepositionToWorldLocation(FVector NewLocation, FVector2D NewWorldPositionOffset, int32 NewFrameOffset)
{
	// Update actor's world location
	SetActorLocation(NewLocation);

	// Update world position offset - shifts which meshes are displayed
	WorldPositionOffset = NewWorldPositionOffset;

	// Update frame offset for temporal variation
	FrameOffset = NewFrameOffset;

	// Force refresh all meshes with new position and frame offset
	UpdateGridMeshes();

	UE_LOG(LogSurf, Log, TEXT("GridLODActor: Repositioned to world location %s with offset %s and frame offset %d"),
		*NewLocation.ToString(), *NewWorldPositionOffset.ToString(), NewFrameOffset);
}

void AGridLODActor::PreloadAroundCurrentFrame(int32 FrameCount)
{
	const int32 Frame = GetCurrentFrameFromController();
	PreloadMeshesSynchronous(Frame, FrameCount);
	UpdateGridMeshes();
	UE_LOG(LogSurf, Display,
		TEXT("GridLODActor: preloaded %d frames around this tile's frame %d (FrameOffset=%d)"),
		FrameCount, Frame, FrameOffset);
}

int32 AGridLODActor::GetCurrentFrameFromController() const
{
	if (!WaterController)
	{
		return 0;
	}

	// Get the "Current Frame" property from the WaterController (same logic as MeshArrayActor)
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

			// Apply frame offset
			int32 OffsetFrame = CurrentFrameValue + FrameOffset;

			// Wrap around if we exceed the end frame (looping animation)
			if (EndFrame > StartFrame)
			{
				int32 FrameRange = EndFrame - StartFrame + 1;
				while (OffsetFrame > EndFrame)
				{
					OffsetFrame -= FrameRange;
				}
				while (OffsetFrame < StartFrame)
				{
					OffsetFrame += FrameRange;
				}
			}

			return OffsetFrame;
		}
	}

	return 0;
}

FVector AGridLODActor::GetGridCellCenterPosition(int32 X, int32 Y) const
{
	const UGridCellComponent* Cell = GetGridCell(X, Y);
	if (Cell)
	{
		FVector Position = Cell->GetComponentLocation();

		// Apply world position offset for infinite tiling
		Position.X += WorldPositionOffset.X;
		Position.Y += WorldPositionOffset.Y;

		return Position;
	}

	return GetActorLocation();
}

int32 AGridLODActor::GetLODLevelForDistance(float Distance, int32 CurrentLOD) const
{
	// Distance is already in the correct form (regular or squared) from UpdateGridCell
	// Apply hysteresis based on current LOD to prevent rapid switching

	float CompareThreshold = LODDistanceThreshold;
	float CompareHysteresis = LODHysteresis;

	if (bUseDistanceSquared)
	{
		// Distance is already squared, so square the thresholds too
		CompareThreshold = LODDistanceThreshold * LODDistanceThreshold;
		CompareHysteresis = LODHysteresis * LODHysteresis;
	}

	// Clamp hysteresis to be less than threshold (otherwise hysteresis math breaks)
	// This can happen when user sets a small threshold with larger hysteresis value
	if (CompareHysteresis >= CompareThreshold)
	{
		CompareHysteresis = CompareThreshold * 0.5f; // Use 50% of threshold as max hysteresis
	}

	// LOD Level -1 = Culled (hidden) - DISABLED FOR NOW
	// LOD Level 0 = High res
	// LOD Level 1 = Low res

	// Apply hysteresis:
	// - If currently high-LOD (0), stay high until distance > (threshold + hysteresis)
	// - If currently low-LOD (1), upgrade to high only when distance < (threshold - hysteresis)
	// - If no current LOD (-1), use simple threshold

	if (CurrentLOD == 0)
	{
		// Currently high-res - require distance to exceed threshold + hysteresis to downgrade
		if (Distance < CompareThreshold + CompareHysteresis)
		{
			return 0; // Stay high LOD
		}
		else
		{
			return 1; // Downgrade to low LOD
		}
	}
	else if (CurrentLOD == 1)
	{
		// Currently low-res - require distance to be less than threshold - hysteresis to upgrade
		if (Distance < CompareThreshold - CompareHysteresis)
		{
			return 0; // Upgrade to high LOD
		}
		else
		{
			return 1; // Stay low LOD
		}
	}
	else
	{
		// No current LOD (initial state or culled) - use simple threshold
		if (Distance < CompareThreshold)
		{
			return 0; // High LOD
		}
		else
		{
			return 1; // Low LOD
		}
	}
}

TArray<FLODPreloadRequest> AGridLODActor::GetCellsNeedingPreload() const
{
	TArray<FLODPreloadRequest> Requests;

	if (!bUseSelectivePreloading)
	{
		// Fallback: preload all cells with both LODs
		for (int32 Y = 0; Y < GridHeight; ++Y)
		{
			for (int32 X = 0; X < GridWidth; ++X)
			{
				FLODPreloadRequest Request;
				Request.GridX = X;
				Request.GridY = Y;
				Request.bNeedHighLOD = true;
				Request.bNeedLowLOD = true;
				Requests.Add(Request);
			}
		}
		return Requests;
	}

	// Track which cells are HIGH, LOW, or adjacent
	TSet<FIntPoint> HighLODCells;
	TSet<FIntPoint> LowLODCells;
	TSet<FIntPoint> AdjacentToHigh;
	TSet<FIntPoint> AdjacentToLow;

	// Pass 1: Identify current LOD states
	for (const UGridCellComponent* Cell : GridCells)
	{
		if (!Cell) continue;

		FIntPoint CellPos(Cell->GridX, Cell->GridY);

		if (Cell->CurrentLODLevel == 0)  // High LOD
		{
			HighLODCells.Add(CellPos);
		}
		else if (Cell->CurrentLODLevel == 1)  // Low LOD
		{
			LowLODCells.Add(CellPos);
		}
		// Culled cells (-1) are ignored
	}

	// Pass 2: Find adjacent cells
	const int32 MaxBorderDistance = 1 + PreloadBorderSize;

	for (const FIntPoint& HighCell : HighLODCells)
	{
		const int32 NeighborRange = bUse8ConnectedNeighbors ? MaxBorderDistance : MaxBorderDistance;

		for (int32 dx = -NeighborRange; dx <= NeighborRange; ++dx)
		{
			for (int32 dy = -NeighborRange; dy <= NeighborRange; ++dy)
			{
				if (dx == 0 && dy == 0) continue;

				// Skip diagonal neighbors if using 4-connected
				if (!bUse8ConnectedNeighbors && dx != 0 && dy != 0) continue;

				FIntPoint Neighbor(HighCell.X + dx, HighCell.Y + dy);

				// Check bounds
				if (Neighbor.X >= 0 && Neighbor.X < GridWidth &&
					Neighbor.Y >= 0 && Neighbor.Y < GridHeight)
				{
					AdjacentToHigh.Add(Neighbor);
				}
			}
		}
	}

	// Similar for Low LOD adjacent
	for (const FIntPoint& LowCell : LowLODCells)
	{
		const int32 NeighborRange = bUse8ConnectedNeighbors ? MaxBorderDistance : MaxBorderDistance;

		for (int32 dx = -NeighborRange; dx <= NeighborRange; ++dx)
		{
			for (int32 dy = -NeighborRange; dy <= NeighborRange; ++dy)
			{
				if (dx == 0 && dy == 0) continue;

				// Skip diagonal neighbors if using 4-connected
				if (!bUse8ConnectedNeighbors && dx != 0 && dy != 0) continue;

				FIntPoint Neighbor(LowCell.X + dx, LowCell.Y + dy);

				if (Neighbor.X >= 0 && Neighbor.X < GridWidth &&
					Neighbor.Y >= 0 && Neighbor.Y < GridHeight)
				{
					AdjacentToLow.Add(Neighbor);
				}
			}
		}
	}

	// Pass 3: Generate preload requests
	for (int32 Y = 0; Y < GridHeight; ++Y)
	{
		for (int32 X = 0; X < GridWidth; ++X)
		{
			FIntPoint CellPos(X, Y);
			FLODPreloadRequest Request;
			Request.GridX = X;
			Request.GridY = Y;
			Request.bNeedHighLOD = false;
			Request.bNeedLowLOD = false;

			// Rule 1: HIGH-RES preloading
			if (HighLODCells.Contains(CellPos) || AdjacentToHigh.Contains(CellPos))
			{
				Request.bNeedHighLOD = true;
			}

			// Rule 2: LOW-RES preloading
			if (LowLODCells.Contains(CellPos) ||
				AdjacentToLow.Contains(CellPos) ||
				HighLODCells.Contains(CellPos))  // HIGH might degrade to LOW
			{
				Request.bNeedLowLOD = true;
			}

			// Only add if at least one LOD is needed
			if (Request.bNeedHighLOD || Request.bNeedLowLOD)
			{
				Requests.Add(Request);
			}
		}
	}

	return Requests;
}

UStaticMesh* AGridLODActor::LoadGridMesh(int32 X, int32 Y, int32 Frame, int32 LODLevel) const
{
	// Build the path to the mesh (no offset applied - frame offset handles variation)
	FString MeshPath = BuildMeshPath(X, Y, Frame, LODLevel);

	// Phase 2: Check cache first if streaming is enabled
	if (bEnableMeshStreaming)
	{
		if (const TObjectPtr<UStaticMesh>* CachedMesh = MeshCache.Find(MeshPath))
		{
			return *CachedMesh;
		}
		else
		{
			// Mesh not in cache - load it on-demand using StreamableManager
			static TSet<FString> LoggedMissingMeshes;
			// if (!LoggedMissingMeshes.Contains(MeshPath))
			// {
			// 	UE_LOG(LogSurf, Warning, TEXT("GridLODActor: Mesh not in cache, loading on-demand with StreamableManager: '%s'"), *MeshPath);
			// 	LoggedMissingMeshes.Add(MeshPath);
			// }

			// Use StaticLoadObject for truly synchronous loading (forces actual data to load)
			// StaticLoadObject blocks until the asset is fully loaded (mesh data included)
			UStaticMesh* Mesh = Cast<UStaticMesh>(StaticLoadObject(UStaticMesh::StaticClass(), nullptr, *MeshPath));
			if (Mesh)
			{
				// Wait for mesh to finish compiling/streaming
				Mesh->WaitForStreaming();

				// Add to cache for future use (const_cast is needed since this is a const function)
				const_cast<AGridLODActor*>(this)->MeshCache.Add(MeshPath, Mesh);
			}
			else
			{
				UE_LOG(LogSurf, Error, TEXT("GridLODActor '%s': Failed to load mesh for cell (%d,%d) frame %d LOD %d - Path: '%s'"), *GetName(), X, Y, Frame, LODLevel, *MeshPath);
			}
			return Mesh;
		}
	}
	else
	{
		// Phase 1: Direct loading using StaticLoadObject (no caching)
		UStaticMesh* Mesh = Cast<UStaticMesh>(StaticLoadObject(UStaticMesh::StaticClass(), nullptr, *MeshPath));

		// Log success/failure (only once per unique mesh path)
		static TSet<FString> LoggedMeshPaths;
		if (!LoggedMeshPaths.Contains(MeshPath))
		{
			if (Mesh)
			{
				UE_LOG(LogSurf, Log, TEXT("GridLODActor: Successfully loaded mesh '%s'"), *MeshPath);
			}
			else
			{
				UE_LOG(LogSurf, Error, TEXT("GridLODActor: Failed to load mesh '%s'"), *MeshPath);
			}
			LoggedMeshPaths.Add(MeshPath);
		}

		return Mesh;
	}
}

int32 AGridLODActor::RoundFrameForLOD(int32 Frame, int32 LODLevel) const
{
	int32 ActualFrame = Frame;

	if (LODLevel == 0)
	{
		// High-res: round to nearest even frame
		if (Frame % 2 != 0)
		{
			ActualFrame = Frame - 1; // Round down to previous even frame
		}
	}
	else if (LODLevel == 1)
	{
		// Low-res: round to nearest frame divisible by 4
		int32 Remainder = Frame % 4;
		if (Remainder != 0)
		{
			// Round down to nearest frame divisible by 4
			ActualFrame = Frame - Remainder;
		}
	}

	// Ensure ActualFrame stays within valid range [StartFrame, EndFrame]
	// If rounding went below StartFrame, round up instead
	if (ActualFrame < StartFrame)
	{
		if (LODLevel == 0)
		{
			// High-res: find next even frame >= StartFrame
			ActualFrame = StartFrame;
			if (ActualFrame % 2 != 0)
			{
				ActualFrame = StartFrame + 1;
			}
		}
		else if (LODLevel == 1)
		{
			// Low-res: find next frame divisible by 4, >= StartFrame
			ActualFrame = StartFrame;
			int32 Remainder = ActualFrame % 4;
			if (Remainder != 0)
			{
				ActualFrame = StartFrame + (4 - Remainder);
			}
		}
	}

	// Also check if rounding went above EndFrame (shouldn't happen with rounding down, but be safe)
	if (ActualFrame > EndFrame)
	{
		if (LODLevel == 0)
		{
			// High-res: find previous even frame <= EndFrame
			ActualFrame = EndFrame;
			if (ActualFrame % 2 != 0)
			{
				ActualFrame = EndFrame - 1;
			}
		}
		else if (LODLevel == 1)
		{
			// Low-res: find previous frame divisible by 4, <= EndFrame
			ActualFrame = EndFrame;
			int32 Remainder = ActualFrame % 4;
			ActualFrame = EndFrame - Remainder;
		}
	}

	return ActualFrame;
}

FString AGridLODActor::BuildMeshPath(int32 X, int32 Y, int32 Frame, int32 LODLevel) const
{
	// Determine LOD folder name
	FString LODFolder;
	if (LODLevel == 0)
	{
		LODFolder = HighResFolderName;
	}
	else if (LODLevel == 1)
	{
		LODFolder = LowResFolderName;
	}
	else
	{
		UE_LOG(LogSurf, Error, TEXT("GridLODActor: Invalid LOD level %d"), LODLevel);
		return TEXT("");
	}

	// Build path: /Game/Waves/chunks_ratio_0_03/2_1_mesh_1025
	// Mesh naming: {x}_{y}_mesh_{frame}.obj -> Asset name: {x}_{y}_mesh_{frame}
	FString MeshName = FString::Printf(TEXT("%d_%d_mesh_%d"), X, Y, Frame);
	FString FullPath = FString::Printf(TEXT("%s/%s/%s"),
		*BaseMeshFolderPath,
		*LODFolder,
		*MeshName);

	return FullPath;
}

void AGridLODActor::UpdateGridCell(UGridCellComponent* Cell, const FVector& CameraPosition, int32 Frame)
{
	if (!Cell)
	{
		return;
	}

	// Calculate distance from camera to CLOSEST POINT on cell bounds
	// This is critical for large meshes (like combined edge chunks) - using bounds center
	// would give incorrect distances when camera is above part of a large mesh

	// Get mesh bounds - if not available, load a mesh first
	FBoxSphereBounds CellBounds = Cell->Bounds;

	if (CellBounds.BoxExtent.IsNearlyZero())
	{
		// Load low-res mesh to get bounds (cheaper than high-res)
		int32 LowResFrame = RoundFrameForLOD(Frame, 1);
		UStaticMesh* ReferenceMesh = LoadGridMesh(Cell->GridX, Cell->GridY, LowResFrame, 1);

		if (ReferenceMesh)
		{
			// Temporarily set mesh to get bounds
			Cell->SetStaticMesh(ReferenceMesh);
			Cell->CurrentLODLevel = 1;
			Cell->CurrentFrame = LowResFrame;

			// Now bounds should be available
			CellBounds = Cell->Bounds;

			// Apply material if one is set
			if (GridMaterial)
			{
				Cell->SetMaterial(0, GridMaterial);
			}
		}
		else
		{
			// If we can't load mesh, hide the cell and return early
			Cell->SetVisibility(false);
			Cell->CurrentLODLevel = -1;
			NumCulledCells++;
			return;
		}
	}

	// Calculate closest point on bounding box to camera
	// This handles large meshes correctly - distance is to the nearest part of the mesh
	FVector BoundsMin = CellBounds.Origin - CellBounds.BoxExtent;
	FVector BoundsMax = CellBounds.Origin + CellBounds.BoxExtent;

	FVector ClosestPoint;
	ClosestPoint.X = FMath::Clamp(CameraPosition.X, BoundsMin.X, BoundsMax.X);
	ClosestPoint.Y = FMath::Clamp(CameraPosition.Y, BoundsMin.Y, BoundsMax.Y);
	ClosestPoint.Z = FMath::Clamp(CameraPosition.Z, BoundsMin.Z, BoundsMax.Z);

	float Distance;

	if (bUse2DDistance)
	{
		// 2D distance to closest point (ignore Z axis)
		const FVector2D CameraPos2D(CameraPosition.X, CameraPosition.Y);
		const FVector2D ClosestPoint2D(ClosestPoint.X, ClosestPoint.Y);

		if (bUseDistanceSquared)
		{
			Distance = FVector2D::DistSquared(CameraPos2D, ClosestPoint2D);
		}
		else
		{
			Distance = FVector2D::Distance(CameraPos2D, ClosestPoint2D);
		}
	}
	else
	{
		// 3D distance to closest point
		if (bUseDistanceSquared)
		{
			Distance = FVector::DistSquared(CameraPosition, ClosestPoint);
		}
		else
		{
			Distance = FVector::Distance(CameraPosition, ClosestPoint);
		}
	}

	// Special handling for edge chunks (0 and 2) which are in the same non-contiguous mesh file
	// These chunks should use distance-based LOD like normal UNLESS the camera is over the middle chunk (1)
	// This prevents edge chunks from being high-res when you're looking at the breaking wave
	bool bIsEdgeChunk = (Cell->GridX == 0 || Cell->GridX == 2);
	bool bCameraOverMiddleChunk = false;

	if (bIsEdgeChunk && !ForceLowLODCells.IsEmpty())
	{
		// Check if ForceLowLODCells contains this GridX (legacy behavior - always force low)
		TArray<FString> ForcedIndices;
		ForceLowLODCells.ParseIntoArray(ForcedIndices, TEXT(","), true);

		for (const FString& IndexStr : ForcedIndices)
		{
			int32 ForcedIndex = FCString::Atoi(*IndexStr.TrimStartAndEnd());
			if (Cell->GridX == ForcedIndex)
			{
				// Check if camera is over middle chunk (GridX=1)
				const UGridCellComponent* MiddleChunk = GetGridCell(1, 0);
				if (MiddleChunk && MiddleChunk->Bounds.BoxExtent.SizeSquared() > 0.0f)
				{
					FBoxSphereBounds MiddleBounds = MiddleChunk->Bounds;
					FVector MiddleBoundsMin = MiddleBounds.Origin - MiddleBounds.BoxExtent;
					FVector MiddleBoundsMax = MiddleBounds.Origin + MiddleBounds.BoxExtent;

					// Check if camera is inside middle chunk bounds (2D check)
					if (bUse2DDistance)
					{
						bCameraOverMiddleChunk = (CameraPosition.X >= MiddleBoundsMin.X && CameraPosition.X <= MiddleBoundsMax.X &&
						                          CameraPosition.Y >= MiddleBoundsMin.Y && CameraPosition.Y <= MiddleBoundsMax.Y);
					}
					else
					{
						bCameraOverMiddleChunk = (CameraPosition.X >= MiddleBoundsMin.X && CameraPosition.X <= MiddleBoundsMax.X &&
						                          CameraPosition.Y >= MiddleBoundsMin.Y && CameraPosition.Y <= MiddleBoundsMax.Y &&
						                          CameraPosition.Z >= MiddleBoundsMin.Z && CameraPosition.Z <= MiddleBoundsMax.Z);
					}
				}
				break;
			}
		}
	}

	int32 LODLevel;

	// Calculate what LOD level distance suggests for this edge chunk
	int32 DistanceBasedLOD = GetLODLevelForDistance(Distance, Cell->CurrentLODLevel);

	// Special handling: force edge chunks to low-LOD when camera is deep inside middle chunk
	// We need to check if camera is far from the SEAM (edge/border of middle chunk)
	if (bIsEdgeChunk && bCameraOverMiddleChunk)
	{
		// Calculate distance to the nearest edge/seam of the middle chunk
		const UGridCellComponent* MiddleChunk = GetGridCell(1, 0);
		if (MiddleChunk)
		{
			FBoxSphereBounds MiddleBounds = MiddleChunk->Bounds;
			FVector MiddleBoundsMin = MiddleBounds.Origin - MiddleBounds.BoxExtent;
			FVector MiddleBoundsMax = MiddleBounds.Origin + MiddleBounds.BoxExtent;

			// Calculate distance to the nearest edge/border of middle chunk
			// If camera is at (X, Y) and middle chunk bounds are [MinX, MaxX] x [MinY, MaxY]:
			// Distance to left edge = X - MinX
			// Distance to right edge = MaxX - X
			// We want the minimum of these
			float DistanceToSeam;
			if (bUse2DDistance)
			{
				// For 2D, calculate distance to nearest edge
				float DistToLeftEdge = CameraPosition.X - MiddleBoundsMin.X;
				float DistToRightEdge = MiddleBoundsMax.X - CameraPosition.X;
				float DistToBottomEdge = CameraPosition.Y - MiddleBoundsMin.Y;
				float DistToTopEdge = MiddleBoundsMax.Y - CameraPosition.Y;

				DistanceToSeam = FMath::Min(FMath::Min(DistToLeftEdge, DistToRightEdge),
				                             FMath::Min(DistToBottomEdge, DistToTopEdge));

				if (bUseDistanceSquared)
				{
					DistanceToSeam = DistanceToSeam * DistanceToSeam;
				}
			}
			else
			{
				// For 3D, similar logic
				float DistToLeft = CameraPosition.X - MiddleBoundsMin.X;
				float DistToRight = MiddleBoundsMax.X - CameraPosition.X;
				float DistToBottom = CameraPosition.Y - MiddleBoundsMin.Y;
				float DistToTop = MiddleBoundsMax.Y - CameraPosition.Y;
				float DistToLower = CameraPosition.Z - MiddleBoundsMin.Z;
				float DistToUpper = MiddleBoundsMax.Z - CameraPosition.Z;

				DistanceToSeam = FMath::Min(FMath::Min(DistToLeft, DistToRight),
				                 FMath::Min(FMath::Min(DistToBottom, DistToTop),
				                 FMath::Min(DistToLower, DistToUpper)));

				if (bUseDistanceSquared)
				{
					DistanceToSeam = DistanceToSeam * DistanceToSeam;
				}
			}

			// Check if camera is far from the seam (deep inside middle chunk)
			int32 SeamLOD = GetLODLevelForDistance(DistanceToSeam, -1);

			if (SeamLOD == 1)
			{
				// Camera is far from seam (deep in middle chunk) - force edge to low-LOD
				LODLevel = 1;
			}
			else
			{
				// Camera is close to seam - allow edge to use distance-based LOD
				LODLevel = DistanceBasedLOD;
			}
		}
		else
		{
			LODLevel = DistanceBasedLOD;
		}
	}
	else
	{
		// Use distance-based LOD in all other cases:
		// - Middle chunk (GridX=1) always uses distance-based
		// - Edge chunks when camera NOT over middle chunk
		LODLevel = DistanceBasedLOD;
	}

	// DETAILED DEBUG: Log ALL cells when one cell's LOD changes
	if (bShowDebugVisualization && Cell->CurrentLODLevel != LODLevel)
	{
		float CompareThreshold = LODDistanceThreshold;
		float CompareHysteresis = LODHysteresis;
		if (bUseDistanceSquared)
		{
			CompareThreshold = LODDistanceThreshold * LODDistanceThreshold;
			CompareHysteresis = LODHysteresis * LODHysteresis;
		}

		// A cell changed LOD - log all cells for this actor to see why
		if (bEnableLODLogging)
		{
			UE_LOG(LogSurf, Warning, TEXT("=== GridLODActor '%s' LOD CHANGE DETECTED (CameraOverMiddle=%s) ==="),
				*GetName(), bCameraOverMiddleChunk ? TEXT("YES") : TEXT("NO"));
		}

		if (bEnableLODLogging)
		{
		for (int32 X = 0; X < GridWidth; ++X)
		{
			const UGridCellComponent* DebugCell = GetGridCell(X, 0);
			if (DebugCell)
			{
				// Calculate closest point on bounds (same logic as main code)
				FBoxSphereBounds DebugBounds = DebugCell->Bounds;
				FVector DebugBoundsMin = DebugBounds.Origin - DebugBounds.BoxExtent;
				FVector DebugBoundsMax = DebugBounds.Origin + DebugBounds.BoxExtent;

				FVector DebugClosestPoint;
				DebugClosestPoint.X = FMath::Clamp(CameraPosition.X, DebugBoundsMin.X, DebugBoundsMax.X);
				DebugClosestPoint.Y = FMath::Clamp(CameraPosition.Y, DebugBoundsMin.Y, DebugBoundsMax.Y);
				DebugClosestPoint.Z = FMath::Clamp(CameraPosition.Z, DebugBoundsMin.Z, DebugBoundsMax.Z);

				float DebugDist;
				if (bUse2DDistance)
				{
					const FVector2D CameraPos2D(CameraPosition.X, CameraPosition.Y);
					const FVector2D ClosestPoint2D(DebugClosestPoint.X, DebugClosestPoint.Y);
					DebugDist = bUseDistanceSquared ? FVector2D::DistSquared(CameraPos2D, ClosestPoint2D) : FVector2D::Distance(CameraPos2D, ClosestPoint2D);
				}
				else
				{
					DebugDist = bUseDistanceSquared ? FVector::DistSquared(CameraPosition, DebugClosestPoint) : FVector::Distance(CameraPosition, DebugClosestPoint);
				}

				int32 DebugLOD = GetLODLevelForDistance(DebugDist, DebugCell->CurrentLODLevel);

				// Determine final LOD and reason (matching main logic)
				bool bDebugIsEdge = (X == 0 || X == 2);
				int32 FinalDebugLOD = DebugLOD;
				FString ExtraInfo = TEXT("");

				if (bDebugIsEdge && bCameraOverMiddleChunk)
				{
					// Check distance to seam to see if edge should be forced low
					const UGridCellComponent* DebugMiddleChunk = GetGridCell(1, 0);
					if (DebugMiddleChunk)
					{
						FBoxSphereBounds DebugMiddleBounds = DebugMiddleChunk->Bounds;
						FVector DebugMiddleBoundsMin = DebugMiddleBounds.Origin - DebugMiddleBounds.BoxExtent;
						FVector DebugMiddleBoundsMax = DebugMiddleBounds.Origin + DebugMiddleBounds.BoxExtent;

						float DebugDistToSeam;
						if (bUse2DDistance)
						{
							float DistToLeft = CameraPosition.X - DebugMiddleBoundsMin.X;
							float DistToRight = DebugMiddleBoundsMax.X - CameraPosition.X;
							float DistToBottom = CameraPosition.Y - DebugMiddleBoundsMin.Y;
							float DistToTop = DebugMiddleBoundsMax.Y - CameraPosition.Y;

							DebugDistToSeam = FMath::Min(FMath::Min(DistToLeft, DistToRight),
							                              FMath::Min(DistToBottom, DistToTop));
							if (bUseDistanceSquared)
							{
								DebugDistToSeam = DebugDistToSeam * DebugDistToSeam;
							}
						}
						else
						{
							float DistToLeft = CameraPosition.X - DebugMiddleBoundsMin.X;
							float DistToRight = DebugMiddleBoundsMax.X - CameraPosition.X;
							float DistToBottom = CameraPosition.Y - DebugMiddleBoundsMin.Y;
							float DistToTop = DebugMiddleBoundsMax.Y - CameraPosition.Y;
							float DistToLower = CameraPosition.Z - DebugMiddleBoundsMin.Z;
							float DistToUpper = DebugMiddleBoundsMax.Z - CameraPosition.Z;

							DebugDistToSeam = FMath::Min(FMath::Min(DistToLeft, DistToRight),
							                  FMath::Min(FMath::Min(DistToBottom, DistToTop),
							                  FMath::Min(DistToLower, DistToUpper)));
							if (bUseDistanceSquared)
							{
								DebugDistToSeam = DebugDistToSeam * DebugDistToSeam;
							}
						}

						int32 SeamLOD = GetLODLevelForDistance(DebugDistToSeam, -1);

						if (SeamLOD == 1)
						{
							FinalDebugLOD = 1; // Forced low
							ExtraInfo = FString::Printf(TEXT(" [FORCED LOW - far from seam, SeamDist=%.1f]"), DebugDistToSeam);
						}
						else
						{
							ExtraInfo = FString::Printf(TEXT(" [EDGE HIGH - near seam, SeamDist=%.1f]"), DebugDistToSeam);
						}
					}
				}

				UE_LOG(LogSurf, Warning, TEXT("  Cell[%d,%d]: CamPos=(%.1f,%.1f) ClosestPt=(%.1f,%.1f) BoundsCenter=(%.1f,%.1f) Dist=%.2f Thresh=%.2f±%.2f => LOD=%d (was %d)%s"),
					X, 0,
					CameraPosition.X, CameraPosition.Y,
					DebugClosestPoint.X, DebugClosestPoint.Y,
					DebugBounds.Origin.X, DebugBounds.Origin.Y,
					DebugDist, CompareThreshold, CompareHysteresis,
					FinalDebugLOD,
					DebugCell->CurrentLODLevel,
					*ExtraInfo);
			}
		}
		} // End bEnableLODLogging
	}

	// Handle culling (LOD level -1)
	if (LODLevel == -1)
	{
		Cell->SetVisibility(false);
		Cell->CurrentLODLevel = -1;
		NumCulledCells++;
		return;
	}

	// Make sure cell is visible
	Cell->SetVisibility(true);

	// Update debug counters
	if (LODLevel == 0)
	{
		NumHighLODCells++;
	}
	else if (LODLevel == 1)
	{
		NumLowLODCells++;
	}

	// Round frame to nearest available frame for this LOD level
	int32 ActualFrame = RoundFrameForLOD(Frame, LODLevel);

	// Check if we need to update the mesh (LOD or actual frame changed)
	bool bNeedMeshUpdate = (Cell->CurrentLODLevel != LODLevel || Cell->CurrentFrame != ActualFrame);

	if (bNeedMeshUpdate)
	{
		// Load and set the new mesh
		UStaticMesh* NewMesh = nullptr;
		bool bKeepPrevious = false;

		if (bUseAsyncPreload)
		{
			// Prefer a cached mesh. If it isn't resident yet, request it asynchronously and keep
			// the currently displayed mesh for a frame or two (no game-thread stall). Only fall
			// back to a blocking load on a cell's very first appearance, so it's never left empty.
			const FString CacheKey = BuildMeshPath(Cell->GridX, Cell->GridY, ActualFrame, LODLevel);
			if (const TObjectPtr<UStaticMesh>* Cached = MeshCache.Find(CacheKey))
			{
				NewMesh = *Cached;
			}
			else
			{
				RequestMeshLoadAsync(Cell->GridX, Cell->GridY, ActualFrame, LODLevel);
				if (Cell->GetStaticMesh() != nullptr)
				{
					bKeepPrevious = true;
				}
				else
				{
					NewMesh = LoadGridMesh(Cell->GridX, Cell->GridY, ActualFrame, LODLevel);
				}
			}
		}
		else
		{
			NewMesh = LoadGridMesh(Cell->GridX, Cell->GridY, ActualFrame, LODLevel);
		}

		if (bKeepPrevious)
		{
			// Async load pending — leave the cell showing its previous mesh this frame.
		}
		else if (NewMesh)
		{
			Cell->SetStaticMesh(NewMesh);
			Cell->CurrentLODLevel = LODLevel;
			Cell->CurrentFrame = ActualFrame;

			// Apply material if one is set
			if (GridMaterial)
			{
				Cell->SetMaterial(0, GridMaterial);
			}
		}
		else
		{
			// Mesh failed to load, hide the cell
			FString MeshPath = BuildMeshPath(Cell->GridX, Cell->GridY, ActualFrame, LODLevel);
			UE_LOG(LogSurf, Error, TEXT("GridLODActor '%s': Failed to load mesh for cell (%d,%d) frame %d LOD %d - Path: '%s'"),
				*GetName(), Cell->GridX, Cell->GridY, ActualFrame, LODLevel, *MeshPath);
			Cell->SetVisibility(false);
			Cell->CurrentLODLevel = -1;
			NumCulledCells++;
		}
	}  // End of bNeedMeshUpdate block

	// Debug visualization - draw colored boxes EVERY FRAME to show current LOD levels
	if (bShowDebugVisualization && Cell->GetStaticMesh())
	{
		FColor DebugColor;
		if (Cell->CurrentLODLevel == 0)
		{
			DebugColor = FColor::Green;  // High LOD = Green
		}
		else if (Cell->CurrentLODLevel == 1)
		{
			DebugColor = FColor::Yellow;  // Low LOD = Yellow
		}
		else
		{
			DebugColor = FColor::Red;  // Hidden/Failed = Red
		}

		// Get the mesh bounds in world space
		FBoxSphereBounds WorldBounds = Cell->Bounds;
		FVector CellCenter = WorldBounds.Origin;
		FVector CellExtent = WorldBounds.BoxExtent;

		// Draw a box around the cell bounds (persistent = every frame)
		DrawDebugBox(GetWorld(), CellCenter, CellExtent, DebugColor, false, 0.0f, 0, 10.0f);

		// Draw text showing LOD level and grid coordinates
		FString DebugText = FString::Printf(TEXT("[%d,%d] LOD%d"), Cell->GridX, Cell->GridY, Cell->CurrentLODLevel);
		DrawDebugString(GetWorld(), CellCenter + FVector(0, 0, CellExtent.Z + 100), DebugText, nullptr, DebugColor, 0.0f, true, 3.0f);
	}
}

void AGridLODActor::UpdateSeamBlendParameters()
{
	if (!bEnableSeamBlending || !WaveHeightActor || !SeamBlendingMPC)
	{
		return;
	}

	// Cast to AWaveHeight to access seam properties
	AWaveHeight* WaveHeight = Cast<AWaveHeight>(WaveHeightActor);
	if (!WaveHeight)
	{
		UE_LOG(LogSurf, Warning, TEXT("GridLODActor: WaveHeightActor is not of type AWaveHeight"));
		return;
	}

	// Get MPC instance
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// Update Material Parameter Collection with WaveHeight actor's transform and parameters
	FVector WaveHeightLocation = WaveHeight->GetActorLocation();
	FRotator WaveHeightRotation = WaveHeight->GetActorRotation();

	// Set WaveHeight actor transform parameters
	UKismetMaterialLibrary::SetScalarParameterValue(World, SeamBlendingMPC, FName("WaveHeightActorX"), WaveHeightLocation.X);
	UKismetMaterialLibrary::SetScalarParameterValue(World, SeamBlendingMPC, FName("WaveHeightActorY"), WaveHeightLocation.Y);
	UKismetMaterialLibrary::SetScalarParameterValue(World, SeamBlendingMPC, FName("WaveHeightActorZ"), WaveHeightLocation.Z);
	UKismetMaterialLibrary::SetScalarParameterValue(World, SeamBlendingMPC, FName("WaveHeightRotationYaw"), WaveHeightRotation.Yaw);

	// Set WaveHeight coordinate parameters (access public properties)
	UKismetMaterialLibrary::SetScalarParameterValue(World, SeamBlendingMPC, FName("CoordinateScale"), WaveHeight->CoordinateScale.X);
	UKismetMaterialLibrary::SetScalarParameterValue(World, SeamBlendingMPC, FName("BaseOffsetX"), WaveHeight->BaseOffsetX);
	UKismetMaterialLibrary::SetScalarParameterValue(World, SeamBlendingMPC, FName("BaseOffsetY"), WaveHeight->BaseOffsetY);
	UKismetMaterialLibrary::SetScalarParameterValue(World, SeamBlendingMPC, FName("WorldOffsetPerTileX"), WaveHeight->WorldOffsetPerTileX);
	UKismetMaterialLibrary::SetScalarParameterValue(World, SeamBlendingMPC, FName("WorldOffsetPerTileY"), WaveHeight->WorldOffsetPerTileY);

	// Get seam boundaries from WaveHeight
	float SeamMinX, SeamMaxX;
	WaveHeight->GetSeamBoundaries(SeamMinX, SeamMaxX);
	UKismetMaterialLibrary::SetScalarParameterValue(World, SeamBlendingMPC, FName("SeamMinX"), SeamMinX);
	UKismetMaterialLibrary::SetScalarParameterValue(World, SeamBlendingMPC, FName("SeamMaxX"), SeamMaxX);

	// Get tiling and grid parameters from WaveHeight metadata
	if (WaveHeight->waveUnifiedMetadata)
	{
		auto* MetadataRow = WaveHeight->waveUnifiedMetadata->FindRow<FWaveUnifiedMetadata>("Metadata", "");
		if (MetadataRow)
		{
			UKismetMaterialLibrary::SetScalarParameterValue(World, SeamBlendingMPC, FName("TilingX"), MetadataRow->tiling_x);
			UKismetMaterialLibrary::SetScalarParameterValue(World, SeamBlendingMPC, FName("GridStartX"), MetadataRow->grid_start_x);
			UKismetMaterialLibrary::SetScalarParameterValue(World, SeamBlendingMPC, FName("SeamBlendWidth"), 2.0f); // Default blend width
		}
		else
		{
			UE_LOG(LogSurf, Warning, TEXT("GridLODActor '%s': WaveHeight metadata row not found!"), *GetName());
		}
	}
	else
	{
		UE_LOG(LogSurf, Warning, TEXT("GridLODActor '%s': WaveHeight has no waveUnifiedMetadata!"), *GetName());
	}
}

void AGridLODActor::ClearGrid()
{
	// Destroy all grid cell components
	for (UGridCellComponent* Cell : GridCells)
	{
		if (Cell)
		{
			Cell->DestroyComponent();
		}
	}

	GridCells.Empty();
}
