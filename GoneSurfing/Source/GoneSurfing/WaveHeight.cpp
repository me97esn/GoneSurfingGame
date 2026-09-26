// Fill out your copyright notice in the Description page of Project Settings.

#include "WaveHeight.h"
#include "SurfLog.h"
#include "SurfDebug.h"
#include <vector>
#include <complex>
#include <cmath>
#define _USE_MATH_DEFINES
#include <math.h>
using namespace std;
#include "Runtime/Engine/Classes/Components/SceneComponent.h"
#include "DrawDebugHelpers.h"
#include "Kismet/GameplayStatics.h"
#include "GridLODActor.h"
#include "InfiniteWaveManager.h"
#include "EngineUtils.h"
#include "Misc/FileHelper.h"
#include "HAL/IConsoleManager.h"

#if !UE_BUILD_SHIPPING
// Path of a recorded input trace (Saved/InputTraces/phone-*.csv) to annotate with the water surface
// under the board. The trace records the board's own pos/vel/attitude and the wave clock but NOT the
// water height, so "did it fall through the surface or ride down the face" cannot be answered from
// the trace alone. The probe re-samples the same wave data at each row's (x, y, wave_frame) — the
// sampler is a pure function of position + frame, so a PC run reproduces the phone's surface exactly —
// and writes <trace>.water.csv with wave_z, depth and the surface normal beside the recorded board_z.
// One-shot on the first tick with data loaded; pass via -ExecCmds="surf.waveprobe '<path>[;<path>...]'".
static TAutoConsoleVariable<FString> CVarWaveProbe(
	TEXT("surf.waveprobe"), TEXT(""),
	TEXT("Trace CSV to annotate with the water surface under each recorded board position (writes <trace>.water.csv)."),
	ECVF_Default);
#endif

using Complex = complex<double>;
using ComplexMatrix = vector<vector<Complex>>;

AWaveHeight::AWaveHeight()
{
     UE_LOG(LogSurf, Log, TEXT("Wave height constructor"));
    auto SceneComponent = CreateDefaultSubobject<USceneComponent>(TEXT("SceneComponent"));
    SetRootComponent(SceneComponent);

    // Set this actor to call Tick() every frame for debug visualization
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.bStartWithTickEnabled = true;
}

// Called when the game starts or when spawned
void AWaveHeight::BeginPlay()
{
    Super::BeginPlay();

    // A/B switch for the mesh-registered tiling: -WaveDeriveTiling=0 rides the authored umap values
    // (the pre-2026-09-17 wave), =1 forces derivation. A command-line param rather than a CVar because
    // -ExecCmds runs after the first level's BeginPlay, too late for this (specs/wave-mesh-data-
    // registration.md, T2: replay the same trace on both waves).
    int32 DeriveOverride = -1;
    if (FParse::Value(FCommandLine::Get(), TEXT("WaveDeriveTiling="), DeriveOverride) && DeriveOverride >= 0)
    {
        bDeriveTilingFromWaveManager = (DeriveOverride != 0);
        UE_LOG(LogSurf, Warning, TEXT("WaveHeight '%s': -WaveDeriveTiling=%d overrides bDeriveTilingFromWaveManager -> %s"),
            *GetName(), DeriveOverride, bDeriveTilingFromWaveManager ? TEXT("derive") : TEXT("authored"));
    }

    // Hardcoded VelocityScale. Set here rather than via UPROPERTY because reducing it
    // is what stops the post-pop-up over-rotation / skid: at 1000 the front-bottom-actor
    // sideways-drag yaw torque outweighs the lateral force on the velocity, so the board
    // out-rotates its velocity vector by ~20° around t=8s. Sweep on surfing-down-the-line:
    //   600 → board barely turns (end yaw -36°, speed 934)
    //   700 → less than full turn (end yaw -117°, late skid 10°)
    //   800 → BEST: end yaw -138°, late skid 10°, speed 662
    //   900 → late skid 17°
    //  1000 → previous baseline, late skid ~20°
    //  1500 → planing never engages, board doesn't surf
    // See specs/fin-carve-coupling.md and the AC2 follow-up investigation in this
    // session's commit history for the full reasoning.
    const float kHardcodedVelocityScale = 800.0f;
    this->VelocityScale = FVector(kHardcodedVelocityScale, kHardcodedVelocityScale, kHardcodedVelocityScale);
    UE_LOG(LogSurf, Warning, TEXT("WaveHeight '%s': VelocityScale HARDCODED to %.1f"), *GetName(), kHardcodedVelocityScale);

    // Verify DataTables are assigned
    if (!waveUnifiedMetadata)
    {
        UE_LOG(LogSurf, Error, TEXT("WaveHeight '%s': waveUnifiedMetadata is not set!"), *GetName());
    }
    if (!waveUnifiedData)
    {
        UE_LOG(LogSurf, Error, TEXT("WaveHeight '%s': waveUnifiedData is not set!"), *GetName());
    }

    if (bDeriveTilingFromWaveManager)
    {
        DeriveTilingFromWaveManager();
    }
}

void AWaveHeight::DeriveTilingFromWaveManager()
{
    // The rendered wave is tiled by AInfiniteWaveManager: mesh tile p sits at p * S_w with S_w =
    // (YOffsetPerActor, ActorSpacing) in WORLD, and shows frame F + FrameOffsetPerActor * (p + 1).
    // The height data is tiled here: data tile t sits at t * (WorldOffsetPerTileX, WorldOffsetPerTileY)
    // in this actor's LOCAL frame and samples frame F + FrameOffsetPerTileX * t. For the two to be the
    // same wave, the data step must be the mesh step brought into local space, and the frame step must
    // run with it. Measured on Surfing_infinite_wave (specs/wave-mesh-data-registration.md): the data
    // tile index runs OPPOSITE to the mesh position index (t = -p - 1), so the local step is
    // R^-1 * (-S_w) and the frame step is -FrameOffsetPerActor. Both signs flip together — flipping
    // only one puts the frames 190 apart per tile. The constant part of the alignment (which data tile
    // sits under mesh tile 0: BaseOffsetX/Y, ReferenceFrameOffset, this actor's placement) is not
    // touched here; it is level-authored and was verified by re-sampling a recorded ride against the
    // mesh vertices (crest positions agree to within one 50 cm scan step over 13 tiles).
    //
    // Reads only the placed GridLODActors' transforms and FrameOffset, so BeginPlay order against the
    // manager does not matter (the manager shifts the actors as a block later; the step is unchanged).
    AInfiniteWaveManager* Manager = nullptr;
    if (UWorld* World = GetWorld())
    {
        for (TActorIterator<AInfiniteWaveManager> It(World); It; ++It) { Manager = *It; break; }
    }
    if (!Manager)
    {
        UE_LOG(LogSurf, Display, TEXT("WaveHeight '%s': no InfiniteWaveManager in the level; keeping the authored tiling (WorldOffsetPerTile=(%.1f, %.1f) FrameOffsetPerTileX=%d)"),
            *GetName(), WorldOffsetPerTileX, WorldOffsetPerTileY, FrameOffsetPerTileX);
        return;
    }

    float ActorSpacing = 0.0f, YOffsetPerActor = 0.0f;
    int32 FrameOffsetPerActor = 0;
    if (!AInfiniteWaveManager::InferTileLayout(Manager->ManagedGridActors, ActorSpacing, YOffsetPerActor, FrameOffsetPerActor))
    {
        UE_LOG(LogSurf, Warning, TEXT("WaveHeight '%s': InfiniteWaveManager has fewer than two valid GridLODActors; keeping the authored tiling"), *GetName());
        return;
    }

    const FVector MeshStepWorld(YOffsetPerActor, ActorSpacing, 0.0f);
    const FVector DataStepLocal = GetActorQuat().Inverse().RotateVector(-MeshStepWorld);

    const float AuthoredX = WorldOffsetPerTileX, AuthoredY = WorldOffsetPerTileY;
    const int32 AuthoredFrame = FrameOffsetPerTileX;
    WorldOffsetPerTileX = DataStepLocal.X;
    WorldOffsetPerTileY = DataStepLocal.Y;
    FrameOffsetPerTileX = -FrameOffsetPerActor;

    const bool bChanged = !FMath::IsNearlyEqual(AuthoredX, WorldOffsetPerTileX, 0.5f)
        || !FMath::IsNearlyEqual(AuthoredY, WorldOffsetPerTileY, 0.5f)
        || AuthoredFrame != FrameOffsetPerTileX;
    UE_LOG(LogSurf, Display, TEXT("WaveHeight '%s': tiling derived from InfiniteWaveManager (mesh step world=(%.1f, %.1f), %d frames/tile) -> WorldOffsetPerTile local=(%.1f, %.1f) FrameOffsetPerTileX=%d%s"),
        *GetName(), MeshStepWorld.X, MeshStepWorld.Y, FrameOffsetPerActor,
        WorldOffsetPerTileX, WorldOffsetPerTileY, FrameOffsetPerTileX,
        bChanged ? *FString::Printf(TEXT(" (authored values (%.1f, %.1f) / %d overridden)"), AuthoredX, AuthoredY, AuthoredFrame) : TEXT(""));
}

namespace
{
    // Forces the wave debug draw on and shows the STORED normals, so the axis wavePenetrationDrag
    // resists along can be checked by eye against the wave mesh. The value is the sample stride
    // (bigger = sparser); 0 = off. Read directly here rather than pushed from a subsystem, so it
    // sticks when set from a launch -ExecCmds. See specs/wave-interaction-damping-and-redirect.md.
    static int32 WaveHeight_DebugNormals = 0;
    static FAutoConsoleVariableRef CVarWaveHeightDebugNormals(
        TEXT("surf.debug.wavenormals"),
        WaveHeight_DebugNormals,
        TEXT("Draw the stored wave normals (white = full 3D, magenta = horizontal projection). Value = sample stride. 0 = off."));
}

// Called every frame
void AWaveHeight::Tick(float DeltaTime)
{
    Super::Tick(DeltaTime);

    if (WaveHeight_DebugNormals > 0)
    {
        // Deliberately does NOT set bShowDebugVisualization: that flag also turns on a per-QUERY
        // green sphere in calculateWaveLocationAndNormal, which every FluidDynamics actor hits many
        // times a tick — it buries the arrows under thousands of spheres. Call the draw directly.
        bShowNormalDebug        = true;
        bShowDataBoundary       = false;
        bShowSamplePoints       = false;
        bShowVelocityDebug      = false;
        bShowCornerMarkers      = false;   // MarkerSize-100 spheres per tile; they swamp the arrows
        bShowLabels             = false;
        SamplePointStride       = WaveHeight_DebugNormals;
        NormalDebugScale        = 120.0f;
        SamplePointCountX       = 0;   // all of the cross-shore axis: that is the axis in question
        SamplePointStartY       = 0;
        SamplePointCountY       = 0;   // whole grid; the stride keeps the arrow count sane
        // The board rides far down the line while the data grid stays put, so tile (0,0) is
        // typically ~150 m from the camera. Sweep a band of tiles and let the per-sample distance
        // filter below keep only what is near the pawn.
        // NOTE: the tile world offset is driven by TileX ALONE (TileY only shifts the frame), and it
        // steps diagonally — roughly +1155 in X and +2265 in Y per tile. The board rides far down
        // the line, so the tile under it is a high TileX, not a high TileY. Sweep wide and let the
        // per-sample distance filter keep what is on screen.
        bShowMultipleTiles      = true;
        DebugTileMinX = -20; DebugTileMaxX = 20;
        DebugTileMinY = 0;  DebugTileMaxY = 0;
        DrawDebugVisualization();
    }
    else if (bShowDebugVisualization)
    {
        DrawDebugVisualization();
    }

#if !UE_BUILD_SHIPPING
    static bool bProbed = false;
    if (!bProbed && waveUnifiedData && waveUnifiedMetadata)
    {
        const FString ProbePaths = CVarWaveProbe.GetValueOnGameThread();
        if (!ProbePaths.IsEmpty())
        {
            bProbed = true;
            TArray<FString> Paths;
            ProbePaths.ParseIntoArray(Paths, TEXT(";"), true); // ';' because ',' is the -ExecCmds separator
            for (const FString& P : Paths) ProbeTraceWaterSurface(P.TrimStartAndEnd());
        }
    }

    // One-shot dump of the RAW datatable, to answer "is the noise in the data or in the sampling?".
    // Walks consecutive grid points at a fixed frame and prints the STORED height and normal, with no
    // interpolation, no tiling and no world transform in the way. If adjacent grid points disagree
    // wildly, the wave data itself is noisy and the fix belongs in whatever generated it.
    static bool bDumped = false;
    if (!bDumped && SurfDebug::IsFlagSet(TEXT("wavedump")) && waveUnifiedData && waveUnifiedMetadata)
    {
        bDumped = true;
        FWaveUnifiedMetadata* Meta = waveUnifiedMetadata->FindRow<FWaveUnifiedMetadata>("Metadata", "");
        if (Meta)
        {
            // Dump the same cross-shore row at two frames. One row answers "where is the fast water
            // relative to the crest" (the velocity-registration question); two frames separated in
            // time give the crest's travel, hence the wave's phase speed — which is the only honest
            // ceiling on how fast the water at the crest can move. See specs/wave-data-velocity-registration.md.
            UE_LOG(LogSurf, Warning,
                TEXT("WAVEDUMP meta: grid=%dx%d step=%.4f gridStart=(%.3f, %.3f) frames=%d..%d CoordinateScale=(%.3f, %.3f, %.3f) VelocityScale=%.1f"),
                Meta->grid_width, Meta->grid_height, Meta->step_size,
                Meta->grid_start_x, Meta->grid_start_y, Meta->start_frame, Meta->end_frame,
                CoordinateScale.X, CoordinateScale.Y, CoordinateScale.Z, VelocityScale.X);

            const int32 kFrameGap = 10;
            const int32 Frames[2] = { StartFrame, StartFrame + kFrameGap };
            for (int32 fi = 0; fi < 2; ++fi)
            {
                const FString RowName = FString::Printf(TEXT("Frame_%d"), Frames[fi]);
                FWaveUnifiedFrameData* Frame =
                    waveUnifiedData->FindRow<FWaveUnifiedFrameData>(FName(*RowName), "");
                if (!Frame)
                {
                    UE_LOG(LogSurf, Warning, TEXT("WAVEDUMP: frame row %s not found"), *RowName);
                    continue;
                }
                UE_LOG(LogSurf, Warning, TEXT("WAVEDUMP: frame=%s heights=%d normals=%d vx=%d"),
                    *RowName, Frame->h.Num(), Frame->nz.Num(), Frame->vx.Num());
                const int32 gy = Meta->grid_height / 2;
                for (int32 gx = 0; gx + 1 < Meta->grid_width; ++gx)
                {
                    const float h  = getHeightAtGridPoint(gx, gy, Frame, Meta);
                    const FVector n = getNormalAtGridPoint(gx, gy, Frame, Meta);
                    const float hN = getHeightAtGridPoint(gx + 1, gy, Frame, Meta);
                    const FVector nN = getNormalAtGridPoint(gx + 1, gy, Frame, Meta);
                    const float slope  = FMath::Sqrt(FMath::Max(0.0f, 1.0f - n.Z * n.Z));
                    const float slopeN = FMath::Sqrt(FMath::Max(0.0f, 1.0f - nN.Z * nN.Z));
                    // Raw stored velocity, and the same value after the hardcoded VelocityScale the
                    // rest of the game samples through (calculateWaveVelocity).
                    const FVector vRaw = getVelocityAtGridPoint(gx, gy, Frame, Meta);
                    const FVector vScaled(vRaw.X * VelocityScale.X, vRaw.Y * VelocityScale.Y, vRaw.Z * VelocityScale.Z);
                    // Neighbours in Y as well as X, so the stored normal can be checked against the
                    // FULL height gradient rather than a single-axis slice (a down-the-line gradient
                    // would otherwise look like a mismatch). gradient = (dh/dx, dh/dy) in data units.
                    const float hYp = getHeightAtGridPoint(gx, FMath::Min(gy + 1, Meta->grid_height - 1), Frame, Meta);
                    const float hYm = getHeightAtGridPoint(gx, FMath::Max(gy - 1, 0), Frame, Meta);
                    const float dhdx = (hN - h) / Meta->step_size;
                    const float dhdy = (hYp - hYm) / (2.0f * Meta->step_size);
                    UE_LOG(LogSurf, Warning,
                        TEXT("WAVEDUMP row: f=%d gx=%4d h=%9.4f slope=%.4f dH=%9.4f dSlope=%.4f n=(%.4f, %.4f, %.4f) grad=(%.4f, %.4f) vRaw=(%.4f, %.4f, %.4f) |vRaw|=%.4f vScaled=(%.1f, %.1f, %.1f) |vScaled|=%.1f"),
                        Frames[fi], gx, h, slope, hN - h, slopeN - slope,
                        n.X, n.Y, n.Z, dhdx, dhdy,
                        vRaw.X, vRaw.Y, vRaw.Z, vRaw.Size(),
                        vScaled.X, vScaled.Y, vScaled.Z, vScaled.Size());
                }
            }
        }
        else
        {
            UE_LOG(LogSurf, Warning, TEXT("WAVEDUMP: metadata row not found"));
        }
    }
#endif
}

void AWaveHeight::ProbeTraceWaterSurface(const FString& TracePath)
{
#if !UE_BUILD_SHIPPING
    TArray<FString> Lines;
    if (!FFileHelper::LoadFileToStringArray(Lines, *TracePath))
    {
        UE_LOG(LogSurf, Error, TEXT("WAVEPROBE: cannot read %s"), *TracePath);
        return;
    }

    // Header row is the first non-comment line; resolve columns by name so the probe survives the
    // trace format growing new trailing columns.
    int32 ColT = -1, ColX = -1, ColY = -1, ColZ = -1, ColFrame = -1;
    int32 LineIdx = 0;
    for (; LineIdx < Lines.Num(); ++LineIdx)
    {
        if (Lines[LineIdx].StartsWith(TEXT("#")) || Lines[LineIdx].IsEmpty()) continue;
        TArray<FString> Cols;
        Lines[LineIdx].ParseIntoArray(Cols, TEXT(","), false);
        ColT     = Cols.IndexOfByKey(TEXT("t"));
        ColX     = Cols.IndexOfByKey(TEXT("board_x"));
        ColY     = Cols.IndexOfByKey(TEXT("board_y"));
        ColZ     = Cols.IndexOfByKey(TEXT("board_z"));
        ColFrame = Cols.IndexOfByKey(TEXT("wave_frame"));
        ++LineIdx;
        break;
    }
    if (ColT < 0 || ColX < 0 || ColY < 0 || ColZ < 0 || ColFrame < 0)
    {
        UE_LOG(LogSurf, Error, TEXT("WAVEPROBE: %s lacks t/board_x/board_y/board_z/wave_frame columns"), *TracePath);
        return;
    }

    FWaveUnifiedMetadata* Meta = waveUnifiedMetadata->FindRow<FWaveUnifiedMetadata>("Metadata", "");

    // Cross-shore ("back of wave") axis, derived from the InfiniteWaveManager tile geometry exactly as
    // ASharedCalculations does for signedDistanceToCrest (perpendicular to the down-line tiling axis).
    FVector BackDir = FVector(1.0f, 0.0f, 0.0f);
    if (UWorld* World = GetWorld())
    {
        for (TActorIterator<AInfiniteWaveManager> It(World); It; ++It)
        {
            const FVector back = FVector(It->ActorSpacing, -It->YOffsetPerActor, 0.0f).GetSafeNormal();
            if (!back.IsNearlyZero()) { BackDir = (back.X < 0.0f) ? -back : back; break; }
        }
    }
    // Height profile offsets along BackDir (cm), written as one column each so the crest's position
    // relative to the board can be eyeballed / plotted straight from the CSV. - = shoreward (face side).
    static const int32 ProfileOffsets[] = { -1200, -900, -600, -450, -300, -150, 0, 150, 300, 450, 600, 900, 1200, 1800 };

    // tile = which of the sampler's -5..+5 search tiles the row resolved to. The search only covers
    // that window, so a ride that outruns it silently samples a clamped edge; pinned at +-5 is the tell.
    // dist_crest = the physics' own signedDistanceToCrest (same robust +-1200 cm scan as
    // SharedCalculations, 50 cm steps, 0.5 cm noise floor): >0 = board is BEHIND the crest (far side),
    // <0 = on the face. crest_h/h0 in data units (multiply by CoordinateScale.Z for cm).
    FString Out = TEXT("t,board_x,board_y,board_z,wave_frame,wave_z,depth,n_x,n_y,n_z,tile,dist_crest,h0,crest_h");
    for (int32 off : ProfileOffsets) Out += FString::Printf(TEXT(",h%+d"), off);
    Out += TEXT("\n");
    int32 Rows = 0;
    float MinDepth = FLT_MAX, MaxDepth = -FLT_MAX;
    for (; LineIdx < Lines.Num(); ++LineIdx)
    {
        if (Lines[LineIdx].IsEmpty()) continue;
        TArray<FString> Cols;
        Lines[LineIdx].ParseIntoArray(Cols, TEXT(","), false);
        if (Cols.Num() <= FMath::Max(FMath::Max(ColX, ColY), FMath::Max(ColZ, ColFrame))) continue;
        const FVector Pos(FCString::Atof(*Cols[ColX]), FCString::Atof(*Cols[ColY]), FCString::Atof(*Cols[ColZ]));
        const int32 Frame = FCString::Atoi(*Cols[ColFrame]);
        const TArray<FVector> R = calculateWaveLocationAndNormal(Pos, Frame);
        const float Depth = Pos.Z - R[0].Z; // negative = board centre under the surface
        const int32 Tile = (Meta && FrameOffsetPerTileX != 0)
            ? (CalculateFrameOffsetForPosition(Pos, Meta) - ReferenceFrameOffset) / FrameOffsetPerTileX : 0;
        MinDepth = FMath::Min(MinDepth, Depth);
        MaxDepth = FMath::Max(MaxDepth, Depth);
        // Same scan as ASharedCalculations::calculateAll (robust mode): global peak within +-1200 cm
        // along BackDir wins, but only if it beats the current best by more than the noise floor.
        auto SampleH = [&](float off) -> double
        {
            return std::get<0>(waveHeightAndNormal(Pos + BackDir * off, Frame));
        };
        const double H0 = SampleH(0.0f);
        double BestH = H0; float BestOff = 0.0f;
        for (int32 i = 1; i <= 24; ++i)
        {
            const float offsets[2] = { -(float)i * 50.0f, (float)i * 50.0f };
            for (const float off : offsets)
            {
                const double h = SampleH(off);
                if (h > BestH + 0.5) { BestH = h; BestOff = off; }
            }
        }
        const float DistCrest = -BestOff;
        Out += FString::Printf(TEXT("%s,%.1f,%.1f,%.1f,%d,%.1f,%.1f,%.3f,%.3f,%.3f,%d,%.0f,%.2f,%.2f"),
            *Cols[ColT], Pos.X, Pos.Y, Pos.Z, Frame, R[0].Z, Depth, R[1].X, R[1].Y, R[1].Z, Tile, DistCrest, H0, BestH);
        for (int32 off : ProfileOffsets) Out += FString::Printf(TEXT(",%.2f"), SampleH((float)off));
        Out += TEXT("\n");
        ++Rows;
    }

    const FString OutPath = FPaths::ChangeExtension(TracePath, TEXT("water.csv"));
    FFileHelper::SaveStringToFile(Out, *OutPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    UE_LOG(LogSurf, Warning, TEXT("WAVEPROBE: %d rows -> %s (board_z - wave_z range %.1f .. %.1f cm)"),
        Rows, *OutPath, MinDepth, MaxDepth);
#endif
}

void AWaveHeight::DrawDebugVisualization()
{
    UE_LOG(LogSurf, Warning, TEXT("DrawDebugVisualization called"));

    if (!waveUnifiedMetadata || !waveUnifiedData)
    {
        UE_LOG(LogSurf, Error, TEXT("DrawDebugVisualization: DataTables are NULL! Metadata=%s Data=%s"),
            waveUnifiedMetadata ? TEXT("OK") : TEXT("NULL"),
            waveUnifiedData ? TEXT("OK") : TEXT("NULL"));
        return;
    }

    auto* MetadataRow = waveUnifiedMetadata->FindRow<FWaveUnifiedMetadata>("Metadata", "");
    if (!MetadataRow)
    {
        UE_LOG(LogSurf, Error, TEXT("DrawDebugVisualization: Could not find Metadata row"));
        return;
    }

    UWorld* World = GetWorld();
    if (!World)
    {
        UE_LOG(LogSurf, Error, TEXT("DrawDebugVisualization: World is NULL"));
        return;
    }

    // Get WaveHeight actor transform
    FVector ActorPos = GetActorLocation();
    FQuat ActorRot = GetActorQuat();
    UE_LOG(LogSurf, Warning, TEXT("DrawDebugVisualization: ActorPos=(%.2f, %.2f, %.2f), Rotation=(%.2f, %.2f, %.2f)"),
        ActorPos.X, ActorPos.Y, ActorPos.Z,
        GetActorRotation().Pitch, GetActorRotation().Yaw, GetActorRotation().Roll);

    // Log metadata once for ground truth testing
    static bool bMetadataLogged = false;
    if (!bMetadataLogged && MetadataRow)
    {
        UE_LOG(LogSurf, Warning, TEXT("METADATA|grid_start=(%.6f,%.6f)|step_size=%.6f|grid_size=(%d,%d)|tiling=(%.6f,%.6f)|bFlipXAxis=%d|bFlipWaveHeight=%d"),
            MetadataRow->grid_start_x, MetadataRow->grid_start_y,
            MetadataRow->step_size,
            MetadataRow->grid_width, MetadataRow->grid_height,
            MetadataRow->tiling_x, MetadataRow->tiling_y,
            bFlipXAxis ? 1 : 0,
            bFlipWaveHeight ? 1 : 0);
        bMetadataLogged = true;
    }

    // Calculate grid dimensions in Blender/data space
    // Use actual grid extent (number of samples - 1) * step_size, not tiling_x
    float gridBlenderWidth = (MetadataRow->grid_width - 1) * MetadataRow->step_size;
    float gridBlenderHeight = (MetadataRow->grid_height - 1) * MetadataRow->step_size;

    // Determine which tiles to visualize
    int32 TileMinX = bShowMultipleTiles ? DebugTileMinX : 0;
    int32 TileMaxX = bShowMultipleTiles ? DebugTileMaxX : 0;
    int32 TileMinY = bShowMultipleTiles ? DebugTileMinY : 0;
    int32 TileMaxY = bShowMultipleTiles ? DebugTileMaxY : 0;

    // Loop through tiles
    for (int32 TileY = TileMinY; TileY <= TileMaxY; ++TileY)
    {
        for (int32 TileX = TileMinX; TileX <= TileMaxX; ++TileX)
        {
            // Calculate tile offset in local space (before rotation)
            // Include base offset + per-tile offset
            // Both X and Y offsets are applied based on TileX (for side-scrolling tiling)
            FVector TileLocalOffset = FVector(
                BaseOffsetX + (TileX * WorldOffsetPerTileX),
                BaseOffsetY + (TileX * WorldOffsetPerTileY),  // Changed from TileY to TileX
                0.0f
            );

            // Rotate tile offset to match actor's rotation
            FVector TileWorldOffset = ActorRot.RotateVector(TileLocalOffset);

            // Calculate frame offset for this tile
            // Same formula as CalculateFrameOffsetForPosition in commit d01aadc1
            // (tile 0=offset 0, tile -1=offset -95, tile -2=offset -190)
            int32 TileFrameOffset = ReferenceFrameOffset + (TileX * FrameOffsetPerTileX) + (TileY * FrameOffsetPerTileY);

            UE_LOG(LogSurf, Warning, TEXT("Drawing tile (%d, %d) with base (%.1f, %.1f) + tile offset (%.1f, %.1f) = local (%.1f, %.1f) -> world (%.1f, %.1f, %.1f), frame %d"),
                TileX, TileY, BaseOffsetX, BaseOffsetY,
                TileX * WorldOffsetPerTileX, TileY * WorldOffsetPerTileY,
                TileLocalOffset.X, TileLocalOffset.Y, TileWorldOffset.X, TileWorldOffset.Y, TileWorldOffset.Z, TileFrameOffset);

            // Calculate corners in data-local space then transform to world
            FVector dataCorner00 = FVector(MetadataRow->grid_start_x, MetadataRow->grid_start_y, 0) * CoordinateScale;
            FVector dataCorner01 = FVector(MetadataRow->grid_start_x, MetadataRow->grid_start_y + gridBlenderHeight, 0) * CoordinateScale;
            FVector dataCorner10 = FVector(MetadataRow->grid_start_x + gridBlenderWidth, MetadataRow->grid_start_y, 0) * CoordinateScale;
            FVector dataCorner11 = FVector(MetadataRow->grid_start_x + gridBlenderWidth, MetadataRow->grid_start_y + gridBlenderHeight, 0) * CoordinateScale;

            // Apply rotation and position offset to transform to world space, then add rotated tile offset
            FVector WorldCorner00 = ActorPos + ActorRot.RotateVector(dataCorner00) + TileWorldOffset;
            FVector WorldCorner01 = ActorPos + ActorRot.RotateVector(dataCorner01) + TileWorldOffset;
            FVector WorldCorner10 = ActorPos + ActorRot.RotateVector(dataCorner10) + TileWorldOffset;
            FVector WorldCorner11 = ActorPos + ActorRot.RotateVector(dataCorner11) + TileWorldOffset;

            // Draw data boundary box
            if (bShowDataBoundary)
            {
                UE_LOG(LogSurf, Warning, TEXT("Drawing boundary box - Corners: (%.2f,%.2f,%.2f) to (%.2f,%.2f,%.2f)"),
                    WorldCorner00.X, WorldCorner00.Y, WorldCorner00.Z,
                    WorldCorner11.X, WorldCorner11.Y, WorldCorner11.Z);

                // Draw horizontal edges
                DrawDebugLine(World, WorldCorner00, WorldCorner01, FColor::Cyan, false, 0.2f, 0, 5.0f);
                DrawDebugLine(World, WorldCorner10, WorldCorner11, FColor::Cyan, false, 0.2f, 0, 5.0f);

                // Draw vertical edges
                DrawDebugLine(World, WorldCorner00, WorldCorner10, FColor::Cyan, false, 0.2f, 0, 5.0f);
                DrawDebugLine(World, WorldCorner01, WorldCorner11, FColor::Cyan, false, 0.2f, 0, 5.0f);

                // Draw vertical lines at corners for visibility
                float VerticalHeight = 500.0f;
                DrawDebugLine(World, WorldCorner00, WorldCorner00 + FVector(0, 0, VerticalHeight), FColor::Cyan, false, 0.2f, 0, 5.0f);
                DrawDebugLine(World, WorldCorner01, WorldCorner01 + FVector(0, 0, VerticalHeight), FColor::Green, false, 0.2f, 0, 5.0f);
                DrawDebugLine(World, WorldCorner10, WorldCorner10 + FVector(0, 0, VerticalHeight), FColor::Yellow, false, 0.2f, 0, 5.0f);
                DrawDebugLine(World, WorldCorner11, WorldCorner11 + FVector(0, 0, VerticalHeight), FColor::Blue, false, 0.2f, 0, 5.0f);
            }

            // Draw corner markers
            if (bShowCornerMarkers)
            {
                float MarkerSize = 100.0f;
                UE_LOG(LogSurf, Warning, TEXT("Drawing corner markers with size %.2f at corners"), MarkerSize);
                DrawDebugSphere(World, WorldCorner00, MarkerSize, 12, FColor::Red, false, 0.2f, 0, 5.0f);      // Origin
                DrawDebugSphere(World, WorldCorner01, MarkerSize, 12, FColor::Green, false, 0.2f, 0, 5.0f);    // Y-end
                DrawDebugSphere(World, WorldCorner10, MarkerSize, 12, FColor::Yellow, false, 0.2f, 0, 5.0f);   // X-end
                DrawDebugSphere(World, WorldCorner11, MarkerSize, 12, FColor::Blue, false, 0.2f, 0, 5.0f);     // Opposite corner
            }

            // Draw labels
            if (bShowLabels)
            {
                FString TileLabel = (TileX != 0 || TileY != 0)
                    ? FString::Printf(TEXT(" [Tile %d,%d]"), TileX, TileY)
                    : TEXT("");
                FString DimensionsLabel = FString::Printf(TEXT("WaveHeight Unified Data%s\nGrid: %dx%d samples\nStep: %.1f\nBlender bounds: (%.1f,%.1f) to (%.1f,%.1f)"),
                    *TileLabel, MetadataRow->grid_width, MetadataRow->grid_height, MetadataRow->step_size,
                    MetadataRow->grid_start_x, MetadataRow->grid_start_y,
                    MetadataRow->grid_start_x + gridBlenderWidth, MetadataRow->grid_start_y + gridBlenderHeight);
                DrawDebugString(World, WorldCorner00 + FVector(0, 0, 600), DimensionsLabel, nullptr, FColor::White, 0.2f, true);
            }

            // Draw sample points (optional)
            if (bShowSamplePoints)
            {
                // Use current frame from WaterController + tile frame offset
                int32 BaseFrame = GetCurrentFrameFromWaterController();
                int32 FrameToShow = BaseFrame + TileFrameOffset;

                // Wrap frame within StartFrame to EndFrame range
                if (EndFrame > StartFrame)
                {
                    int32 FrameRange = EndFrame - StartFrame + 1;
                    while (FrameToShow > EndFrame)
                    {
                        FrameToShow -= FrameRange;
                    }
                    while (FrameToShow < StartFrame)
                    {
                        FrameToShow += FrameRange;
                    }
                }

                FString FrameName = FString::Printf(TEXT("Frame_%d"), FrameToShow);
                auto* FrameRow = waveUnifiedData->FindRow<FWaveUnifiedFrameData>(FName(*FrameName), "");

                UE_LOG(LogSurf, Warning, TEXT("Attempting to draw sample points for frame %d (base=%d + offset=%d)"),
                    FrameToShow, BaseFrame, TileFrameOffset);

                if (FrameRow && FrameRow->h.Num() > 0)
                {
                    UE_LOG(LogSurf, Warning, TEXT("Frame data loaded: %d samples, SamplePointStride=%d, DebugSphereSize=%.2f"),
                        FrameRow->h.Num(), SamplePointStride, DebugSphereSize);
                    // Calculate range to draw
                    int32 StartX = FMath::Clamp(SamplePointStartX, 0, MetadataRow->grid_width - 1);
                    int32 EndX = (SamplePointCountX > 0)
                        ? FMath::Clamp(StartX + SamplePointCountX, StartX, MetadataRow->grid_width)
                        : MetadataRow->grid_width;

                    int32 StartY = FMath::Clamp(SamplePointStartY, 0, MetadataRow->grid_height - 1);
                    int32 EndY = (SamplePointCountY > 0)
                        ? FMath::Clamp(StartY + SamplePointCountY, StartY, MetadataRow->grid_height)
                        : MetadataRow->grid_height;

                    int32 PointsDrawn = 0;
                    for (int32 gridY = StartY; gridY < EndY; gridY += SamplePointStride)
                    {
                        for (int32 gridX = StartX; gridX < EndX; gridX += SamplePointStride)
                        {
                            int32 idx = getGridIndex(gridX, gridY, MetadataRow->grid_width);
                            if (idx < 0 || idx >= FrameRow->h.Num())
                                continue;

                            float waveHeight = FrameRow->h[idx];
                            if (bFlipWaveHeight)
                                waveHeight = -waveHeight;

                            // Convert grid position to Blender coordinates
                            float blenderX = MetadataRow->grid_start_x + gridX * MetadataRow->step_size;
                            float blenderY = MetadataRow->grid_start_y + gridY * MetadataRow->step_size;

                            // Apply X-axis flip if enabled (mirror horizontally)
                            if (bFlipXAxis)
                            {
                                float gridExtentX = (MetadataRow->grid_width - 1) * MetadataRow->step_size;
                                float centerX = MetadataRow->grid_start_x + gridExtentX / 2.0f;
                                blenderX = 2.0f * centerX - blenderX;  // Mirror around center
                            }

                            // Convert to Unreal local space, then apply rotation and translate to world
                            FVector dataLocalPos = FVector(blenderX, blenderY, waveHeight) * CoordinateScale;

                            // Add tile offset (this is just a simple positional offset in world space)
                            FVector WorldPos = ActorPos + ActorRot.RotateVector(dataLocalPos) + TileWorldOffset;

                            // GROUND TRUTH LOGGING: Log known-good grid→world transformations for testing
                            // This data will be used to validate the backward transformation (world→grid)
                            // Color based on height
                            FColor PointColor;
                            if (FMath::Abs(waveHeight) < 0.01f)
                            {
                                PointColor = FColor::Silver;  // Zero/near-zero = gray
                            }
                            else if (waveHeight > 0)
                            {
                                PointColor = FColor::Cyan;    // Positive = cyan (wave crest)
                            }
                            else
                            {
                                PointColor = FColor::Magenta; // Negative = magenta (trough)
                            }

                            DrawDebugSphere(World, WorldPos, DebugSphereSize, 8, PointColor, false, 0.2f, 0, 1.0f);
                            PointsDrawn++;
                        }
                    }

                    if (bShowLabels)
                    {
                        FString PointsLabel = FString::Printf(TEXT("Sample Points: %d\n(X:%d-%d step %d, Y:%d-%d step %d)\nFrame:%d"),
                            PointsDrawn, StartX, EndX, SamplePointStride, StartY, EndY, SamplePointStride, FrameToShow);
                        DrawDebugString(World, WorldCorner01 + FVector(0, 0, 600), PointsLabel, nullptr, FColor::Cyan, 0.2f, true);
                    }
                }
                else
                {
                    UE_LOG(LogSurf, Warning, TEXT("DrawDebugVisualization: Could not find frame data for Frame_%d"), FrameToShow);
                }
            }

            // Draw the STORED surface normals (optional). Two arrows per sample:
            //   WHITE  - the full 3D normal, so the surface orientation is visible
            //   MAGENTA - its HORIZONTAL projection, flat on the water. This is the axis
            //             wavePenetrationDrag resists along, and the thing being checked: it should
            //             point ACROSS the face (toward/away from shore), never along the crest.
            // See specs/wave-interaction-damping-and-redirect.md.
            if (bShowNormalDebug)
            {
                int32 BaseFrameN = GetCurrentFrameFromWaterController();
                int32 FrameToShowN = BaseFrameN + TileFrameOffset;
                if (EndFrame > StartFrame)
                {
                    const int32 FrameRangeN = EndFrame - StartFrame + 1;
                    while (FrameToShowN > EndFrame)   { FrameToShowN -= FrameRangeN; }
                    while (FrameToShowN < StartFrame) { FrameToShowN += FrameRangeN; }
                }
                const FString FrameNameN = FString::Printf(TEXT("Frame_%d"), FrameToShowN);
                auto* FrameRowN = waveUnifiedData->FindRow<FWaveUnifiedFrameData>(FName(*FrameNameN), "");
                if (FrameRowN && FrameRowN->h.Num() > 0)
                {
                    const int32 StartXN = FMath::Clamp(SamplePointStartX, 0, MetadataRow->grid_width - 1);
                    const int32 EndXN = (SamplePointCountX > 0)
                        ? FMath::Clamp(StartXN + SamplePointCountX, StartXN, MetadataRow->grid_width)
                        : MetadataRow->grid_width;
                    const int32 StartYN = FMath::Clamp(SamplePointStartY, 0, MetadataRow->grid_height - 1);
                    const int32 EndYN = (SamplePointCountY > 0)
                        ? FMath::Clamp(StartYN + SamplePointCountY, StartYN, MetadataRow->grid_height)
                        : MetadataRow->grid_height;

                    int32 nDrawn = 0, nHorizAlongY = 0;
                    FVector firstOrigin = FVector::ZeroVector, lastOrigin = FVector::ZeroVector;
                    // Only draw what the camera can actually see, or the arrows land a wave-length
                    // away and the screenshot shows nothing.
                    const APawn* NearPawn = World->GetFirstPlayerController() ? World->GetFirstPlayerController()->GetPawn() : nullptr;
                    const FVector NearPos = NearPawn ? NearPawn->GetActorLocation() : FVector::ZeroVector;
                    const float NearRadiusSq = 3000.0f * 3000.0f;
                    for (int32 gridY = StartYN; gridY < EndYN; gridY += SamplePointStride)
                    {
                        for (int32 gridX = StartXN; gridX < EndXN; gridX += SamplePointStride)
                        {
                            const int32 idx = getGridIndex(gridX, gridY, MetadataRow->grid_width);
                            if (idx < 0 || idx >= FrameRowN->h.Num()) { continue; }

                            float waveHeightN = FrameRowN->h[idx];
                            if (bFlipWaveHeight) { waveHeightN = -waveHeightN; }

                            float blenderXN = MetadataRow->grid_start_x + gridX * MetadataRow->step_size;
                            const float blenderYN = MetadataRow->grid_start_y + gridY * MetadataRow->step_size;
                            if (bFlipXAxis)
                            {
                                const float gridExtentXN = (MetadataRow->grid_width - 1) * MetadataRow->step_size;
                                const float centerXN = MetadataRow->grid_start_x + gridExtentXN / 2.0f;
                                blenderXN = 2.0f * centerXN - blenderXN;
                            }

                            const FVector dataLocalPosN = FVector(blenderXN, blenderYN, waveHeightN) * CoordinateScale;
                            const FVector originN = ActorPos + ActorRot.RotateVector(dataLocalPosN) + TileWorldOffset;
                            if (NearPawn && FVector::DistSquared2D(originN, NearPos) > NearRadiusSq) { continue; }

                            // Stored normal, transformed the same way the runtime sampler does.
                            const FVector nLocal = getNormalAtGridPoint(gridX, gridY, FrameRowN, MetadataRow);
                            if (nLocal.IsNearlyZero()) { continue; }
                            const FVector nWorld = ActorRot.RotateVector(nLocal).GetSafeNormal();
                            const FVector nHoriz = FVector(nWorld.X, nWorld.Y, 0.0f).GetSafeNormal();

                            // Only the HORIZONTAL projection is drawn — that is the axis in question.
                            // Colour encodes the verdict so the picture answers the question directly:
                            //   GREEN = points across the face (correct for a wave-face normal)
                            //   RED   = points along the crest (the failure being checked for)
                            if (!nHoriz.IsNearlyZero())
                            {
                                const bool bAlongCrest = FMath::Abs(nHoriz.Y) > FMath::Abs(nHoriz.X);
                                const FColor c = bAlongCrest ? FColor::Red : FColor::Green;
                                DrawDebugDirectionalArrow(World, originN, originN + nHoriz * NormalDebugScale,
                                    25.0f, c, false, 0.2f, 0, 5.0f);
                                DrawDebugLine(World, originN, originN + FVector(0, 0, 25.0f), c, false, 0.2f, 0, 3.0f);
                                if (bAlongCrest) { ++nHorizAlongY; }
                            }
                            if (nDrawn == 0) { firstOrigin = originN; }
                            lastOrigin = originN;
                            ++nDrawn;
                        }
                    }
                    const APawn* CamPawn = World->GetFirstPlayerController() ? World->GetFirstPlayerController()->GetPawn() : nullptr;
                    const FVector CamPos = CamPawn ? CamPawn->GetActorLocation() : FVector::ZeroVector;
                    UE_LOG(LogSurf, Warning,
                        TEXT("NORMALDEBUG: frame=%s drew %d normals; horizontal projection is Y-dominant (along the crest) on %d of them (%.0f%%) | first=(%.0f, %.0f, %.0f) last=(%.0f, %.0f, %.0f) pawn=(%.0f, %.0f, %.0f) tile=(%d,%d)"),
                        *FrameNameN, nDrawn, nHorizAlongY, nDrawn ? 100.0f * nHorizAlongY / nDrawn : 0.0f,
                        firstOrigin.X, firstOrigin.Y, firstOrigin.Z, lastOrigin.X, lastOrigin.Y, lastOrigin.Z,
                        CamPos.X, CamPos.Y, CamPos.Z, TileX, TileY);
                }
            }

            // Draw velocity vectors (optional)
            if (bShowVelocityDebug)
            {
                // Get current frame for velocity calculation (same as sample points)
                int32 BaseFrame = GetCurrentFrameFromWaterController();
                int32 FrameToShow = BaseFrame + TileFrameOffset;

                // Wrap frame within StartFrame to EndFrame range
                if (EndFrame > StartFrame)
                {
                    int32 FrameRange = EndFrame - StartFrame + 1;
                    while (FrameToShow > EndFrame)
                    {
                        FrameToShow -= FrameRange;
                    }
                    while (FrameToShow < StartFrame)
                    {
                        FrameToShow += FrameRange;
                    }
                }

                FString FrameName = FString::Printf(TEXT("Frame_%d"), FrameToShow);
                auto* FrameRowForHeight = waveUnifiedData->FindRow<FWaveUnifiedFrameData>(FName(*FrameName), "");

                UE_LOG(LogSurf, Warning, TEXT("Drawing velocity arrows for frame %d (base=%d + offset=%d)"),
                    FrameToShow, BaseFrame, TileFrameOffset);

                if (FrameRowForHeight && FrameRowForHeight->h.Num() > 0)
                {
                    // Calculate range to draw (same as sample points for consistency)
                    int32 StartX = FMath::Clamp(SamplePointStartX, 0, MetadataRow->grid_width - 1);
                    int32 EndX = (SamplePointCountX > 0)
                        ? FMath::Clamp(StartX + SamplePointCountX, StartX, MetadataRow->grid_width)
                        : MetadataRow->grid_width;

                    int32 StartY = FMath::Clamp(SamplePointStartY, 0, MetadataRow->grid_height - 1);
                    int32 EndY = (SamplePointCountY > 0)
                        ? FMath::Clamp(StartY + SamplePointCountY, StartY, MetadataRow->grid_height)
                        : MetadataRow->grid_height;

                    int32 ArrowsDrawn = 0;
                    int32 ZeroVelocityCount = 0;
                    float MaxVelocityMagnitudeSq = 0.0f;
                    float TotalVelocityMagnitudeSq = 0.0f;
                    int32 TotalSampled = 0;

                    for (int32 gridY = StartY; gridY < EndY; gridY += SamplePointStride)
                    {
                        for (int32 gridX = StartX; gridX < EndX; gridX += SamplePointStride)
                        {
                            int32 idx = getGridIndex(gridX, gridY, MetadataRow->grid_width);
                            if (idx < 0 || idx >= FrameRowForHeight->h.Num())
                                continue;

                            // Get wave height directly from frame data (same as debug spheres)
                            float waveHeight = FrameRowForHeight->h[idx];
                            if (bFlipWaveHeight)
                                waveHeight = -waveHeight;

                            // Convert grid position to Blender coordinates (same as debug spheres)
                            float blenderX = MetadataRow->grid_start_x + gridX * MetadataRow->step_size;
                            float blenderY = MetadataRow->grid_start_y + gridY * MetadataRow->step_size;

                            // Apply X-axis flip if enabled
                            if (bFlipXAxis)
                            {
                                float gridExtentX = (MetadataRow->grid_width - 1) * MetadataRow->step_size;
                                float centerX = MetadataRow->grid_start_x + gridExtentX / 2.0f;
                                blenderX = 2.0f * centerX - blenderX;
                            }

                            // Convert to Unreal local space (same as debug spheres)
                            FVector dataLocalPos = FVector(blenderX, blenderY, waveHeight) * CoordinateScale;

                            // Transform to world space with tile offset (same as debug spheres)
                            FVector arrowStart = ActorPos + ActorRot.RotateVector(dataLocalPos) + TileWorldOffset;

                            // Use calculateWaveVelocity to get velocity with proper infinite tiling support
                            // NOTE: calculateWaveVelocity returns velocity in Blender coordinate space
                            // Pass tile indices directly to avoid coordinate confusion bug where
                            // CalculateFrameOffsetForPosition() misidentifies the tile due to data grid offset
                            FVector velocityBlenderSpace = calculateWaveVelocity(arrowStart, BaseFrame, TileX, TileY);

                            // Velocity is in the same coordinate space as positions (actor-local/Blender space)
                            // Apply the same rotation transformation as positions to align with the mesh
                            FVector worldVelocity = ActorRot.RotateVector(velocityBlenderSpace);

                            TotalSampled++;
                            float velocityMagSq = velocityBlenderSpace.SizeSquared();
                            MaxVelocityMagnitudeSq = FMath::Max(MaxVelocityMagnitudeSq, velocityMagSq);
                            TotalVelocityMagnitudeSq += velocityMagSq;

                            // Skip if velocity is exactly zero
                            if (velocityBlenderSpace.IsNearlyZero(0.0001f))
                            {
                                ZeroVelocityCount++;
                                continue;
                            }

                            // Calculate arrow end point
                            FVector arrowEnd = arrowStart + (worldVelocity * VelocityDebugScale);

                            // Draw arrow
                            DrawDebugDirectionalArrow(
                                World,
                                arrowStart,
                                arrowEnd,
                                50.0f,          // Arrow size
                                FColor::Red,    // Color (red for velocity)
                                false,          // Persistent
                                0.2f,           // Lifetime
                                0,              // Depth priority
                                2.0f            // Thickness
                            );

                            ArrowsDrawn++;
                        }
                    }

                    float MaxVelocityMagnitude = FMath::Sqrt(MaxVelocityMagnitudeSq);
                    float AvgVelocityMagnitude = (TotalSampled > 0) ? FMath::Sqrt(TotalVelocityMagnitudeSq / TotalSampled) : 0.0f;

                    if (bShowLabels)
                    {
                        FString VelocityLabel = FString::Printf(TEXT("Velocity Arrows: %d / %d\n(Zero: %d, Max: %.4f, Avg: %.4f)\nScale: %.3f, Frame:%d"),
                            ArrowsDrawn, TotalSampled, ZeroVelocityCount, MaxVelocityMagnitude, AvgVelocityMagnitude, VelocityDebugScale, FrameToShow);
                        DrawDebugString(World, WorldCorner10 + FVector(0, 0, 600), VelocityLabel, nullptr, FColor::Red, 0.2f, true);
                    }

                    UE_LOG(LogSurf, Warning, TEXT("Velocity debug: Drew %d arrows out of %d samples (%d zero). Max velocity: %.6f, Avg velocity: %.6f"),
                        ArrowsDrawn, TotalSampled, ZeroVelocityCount, MaxVelocityMagnitude, AvgVelocityMagnitude);
                }
                else
                {
                    UE_LOG(LogSurf, Warning, TEXT("DrawDebugVisualization: Could not find frame data for velocity arrows, Frame_%d"), FrameToShow);
                }
            }
        } // end TileX loop
    } // end TileY loop
}

// Helper function to read properties from WaterController
bool AWaveHeight::ReadIntPropertyFromWaterController(const FName& PropertyName, int32& OutValue) const
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

    FProperty* Property = WaterControllerClass->FindPropertyByName(PropertyName);
    if (!Property)
    {
        // Try camelCase version
        FString PropertyNameStr = PropertyName.ToString();
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

// Get current frame from WaterController
int32 AWaveHeight::GetCurrentFrameFromWaterController() const
{
    int32 CurrentFrame = StartFrame; // Default to start frame
    
    if (WaterController)
    {
        ReadIntPropertyFromWaterController(FName("CurrentFrame"), CurrentFrame);
    }
    
    return CurrentFrame;
}

// Wrap frame number and construct FName (like GridLODActor does)
FName AWaveHeight::WrapAndConstructFrameName(int32 Frame) const
{
    int32 WrappedFrame = Frame;
    
    // Wrap frame using same logic as GridLODActor
    if (EndFrame > StartFrame)
    {
        int32 AnimationLength = EndFrame - StartFrame + 1;
        while (WrappedFrame > EndFrame)
        {
            WrappedFrame = StartFrame + ((WrappedFrame - StartFrame) % AnimationLength);
        }
        while (WrappedFrame < StartFrame)
        {
            WrappedFrame = EndFrame - ((StartFrame - WrappedFrame - 1) % AnimationLength);
        }
    }
    
    return FName(*FString::Printf(TEXT("Frame_%d"), WrappedFrame));
}

// New auto functions that read from WaterController
TArray<FVector> AWaveHeight::calculateWaveLocationAndNormalAuto(FVector location)
{
    int32 CurrentFrame = GetCurrentFrameFromWaterController();

    // Wrap frame number
    int32 WrappedFrame = CurrentFrame;
    if (EndFrame > StartFrame)
    {
        int32 AnimationLength = EndFrame - StartFrame + 1;
        while (WrappedFrame > EndFrame)
        {
            WrappedFrame = StartFrame + ((WrappedFrame - StartFrame) % AnimationLength);
        }
        while (WrappedFrame < StartFrame)
        {
            WrappedFrame = EndFrame - ((StartFrame - WrappedFrame - 1) % AnimationLength);
        }
    }

    return calculateWaveLocationAndNormal(location, WrappedFrame);
}

FVector AWaveHeight::calculateWaveVelocityAuto(FVector location)
{
    int32 CurrentFrame = GetCurrentFrameFromWaterController();

    // Wrap frame number
    int32 WrappedFrame = CurrentFrame;
    if (EndFrame > StartFrame)
    {
        int32 AnimationLength = EndFrame - StartFrame + 1;
        while (WrappedFrame > EndFrame)
        {
            WrappedFrame = StartFrame + ((WrappedFrame - StartFrame) % AnimationLength);
        }
        while (WrappedFrame < StartFrame)
        {
            WrappedFrame = EndFrame - ((StartFrame - WrappedFrame - 1) % AnimationLength);
        }
    }

    return calculateWaveVelocity(location, WrappedFrame);
}

float AWaveHeight::GetSeamBlendFactor(FVector WorldPosition, float BlendWidth)
{
    if (!waveUnifiedMetadata)
    {
        return 1.0f; // No seam blending if metadata not available
    }

    auto* MetadataRow = waveUnifiedMetadata->FindRow<FWaveUnifiedMetadata>("Metadata", "");
    if (!MetadataRow)
    {
        return 1.0f;
    }

    // Convert world position to data space (same as in convertWorldToGridIndices)
    FVector actorPos = GetActorLocation();
    FQuat actorRot = GetActorQuat();
    FVector relativePos = WorldPosition - actorPos;
    FVector localPos = actorRot.Inverse().RotateVector(relativePos);

    // Determine which tile we're in and subtract tile offset
    int32 TileIndexX = 0;
    if (FMath::Abs(WorldOffsetPerTileX) > 0.01f)
    {
        TileIndexX = FMath::RoundToInt((localPos.X - BaseOffsetX) / WorldOffsetPerTileX);
    }

    FVector TileOffset = FVector(
        BaseOffsetX + (TileIndexX * WorldOffsetPerTileX),
        BaseOffsetY + (TileIndexX * WorldOffsetPerTileY),
        0.0f
    );
    FVector localPosWithoutTileOffset = localPos - TileOffset;
    FVector dataPos = localPosWithoutTileOffset / CoordinateScale;

    // Apply X-axis flip if enabled
    if (bFlipXAxis)
    {
        float gridExtentX = (MetadataRow->grid_width - 1) * MetadataRow->step_size;
        float centerX = MetadataRow->grid_start_x + gridExtentX / 2.0f;
        dataPos.X = 2.0f * centerX - dataPos.X;
    }

    // Wrap position within tiling bounds
    float wrappedX = fmod(dataPos.X - MetadataRow->grid_start_x, MetadataRow->tiling_x);
    if (wrappedX < 0) wrappedX += MetadataRow->tiling_x;
    wrappedX += MetadataRow->grid_start_x;

    // Calculate distance to nearest seam edge (min or max)
    float distToMinSeam = FMath::Abs(wrappedX - MetadataRow->seam_min);
    float distToMaxSeam = FMath::Abs(wrappedX - MetadataRow->seam_max);
    float distToNearestSeam = FMath::Min(distToMinSeam, distToMaxSeam);

    // Calculate blend factor: 0.0 at seam, 1.0 at BlendWidth distance or more
    float blendFactor = FMath::Clamp(distToNearestSeam / BlendWidth, 0.0f, 1.0f);

    return blendFactor;
}

void AWaveHeight::GetSeamBoundaries(float& OutSeamMinX, float& OutSeamMaxX)
{
    if (!waveUnifiedMetadata)
    {
        OutSeamMinX = 0.0f;
        OutSeamMaxX = 0.0f;
        return;
    }

    auto* MetadataRow = waveUnifiedMetadata->FindRow<FWaveUnifiedMetadata>("Metadata", "");
    if (!MetadataRow)
    {
        OutSeamMinX = 0.0f;
        OutSeamMaxX = 0.0f;
        return;
    }

    // Return seam boundaries in data space (they're stored directly in metadata)
    OutSeamMinX = MetadataRow->seam_min;
    OutSeamMaxX = MetadataRow->seam_max;
}

// ========================================================================
// UNIFIED WAVE DATA IMPLEMENTATION
// ========================================================================

// Which tile (along the side-scrolling tile axis) a world position most likely belongs to, by
// projecting it onto the per-tile offset in actor-local space. Both tile searches below used to walk a
// FIXED -5..+5 window around the actor's base; the ride moves one tile every ~4.5 s, so ~14 s after
// handoff every ride left that window and the search settled on tile +-5 with a non-zero score, which
// then clamped gridX to the data's edge column. Physics floated on that one stale column (flat,
// n_z~1, crest height) while the InfiniteWaveManager kept the VISIBLE tiles correct - and when the wave
// face passed through the clamped column the board "fell through the water" to the trough
// (phone-2026-09-13-13-41-35, t=37s). Centring the window on this estimate makes the search position-
// relative; the scoring loop still decides the exact tile, this only says where to look.
int32 AWaveHeight::EstimateTileIndex(FVector worldPos) const
{
    const FVector2D TileVec(WorldOffsetPerTileX, WorldOffsetPerTileY);
    const float TileLenSq = TileVec.SizeSquared();
    if (TileLenSq < 0.01f)
    {
        return 0;
    }
    const FVector Local = GetActorQuat().Inverse().RotateVector(worldPos - GetActorLocation());
    const FVector2D Rel(Local.X - BaseOffsetX, Local.Y - BaseOffsetY);
    return FMath::RoundToInt(FVector2D::DotProduct(Rel, TileVec) / TileLenSq);
}

tuple<float,float> AWaveHeight::convertWorldToGridIndices(FVector worldPos, FWaveUnifiedMetadata* Metadata)
{
    // Step 1: Convert from Unreal world space to data-local space
    // Subtract WaveHeight actor position to get position relative to actor
    FVector actorPos = GetActorLocation();
    FQuat actorRot = GetActorQuat();

    // Determine which tile this position is in by trying different tile indices
    // and seeing which one gives us a position in the expected data range
    int32 TileIndexX = 0;
    FVector localPosWithoutTileOffset = FVector::ZeroVector;

    if (FMath::Abs(WorldOffsetPerTileX) > 0.01f)
    {
        // FIXED: Use same tile selection scoring as CalculateFrameOffsetForPosition()
        // Score based on gridX position AFTER all transforms (including X-flip), not local space position
        // This ensures both functions select the same tile for the same world position

        float bestScore = FLT_MAX;
        int32 bestTileX = 0;

        // Try different tiles and see which one gives valid grid coordinates
        static int32 TileSearchLogCounter = 0;
        bool bLogThisSearch = (TileSearchLogCounter < 50 && worldPos.X > 8000.0f && worldPos.X < 8500.0f);

        // Search the tiles around the projected estimate (same window as CalculateFrameOffsetForPosition)
        const int32 centreTile = EstimateTileIndex(worldPos);
        for (int32 testTileX = centreTile - kTileSearchRadius; testTileX <= centreTile + kTileSearchRadius; testTileX++)
        {
            // Calculate tile offset in LOCAL space
            FVector testTileLocalOffset = FVector(
                BaseOffsetX + (testTileX * WorldOffsetPerTileX),
                BaseOffsetY + (testTileX * WorldOffsetPerTileY),
                0.0f
            );

            // Backward transformation (inverse of forward):
            // Forward: WorldPos = ActorPos + Rotate(dataLocalPos) + Rotate(TileLocalOffset)
            // This is NOT the same as: WorldPos = ActorPos + Rotate(dataLocalPos + TileLocalOffset)
            // Because: Rotate(A) + Rotate(B) ≠ Rotate(A + B) when rotation is non-identity
            //
            // Correct backward:
            // WorldPos - ActorPos = Rotate(dataLocalPos) + Rotate(TileLocalOffset)
            // Rotate^-1(WorldPos - ActorPos) = dataLocalPos + TileLocalOffset
            // dataLocalPos = ActorRot.Inverse().RotateVector(WorldPos - ActorPos - TileWorldOffset)
            FVector testTileWorldOffset = actorRot.RotateVector(testTileLocalOffset);
            FVector testRelativePos = worldPos - actorPos - testTileWorldOffset;
            FVector testLocalPosWithoutTile = actorRot.Inverse().RotateVector(testRelativePos);

            // Convert to data space
            FVector testDataPos = testLocalPosWithoutTile / CoordinateScale;

            // Apply X-axis flip if enabled (MUST apply before scoring!)
            if (bFlipXAxis)
            {
                float gridExtentX = (Metadata->grid_width - 1) * Metadata->step_size;
                float centerX = Metadata->grid_start_x + gridExtentX / 2.0f;
                testDataPos.X = 2.0f * centerX - testDataPos.X;
            }

            // Calculate grid position (AFTER all transforms)
            float testGridX = (testDataPos.X - Metadata->grid_start_x) / Metadata->step_size;

            // Score based on how far outside valid grid range [0, grid_width-1]
            // This matches CalculateFrameOffsetForPosition() scoring logic
            float score = 0.0f;
            if (testGridX < 0.0f)
                score = -testGridX;
            else if (testGridX > (Metadata->grid_width - 1))
                score = testGridX - (Metadata->grid_width - 1);
            // else score stays 0.0f (within valid range)

            if (bLogThisSearch && bEnableCoordinateLogging)
            {
                UE_LOG(LogSurf, Warning, TEXT("[TILE SEARCH] testTileX=%d: testLocalPos=(%.2f,%.2f), testGridX=%.2f, score=%.2f"),
                    testTileX, testLocalPosWithoutTile.X, testLocalPosWithoutTile.Y, testGridX, score);
            }

            if (score < bestScore)
            {
                bestScore = score;
                bestTileX = testTileX;
                localPosWithoutTileOffset = testLocalPosWithoutTile;
            }
        }

        if (bLogThisSearch && bEnableCoordinateLogging)
        {
            UE_LOG(LogSurf, Warning, TEXT("[TILE SEARCH] RESULT: bestTileX=%d, bestScore=%.2f, localPosWithoutTileOffset=(%.2f,%.2f)"),
                bestTileX, bestScore, localPosWithoutTileOffset.X, localPosWithoutTileOffset.Y);
            TileSearchLogCounter++;
        }

        TileIndexX = bestTileX;
    }
    else
    {
        // No tiling - just do direct transformation
        FVector relativePos = worldPos - actorPos;
        localPosWithoutTileOffset = actorRot.Inverse().RotateVector(relativePos);
    }

    // DEBUG: Log tile calculation details
    if (bEnableCoordinateLogging)
    {
        UE_LOG(LogSurf, Warning, TEXT("[TILE CALC] World=(%.2f,%.2f) BaseOffset=(%.2f,%.2f) WorldOffsetPerTile=(%.2f,%.2f) -> TileIndexX=%d"),
            worldPos.X, worldPos.Y, BaseOffsetX, BaseOffsetY, WorldOffsetPerTileX, WorldOffsetPerTileY, TileIndexX);
        UE_LOG(LogSurf, Warning, TEXT("[TILE CALC] localPosWithoutTileOffset=(%.2f,%.2f,%.2f)"),
            localPosWithoutTileOffset.X, localPosWithoutTileOffset.Y, localPosWithoutTileOffset.Z);
    }

    // Scale by CoordinateScale (accounts for GridLODActor scale)
    // CoordinateScale adjusts for the scale difference between Unreal and Blender export
    // - 1.0 = no scaling (Unreal world units match Blender)
    // - 0.1 = Blender units are 10x smaller than Unreal (common with GridLODActor scale 0.1)
    FVector dataPos = localPosWithoutTileOffset / CoordinateScale;

    // DEBUG: Log data space conversion
    if (bEnableCoordinateLogging)
    {
        UE_LOG(LogSurf, Warning, TEXT("[DATA CONV] CoordinateScale=(%.4f,%.4f,%.4f) -> dataPos=(%.2f,%.2f,%.2f)"),
            CoordinateScale.X, CoordinateScale.Y, CoordinateScale.Z, dataPos.X, dataPos.Y, dataPos.Z);
    }

    // Apply X-axis flip if enabled (mirror horizontally)
    if (bFlipXAxis)
    {
        float gridExtentX = (Metadata->grid_width - 1) * Metadata->step_size;
        float centerX = Metadata->grid_start_x + gridExtentX / 2.0f;
        float originalDataPosX = dataPos.X;
        dataPos.X = 2.0f * centerX - dataPos.X;  // Mirror around center

        if (bEnableCoordinateLogging)
        {
            UE_LOG(LogSurf, Warning, TEXT("[FLIP X] bFlipXAxis=true: dataPos.X %.2f -> %.2f (mirrored around center %.2f)"),
                originalDataPosX, dataPos.X, centerX);
        }
    }
    else if (bEnableCoordinateLogging)
    {
        UE_LOG(LogSurf, Warning, TEXT("[FLIP X] bFlipXAxis=false: dataPos.X unchanged = %.2f"), dataPos.X);
    }

    // FIXED: Do NOT apply modulo wrapping for X or Y coordinates
    // The forward transformation (Grid → Blender → World) does NOT wrap
    // The backward transformation (World → Blender → Grid) should NOT wrap either
    // Grid coordinates can exceed the physical grid bounds due to tiling
    // The wrapping happens when actually sampling the wave data, not when calculating indices

    // OLD BROKEN CODE (kept for reference):
    // float tiledX = fmod(dataPos.X - Metadata->grid_start_x, Metadata->tiling_x);
    // if (tiledX < 0) tiledX += Metadata->tiling_x;
    // tiledX += Metadata->grid_start_x;
    // float tiledY = fmod(dataPos.Y - Metadata->grid_start_y, Metadata->tiling_y);
    // if (tiledY < 0) tiledY += Metadata->tiling_y;
    // tiledY += Metadata->grid_start_y;

    // Convert directly to grid indices (fractional for interpolation)
    float gridX = (dataPos.X - Metadata->grid_start_x) / Metadata->step_size;
    float gridY = (dataPos.Y - Metadata->grid_start_y) / Metadata->step_size;

    // DEBUG: Log final grid index calculation
    if (bEnableCoordinateLogging)
    {
        UE_LOG(LogSurf, Warning, TEXT("[GRID CALC] dataPos=(%.2f,%.2f) grid_start=(%.2f,%.2f) step_size=%.2f -> gridX=%.2f gridY=%.2f"),
            dataPos.X, dataPos.Y, Metadata->grid_start_x, Metadata->grid_start_y, Metadata->step_size, gridX, gridY);
    }

    // DEBUG: Log conversion details with Y-coordinate step-by-step trace
    // Filter to only show positions near cyan sphere area (same filter as earlier logs)
    if (bEnableCoordinateLogging)
    {
        bool bNearCyanSphere = (worldPos.X >= 8200.0f && worldPos.X <= 8600.0f &&
                                worldPos.Y >= 1350.0f && worldPos.Y <= 1600.0f);
        static int32 ConversionLogCounter = 0;
        if (bNearCyanSphere && ConversionLogCounter < 50)
        {
            // Recalculate intermediate values for logging (FIXED transformation)
            FVector finalTileLocalOffset = FVector(
                BaseOffsetX + (TileIndexX * WorldOffsetPerTileX),
                BaseOffsetY + (TileIndexX * WorldOffsetPerTileY),
                0.0f
            );
            FVector finalTileWorldOffset = actorRot.RotateVector(finalTileLocalOffset);
            FVector relativePos = worldPos - actorPos - finalTileWorldOffset;

            UE_LOG(LogSurf, Warning, TEXT("[Y-TRACE] ========== Y-coordinate transformation for World=(%.2f,%.2f) =========="), worldPos.X, worldPos.Y);
            UE_LOG(LogSurf, Warning, TEXT("[Y-TRACE]   1. Input World Y: %.2f"), worldPos.Y);
            UE_LOG(LogSurf, Warning, TEXT("[Y-TRACE]   2. Actor Y: %.2f"), actorPos.Y);
            UE_LOG(LogSurf, Warning, TEXT("[Y-TRACE]   3. TileLocalOffset: (%.2f, %.2f)"), finalTileLocalOffset.X, finalTileLocalOffset.Y);
            UE_LOG(LogSurf, Warning, TEXT("[Y-TRACE]   4. TileWorldOffset (rotated): (%.2f, %.2f)"), finalTileWorldOffset.X, finalTileWorldOffset.Y);
            UE_LOG(LogSurf, Warning, TEXT("[Y-TRACE]   5. Relative Y (World - Actor - TileWorld): %.2f"), relativePos.Y);
            UE_LOG(LogSurf, Warning, TEXT("[Y-TRACE]   6. After inverse rotation (Local Y): %.2f"), localPosWithoutTileOffset.Y);
            UE_LOG(LogSurf, Warning, TEXT("[Y-TRACE]   8. Data space Y (Local / CoordinateScale.Y): %.2f / %.2f = %.2f"),
                localPosWithoutTileOffset.Y, CoordinateScale.Y, dataPos.Y);
            UE_LOG(LogSurf, Warning, TEXT("[Y-TRACE]   9. FIXED: No wrapping applied - using dataPos.Y directly"));
            UE_LOG(LogSurf, Warning, TEXT("[Y-TRACE]  10. Final Grid Y: (%.2f - %.2f) / %.2f = %.2f"),
                dataPos.Y, Metadata->grid_start_y, Metadata->step_size, gridY);
            UE_LOG(LogSurf, Warning, TEXT("[Y-TRACE] === For reference: gridX=%.2f ==="), gridX);
            ConversionLogCounter++;
        }
    }

    return make_tuple(gridX, gridY);
}

int32 AWaveHeight::CalculateFrameOffsetForPosition(FVector worldPos, FWaveUnifiedMetadata* Metadata)
{
    // FIXED: Use same tile search logic as convertWorldToGridIndices
    // Key insight: Must subtract TileWorldOffset BEFORE inverse rotation, not TileLocalOffset AFTER
    FVector actorPos = GetActorLocation();
    FQuat actorRot = GetActorQuat();

    // Data grid offset (from Blender space to local space)
    float DataGridOffsetX = Metadata->grid_start_x * CoordinateScale.X;
    float DataGridOffsetY = Metadata->grid_start_y * CoordinateScale.Y;

    // Search for the correct tile by testing nearby tiles
    // We find the tile that places the position within valid data bounds
    const int32 CentreTile = EstimateTileIndex(worldPos);
    int32 BestTileX = CentreTile;
    float BestScore = FLT_MAX;
    int32 ZeroScoreTiles = 0;

    for (int32 testTileX = CentreTile - kTileSearchRadius; testTileX <= CentreTile + kTileSearchRadius; testTileX++)
    {
        // Calculate tile offset in local space
        FVector testTileLocalOffset = FVector(
            BaseOffsetX + (testTileX * WorldOffsetPerTileX),
            BaseOffsetY + (testTileX * WorldOffsetPerTileY),  // Uses testTileX for both (side-scrolling)
            0.0f
        );

        // Rotate to world space
        FVector testTileWorldOffset = actorRot.RotateVector(testTileLocalOffset);

        // Subtract in WORLD space BEFORE inverse rotation (this is the key fix!)
        FVector testRelativePos = worldPos - actorPos - testTileWorldOffset;
        FVector testLocalPosWithoutTile = actorRot.Inverse().RotateVector(testRelativePos);

        // Convert to data space
        FVector dataPos = testLocalPosWithoutTile / CoordinateScale;

        // Apply X-axis flip if enabled
        if (bFlipXAxis)
        {
            float grid_extent_x = (Metadata->grid_width - 1) * Metadata->step_size;
            float center_x = Metadata->grid_start_x + grid_extent_x / 2.0f;
            dataPos.X = 2.0f * center_x - dataPos.X;
        }

        // Calculate grid position
        float gridX = (dataPos.X - Metadata->grid_start_x) / Metadata->step_size;

        // Score based on how reasonable the grid X position is
        // We expect gridX to be roughly in range [0, grid_width-1] for valid data
        float score = 0.0f;
        if (gridX < 0)
        {
            score = -gridX;
        }
        else if (gridX > (Metadata->grid_width - 1))
        {
            score = gridX - (Metadata->grid_width - 1);
        }
        else
        {
            score = 0.0f;  // Within valid range
        }

        if (score == 0.0f) { ZeroScoreTiles++; }

        if (score < BestScore)
        {
            BestScore = score;
            BestTileX = testTileX;
        }
    }

    // DIAGNOSTIC (specs/flat-water-propulsion-audit.md, slope-noise investigation).
    // The chosen tile sets the sampled ANIMATION FRAME via FrameOffsetPerTileX, so if more than one
    // tile scores 0 the winner is decided by loop order alone and a sub-centimetre move can flip it,
    // swapping the wave surface by 80 frames. Count the ambiguity rather than guess at it.
#if !UE_BUILD_SHIPPING
    if (SurfDebug::IsFlagSet(TEXT("tilejump")))
    {
        UE_LOG(LogSurf, Warning, TEXT("TILESEL: pos=(%.0f, %.0f) chosenTile=%d zeroScoreTiles=%d bestScore=%.4f frameOffset=%d"),
            worldPos.X, worldPos.Y, BestTileX, ZeroScoreTiles, BestScore,
            ReferenceFrameOffset + (BestTileX * FrameOffsetPerTileX));
    }
#endif

    int32 TileIndexX = BestTileX;
    int32 TileIndexY = 0;

    // Calculate frame offset based on tile position
    int32 FrameOffset = ReferenceFrameOffset + (TileIndexX * FrameOffsetPerTileX) + (TileIndexY * FrameOffsetPerTileY);

    if (bEnableCoordinateLogging)
    {
        UE_LOG(LogSurf, Warning, TEXT("[WaveHeight DEBUG] CalculateFrameOffsetForPosition (FIXED):"));
        UE_LOG(LogSurf, Warning, TEXT("  World Position: (%.2f, %.2f, %.2f)"), worldPos.X, worldPos.Y, worldPos.Z);
        UE_LOG(LogSurf, Warning, TEXT("  BaseOffset: (%.2f, %.2f)"), BaseOffsetX, BaseOffsetY);
        UE_LOG(LogSurf, Warning, TEXT("  Tile Indices (FIXED): (%d, %d)"), TileIndexX, TileIndexY);
        UE_LOG(LogSurf, Warning, TEXT("  Frame Offset: %d = %d + (%d * %d) + (%d * %d)"),
            FrameOffset, ReferenceFrameOffset, TileIndexX, FrameOffsetPerTileX, TileIndexY, FrameOffsetPerTileY);
    }

    return FrameOffset;
}

int32 AWaveHeight::getGridIndex(int32 x, int32 y, int32 width)
{
    return y * width + x;
}

float AWaveHeight::getHeightAtGridPoint(int32 gridX, int32 gridY,
    FWaveUnifiedFrameData* FrameData, FWaveUnifiedMetadata* Metadata)
{
    // Clamp to valid grid bounds
    gridX = FMath::Clamp(gridX, 0, Metadata->grid_width - 1);
    gridY = FMath::Clamp(gridY, 0, Metadata->grid_height - 1);

    int32 index = getGridIndex(gridX, gridY, Metadata->grid_width);

    if (FrameData->h.Num() == 0)
    {
        UE_LOG(LogSurf, Error, TEXT("getHeightAtGridPoint - FrameData->h array is EMPTY! Data not loaded?"));
        return 0.0f;
    }

    if (index >= 0 && index < FrameData->h.Num())
    {
        return FrameData->h[index];
    }

    return 0.0f;
}

FVector AWaveHeight::getNormalAtGridPoint(int32 gridX, int32 gridY,
    FWaveUnifiedFrameData* FrameData, FWaveUnifiedMetadata* Metadata)
{
    // Clamp to valid grid bounds
    gridX = FMath::Clamp(gridX, 0, Metadata->grid_width - 1);
    gridY = FMath::Clamp(gridY, 0, Metadata->grid_height - 1);

    int32 index = getGridIndex(gridX, gridY, Metadata->grid_width);

    if (index >= 0 && index < FrameData->nx.Num())
    {
        return FVector(FrameData->nx[index], FrameData->ny[index], FrameData->nz[index]);
    }

    return FVector::UpVector;
}

FVector AWaveHeight::getVelocityAtGridPoint(int32 gridX, int32 gridY,
    FWaveUnifiedFrameData* FrameData, FWaveUnifiedMetadata* Metadata)
{
    // Store original values before clamping
    int32 originalGridX = gridX;
    int32 originalGridY = gridY;

    // Clamp to valid grid bounds
    gridX = FMath::Clamp(gridX, 0, Metadata->grid_width - 1);
    gridY = FMath::Clamp(gridY, 0, Metadata->grid_height - 1);

    int32 index = getGridIndex(gridX, gridY, Metadata->grid_width);

    // Check if velocity data exists and is valid
    if (index >= 0 && index < FrameData->vx.Num() &&
        index < FrameData->vy.Num() && index < FrameData->vz.Num())
    {
        FVector velocity = FVector(FrameData->vx[index], FrameData->vy[index], FrameData->vz[index]);

        // DEBUG: Log if clamping occurred or if velocity is near-zero
        if (bEnableVelocityLogging)
        {
            bool bWasClamped = (originalGridX != gridX || originalGridY != gridY);
            bool bNearZero = velocity.IsNearlyZero(0.01f);
            if (bWasClamped || bNearZero)
            {
                UE_LOG(LogSurf, Warning, TEXT("[VEL SAMPLE] Grid=(%d,%d)->(%d,%d) Index=%d Vel=(%.4f,%.4f,%.4f) %s%s"),
                    originalGridX, originalGridY, gridX, gridY, index,
                    velocity.X, velocity.Y, velocity.Z,
                    bWasClamped ? TEXT("[CLAMPED] ") : TEXT(""),
                    bNearZero ? TEXT("[NEAR-ZERO]") : TEXT(""));
            }
        }

        return velocity;
    }

    if (bEnableVelocityLogging)
    {
        UE_LOG(LogSurf, Warning, TEXT("[VEL SAMPLE] Grid=(%d,%d) Index=%d OUT OF BOUNDS - Returning ZeroVector"),
            gridX, gridY, index);
    }
    return FVector::ZeroVector;
}

FVector AWaveHeight::getVelocityBilinear(float gridX, float gridY,
    FWaveUnifiedFrameData* FrameData, FWaveUnifiedMetadata* Metadata)
{
    // Get the four surrounding grid points
    int32 x0 = FMath::FloorToInt(gridX);
    int32 y0 = FMath::FloorToInt(gridY);
    int32 x1 = x0 + 1;
    int32 y1 = y0 + 1;

    // Calculate interpolation weights
    float fx = gridX - x0;  // Fractional part of x
    float fy = gridY - y0;  // Fractional part of y

    // Clamp to valid grid bounds
    x0 = FMath::Clamp(x0, 0, Metadata->grid_width - 1);
    y0 = FMath::Clamp(y0, 0, Metadata->grid_height - 1);
    x1 = FMath::Clamp(x1, 0, Metadata->grid_width - 1);
    y1 = FMath::Clamp(y1, 0, Metadata->grid_height - 1);

    // Get velocities at the four corners
    int32 idx00 = getGridIndex(x0, y0, Metadata->grid_width);
    int32 idx10 = getGridIndex(x1, y0, Metadata->grid_width);
    int32 idx01 = getGridIndex(x0, y1, Metadata->grid_width);
    int32 idx11 = getGridIndex(x1, y1, Metadata->grid_width);

    // Validate indices
    int32 maxIndex = FrameData->vx.Num();
    if (idx00 >= maxIndex || idx10 >= maxIndex || idx01 >= maxIndex || idx11 >= maxIndex)
    {
        if (bEnableVelocityLogging)
        {
            UE_LOG(LogSurf, Warning, TEXT("[VEL BILINEAR] Out of bounds indices - returning ZeroVector"));
        }
        return FVector::ZeroVector;
    }

    // Sample the four corner velocities
    FVector v00(FrameData->vx[idx00], FrameData->vy[idx00], FrameData->vz[idx00]);
    FVector v10(FrameData->vx[idx10], FrameData->vy[idx10], FrameData->vz[idx10]);
    FVector v01(FrameData->vx[idx01], FrameData->vy[idx01], FrameData->vz[idx01]);
    FVector v11(FrameData->vx[idx11], FrameData->vy[idx11], FrameData->vz[idx11]);

    // Bilinear interpolation:
    // Interpolate along X axis first
    FVector v_y0 = FMath::Lerp(v00, v10, fx);  // Bottom edge
    FVector v_y1 = FMath::Lerp(v01, v11, fx);  // Top edge

    // Then interpolate along Y axis
    FVector result = FMath::Lerp(v_y0, v_y1, fy);

    // DEBUG: Log if we're interpolating between near-zero and normal velocities (dead zone edge)
    if (bEnableVelocityLogging)
    {
        static int32 BilinearLogCounter = 0;
        if (BilinearLogCounter < 20)
        {
            bool hasZero = v00.IsNearlyZero(0.01f) || v10.IsNearlyZero(0.01f) ||
                           v01.IsNearlyZero(0.01f) || v11.IsNearlyZero(0.01f);
            bool hasNonZero = !v00.IsNearlyZero(0.01f) || !v10.IsNearlyZero(0.01f) ||
                              !v01.IsNearlyZero(0.01f) || !v11.IsNearlyZero(0.01f);

            if (hasZero && hasNonZero)
            {
                UE_LOG(LogSurf, Warning, TEXT("[VEL BILINEAR] Interpolating across dead zone - Grid=(%.2f,%.2f) Corners: v00=(%.2f,%.2f,%.2f) v10=(%.2f,%.2f,%.2f) v01=(%.2f,%.2f,%.2f) v11=(%.2f,%.2f,%.2f) Result=(%.2f,%.2f,%.2f)"),
                    gridX, gridY,
                    v00.X, v00.Y, v00.Z,
                    v10.X, v10.Y, v10.Z,
                    v01.X, v01.Y, v01.Z,
                    v11.X, v11.Y, v11.Z,
                    result.X, result.Y, result.Z);
                BilinearLogCounter++;
            }
        }
    }

    return result;
}

tuple<double, FVector> AWaveHeight::waveHeightAndNormalInternal(
    FVector location,
    FWaveUnifiedFrameData* FrameData,
    FWaveUnifiedMetadata* Metadata)
{
    // DEBUG: Log input world position and actor transform
    // Filter: only log if near cyan sphere area (X between 8200-8600, Y between 1350-1600)
    if (bEnableCoordinateLogging)
    {
        bool bNearCyanSphere = (location.X >= 8200.0f && location.X <= 8600.0f &&
                                location.Y >= 1350.0f && location.Y <= 1600.0f);
        static int32 WorldPosLogCounter = 0;
        if (bNearCyanSphere && WorldPosLogCounter < 50)
        {
            FVector actorPos = GetActorLocation();
            FQuat actorRot = GetActorQuat();
            FVector eulerRot = actorRot.Rotator().Euler();

            UE_LOG(LogSurf, Warning, TEXT("[SHARED CALC] World position input: (%.2f, %.2f, %.2f)"), location.X, location.Y, location.Z);
            UE_LOG(LogSurf, Warning, TEXT("[SHARED CALC] ActorPos: (%.2f, %.2f, %.2f)"), actorPos.X, actorPos.Y, actorPos.Z);
            UE_LOG(LogSurf, Warning, TEXT("[SHARED CALC] ActorRot Euler: (%.2f, %.2f, %.2f)"), eulerRot.X, eulerRot.Y, eulerRot.Z);
            UE_LOG(LogSurf, Warning, TEXT("[SHARED CALC] ActorRot Quat: (%.6f, %.6f, %.6f, %.6f)"), actorRot.X, actorRot.Y, actorRot.Z, actorRot.W);

            // Calculate what the position SHOULD be to match cyan sphere Grid (22, 374)
            // Based on cyan sphere: blenderX=41, blenderY=410.85 (after flip)
            float targetBlenderX = 41.0f;
            float targetBlenderY = 410.85f;
            FVector targetDataLocalPos = FVector(targetBlenderX * 28.0f, targetBlenderY * 20.1f, 0);
            FVector targetTileLocalOffset = FVector(-2270.0f, -1100.0f, 0);
            FVector targetLocalWithTile = targetDataLocalPos + targetTileLocalOffset;
            // -90° Z rotation: (X,Y) -> (Y,-X)
            FVector targetWorldOffset = FVector(targetLocalWithTile.Y, -targetLocalWithTile.X, 0);
            FVector targetWorldPos = actorPos + targetWorldOffset;

            UE_LOG(LogSurf, Warning, TEXT("[SHARED CALC] Expected position for Grid(22,374): World=(%.2f,%.2f) vs Actual=(%.2f,%.2f)"),
                targetWorldPos.X, targetWorldPos.Y, location.X, location.Y);
            UE_LOG(LogSurf, Warning, TEXT("[SHARED CALC] Position offset: (%.2f, %.2f)"),
                location.X - targetWorldPos.X, location.Y - targetWorldPos.Y);

            WorldPosLogCounter++;
        }
    }

    // Get fractional grid coordinates with tiling
    auto gridIndices = convertWorldToGridIndices(location, Metadata);
    float gridX = get<0>(gridIndices);
    float gridY = get<1>(gridIndices);

    // DEBUG: Log final grid coordinates and Blender coordinates
    if (bEnableCoordinateLogging)
    {
        bool bNearCyanSphere = (location.X >= 8200.0f && location.X <= 8600.0f &&
                                location.Y >= 1350.0f && location.Y <= 1600.0f);
        static int32 GridCalcLogCounter = 0;
        if (bNearCyanSphere && GridCalcLogCounter < 50)
        {
            // Calculate Blender coordinates from grid coordinates
            float blenderX = Metadata->grid_start_x + gridX * Metadata->step_size;
            float blenderY = Metadata->grid_start_y + gridY * Metadata->step_size;

            UE_LOG(LogSurf, Warning, TEXT("[SHARED CALC GRID] World=(%.2f,%.2f) -> Grid=(%.2f,%.2f) -> Blender=(%.2f,%.2f)"),
                location.X, location.Y, gridX, gridY, blenderX, blenderY);
            GridCalcLogCounter++;
        }
    }

    // Get integer grid positions for bilinear interpolation
    int32 x0 = FMath::FloorToInt(gridX);
    int32 y0 = FMath::FloorToInt(gridY);
    int32 x1 = x0 + 1;
    int32 y1 = y0 + 1;

    // Fractional parts for interpolation
    float fx = gridX - x0;
    float fy = gridY - y0;

    // Get heights at 4 corners
    float h00 = getHeightAtGridPoint(x0, y0, FrameData, Metadata);
    float h10 = getHeightAtGridPoint(x1, y0, FrameData, Metadata);
    float h01 = getHeightAtGridPoint(x0, y1, FrameData, Metadata);
    float h11 = getHeightAtGridPoint(x1, y1, FrameData, Metadata);

    // Bilinear interpolation for height
    float h0 = FMath::Lerp(h00, h10, fx);
    float h1 = FMath::Lerp(h01, h11, fx);
    double height = FMath::Lerp(h0, h1, fy);

    // Apply flip if enabled
    if (bFlipWaveHeight)
        height = -height;

    // Get normals at 4 corners
    FVector n00 = getNormalAtGridPoint(x0, y0, FrameData, Metadata);
    FVector n10 = getNormalAtGridPoint(x1, y0, FrameData, Metadata);
    FVector n01 = getNormalAtGridPoint(x0, y1, FrameData, Metadata);
    FVector n11 = getNormalAtGridPoint(x1, y1, FrameData, Metadata);

    // Bilinear interpolation for normal
    FVector n0 = FMath::Lerp(n00, n10, fx);
    FVector n1 = FMath::Lerp(n01, n11, fx);
    FVector normal = FMath::Lerp(n0, n1, fy);

    // Flip normal Z component if wave height is flipped
    if (bFlipWaveHeight)
        normal.Z = -normal.Z;

    normal.Normalize();

    if (bEnableCoordinateLogging)
    {
        UE_LOG(LogSurf, Warning, TEXT("[WaveHeight DEBUG] waveHeightAndNormalInternal:"));
        UE_LOG(LogSurf, Warning, TEXT("  Grid coords: (%.2f, %.2f) -> corners [%d,%d][%d,%d]"), gridX, gridY, x0, y0, x1, y1);
        UE_LOG(LogSurf, Warning, TEXT("  Heights: h00=%.2f h10=%.2f h01=%.2f h11=%.2f -> %.2f"), h00, h10, h01, h11, height);
        UE_LOG(LogSurf, Warning, TEXT("  Normal: (%.3f, %.3f, %.3f)"), normal.X, normal.Y, normal.Z);
    }

    return make_tuple(height, normal);
}

tuple<double, FVector> AWaveHeight::waveHeightAndNormal(FVector location, int32 frame)
{
    // Get metadata (single "Metadata" row)
    if (!waveUnifiedMetadata)
    {
        UE_LOG(LogSurf, Error, TEXT("waveUnifiedMetadata is not set"));
        return make_tuple(0.0, FVector::UpVector);
    }

    auto MetadataRow = waveUnifiedMetadata->FindRow<FWaveUnifiedMetadata>("Metadata", "");
    if (!MetadataRow)
    {
        UE_LOG(LogSurf, Error, TEXT("Metadata row not found in waveUnifiedMetadata"));
        return make_tuple(0.0, FVector::UpVector);
    }

    // Calculate frame offset based on tile position (for infinite tiling)
    int32 PositionFrameOffset = CalculateFrameOffsetForPosition(location, MetadataRow);
    int32 OffsetFrame = frame + PositionFrameOffset;

    // Wrap frame within StartFrame to EndFrame range (same logic as GridLODActor)
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

    if (bEnableCoordinateLogging)
    {
        UE_LOG(LogSurf, Warning, TEXT("[WaveHeight DEBUG] Frame offset applied: base=%d, offset=%d, final=%d (range %d-%d)"),
            frame, PositionFrameOffset, OffsetFrame, StartFrame, EndFrame);
    }

    // Get frame data (row named "Frame_<number>")
    if (!waveUnifiedData)
    {
        UE_LOG(LogSurf, Error, TEXT("waveUnifiedData is not set"));
        return make_tuple(0.0, FVector::UpVector);
    }

    FString frameName = FString::Printf(TEXT("Frame_%d"), OffsetFrame);
    auto FrameDataRow = waveUnifiedData->FindRow<FWaveUnifiedFrameData>(FName(*frameName), "");
    if (!FrameDataRow)
    {
        UE_LOG(LogSurf, Warning, TEXT("Frame data not found: %s"), *frameName);
        return make_tuple(0.0, FVector::UpVector);
    }

    // Validate data
    int32 expectedSize = MetadataRow->grid_width * MetadataRow->grid_height;
    if (FrameDataRow->h.Num() != expectedSize)
    {
        UE_LOG(LogSurf, Error, TEXT("Frame %s has wrong array size: %d (expected %d)"),
            *frameName, FrameDataRow->h.Num(), expectedSize);
        return make_tuple(0.0, FVector::UpVector);
    }

    return waveHeightAndNormalInternal(location, FrameDataRow, MetadataRow);
}

TArray<FVector> AWaveHeight::calculateWaveLocationAndNormal(FVector location, int32 frame)
{
    if (bEnableCoordinateLogging)
    {
        UE_LOG(LogSurf, Warning, TEXT("[WaveHeight DEBUG] calculateWaveLocationAndNormal:"));
        UE_LOG(LogSurf, Warning, TEXT("  Input location: (%.2f, %.2f, %.2f), Frame: %d"), location.X, location.Y, location.Z, frame);
    }

    auto waveHeightAndNormalResult = this->waveHeightAndNormal(location, frame);
    double waveHeight = get<0>(waveHeightAndNormalResult);
    FVector normal = get<1>(waveHeightAndNormalResult);
    normal.Normalize();
    // Wave data is in actor-local (Blender export) frame; the actor is rotated -90° around Z
    // to align the crest with the Y axis. Rotate the normal to world frame so every consumer
    // (buoyancy, supplement, lateral turn force) gets a correctly oriented vector.
    normal = this->GetActorQuat().RotateVector(normal);

    // Create wave location in Unreal world space
    // waveHeight is in data space (same as Blender export), convert to Unreal scale
    // Keep X,Y from input location, only set Z from wave height
    FVector worldLocation = FVector(location.X, location.Y, waveHeight * CoordinateScale.Z);

    if (bEnableCoordinateLogging)
    {
        UE_LOG(LogSurf, Warning, TEXT("  Wave height (data space): %.2f, Output world location: (%.2f, %.2f, %.2f)"), waveHeight, worldLocation.X, worldLocation.Y, worldLocation.Z);
    }

    // Debug visualization - draw sphere at calculated wave surface position
    if (bShowDebugVisualization)
    {
        UWorld* World = GetWorld();
        if (World)
        {
            DrawDebugSphere(World, worldLocation, 15.0f, 8, FColor::Green, false, 0.5f, 0, 2.0f);
        }
    }

    auto result = TArray<FVector>{
        worldLocation,
        normal
    };
    return result;
}

FVector AWaveHeight::calculateWaveVelocity(FVector location, int32 frame, int32 TileIndexX, int32 TileIndexY)
{
    // Get metadata (single "Metadata" row)
    if (!waveUnifiedMetadata)
    {
        UE_LOG(LogSurf, Error, TEXT("calculateWaveVelocity: waveUnifiedMetadata is not set"));
        return FVector::ZeroVector;
    }

    auto MetadataRow = waveUnifiedMetadata->FindRow<FWaveUnifiedMetadata>("Metadata", "");
    if (!MetadataRow)
    {
        UE_LOG(LogSurf, Error, TEXT("calculateWaveVelocity: Metadata row not found"));
        return FVector::ZeroVector;
    }

    // Calculate frame offset based on tile indices (if provided) or position (fallback)
    int32 PositionFrameOffset;
    int32 CalculatedTileX = TileIndexX;
    int32 CalculatedTileY = TileIndexY;

    if (TileIndexX == -999999)
    {
        // Tile indices not provided (the game path). UNIFIED with the height path: frame offset via
        // CalculateFrameOffsetForPosition (scored tile search) and, below, grid conversion via
        // convertWorldToGridIndices — the exact machinery the height sampling uses. The old hand-rolled
        // rounding + "simplified" conversion here made the velocity grid read ~1-4 cells shoreward of
        // the height grid — see specs/wave-data-velocity-registration.md.
        // CalculatedTileX stays -999999, which routes the grid conversion below to the unified path.
        PositionFrameOffset = CalculateFrameOffsetForPosition(location, MetadataRow);
    }
    else
    {
        // Tile indices provided - use them directly (fixes coordinate confusion bug)
        // Same formula as CalculateFrameOffsetForPosition in commit d01aadc1
        // (tile 0=offset 0, tile -1=offset -95, tile -2=offset -190)
        PositionFrameOffset = ReferenceFrameOffset + (TileIndexX * FrameOffsetPerTileX) + (TileIndexY * FrameOffsetPerTileY);

        if (bEnableVelocityLogging)
        {
            // UNIT TEST: Verify formula produces expected values
            UE_LOG(LogSurf, Warning, TEXT("[UNIT TEST] TileIndexX=%d, ReferenceFrameOffset=%d, FrameOffsetPerTileX=%d, Calculated PositionFrameOffset=%d"),
                TileIndexX, ReferenceFrameOffset, FrameOffsetPerTileX, PositionFrameOffset);
        }
    }
    int32 OffsetFrame = frame + PositionFrameOffset;

    // Wrap frame within StartFrame to EndFrame range (same logic as wave height)
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

    if (bEnableVelocityLogging)
    {
        UE_LOG(LogSurf, Warning, TEXT("[VELOCITY DEBUG] Tile=(%d,%d) Frame: base=%d, offset=%d, final=%d (range %d-%d)"),
            CalculatedTileX, CalculatedTileY, frame, PositionFrameOffset, OffsetFrame, StartFrame, EndFrame);
    }

    // Get frame data (row named "Frame_<number>")
    if (!waveUnifiedData)
    {
        UE_LOG(LogSurf, Error, TEXT("calculateWaveVelocity: waveUnifiedData is not set"));
        return FVector::ZeroVector;
    }

    FString frameName = FString::Printf(TEXT("Frame_%d"), OffsetFrame);
    auto FrameDataRow = waveUnifiedData->FindRow<FWaveUnifiedFrameData>(FName(*frameName), "");
    if (!FrameDataRow)
    {
        UE_LOG(LogSurf, Warning, TEXT("calculateWaveVelocity: Frame data not found: %s"), *frameName);
        return FVector::ZeroVector;
    }

    // Validate velocity data exists
    int32 expectedSize = MetadataRow->grid_width * MetadataRow->grid_height;

    static bool bLoggedOnce = false;
    if (!bLoggedOnce)
    {
        // Check if velocity data has any non-zero values
        int32 nonZeroCount = 0;
        float maxMagnitude = 0.0f;
        for (int32 i = 0; i < FrameDataRow->vx.Num() && i < 100; i++)
        {
            float magnitude = FMath::Sqrt(
                FrameDataRow->vx[i] * FrameDataRow->vx[i] +
                FrameDataRow->vy[i] * FrameDataRow->vy[i] +
                FrameDataRow->vz[i] * FrameDataRow->vz[i]
            );
            if (magnitude > 0.0001f) nonZeroCount++;
            if (magnitude > maxMagnitude) maxMagnitude = magnitude;
        }

        UE_LOG(LogSurf, Warning, TEXT("calculateWaveVelocity: Frame %s velocity array sizes - vx:%d vy:%d vz:%d (expected:%d), NonZero in first 100: %d, MaxMagnitude: %.4f"),
            *frameName, FrameDataRow->vx.Num(), FrameDataRow->vy.Num(), FrameDataRow->vz.Num(), expectedSize, nonZeroCount, maxMagnitude);
        bLoggedOnce = true;
    }

    if (FrameDataRow->vx.Num() != expectedSize ||
        FrameDataRow->vy.Num() != expectedSize ||
        FrameDataRow->vz.Num() != expectedSize)
    {
        UE_LOG(LogSurf, Error, TEXT("calculateWaveVelocity: Frame %s has missing or wrong velocity data size - vx:%d vy:%d vz:%d (expected:%d)"),
            *frameName, FrameDataRow->vx.Num(), FrameDataRow->vy.Num(), FrameDataRow->vz.Num(), expectedSize);
        return FVector::ZeroVector;
    }

    // Grid conversion. Game path (no explicit tile): UNIFIED with the height path via
    // convertWorldToGridIndices, so height and velocity sample pixel-registered grid coordinates
    // (the old "TEMPORARY simplified" conversion here read the velocity field ~1-4 cells shoreward
    // of the height field — specs/wave-data-velocity-registration.md). The explicit-tile path is
    // kept as-is for the debug visualization, which pre-offsets its positions by TileWorldOffset.
    float gridX, gridY;
    if (CalculatedTileX == -999999)
    {
        auto gridIndices = convertWorldToGridIndices(location, MetadataRow);
        gridX = get<0>(gridIndices);
        gridY = get<1>(gridIndices);
    }
    else
    {
        // Legacy explicit-tile conversion (debug visualization only).
        FVector actorPos = GetActorLocation();
        FQuat actorRot = GetActorQuat();
        FVector relativePos = location - actorPos;
        FVector localPos = actorRot.Inverse().RotateVector(relativePos);
        FVector TileLocalOffset = FVector(
            BaseOffsetX + (CalculatedTileX * WorldOffsetPerTileX),
            BaseOffsetY + (CalculatedTileX * WorldOffsetPerTileY),  // Note: uses TileX for Y offset too (matches debug viz)
            0.0f
        );
        localPos = localPos - TileLocalOffset;

        FVector dataPos = localPos / CoordinateScale;

        // Apply X-axis flip if enabled
        if (bFlipXAxis)
        {
            float gridExtentX = (MetadataRow->grid_width - 1) * MetadataRow->step_size;
            float centerX = MetadataRow->grid_start_x + gridExtentX / 2.0f;
            dataPos.X = 2.0f * centerX - dataPos.X;
        }

        gridX = (dataPos.X - MetadataRow->grid_start_x) / MetadataRow->step_size;
        gridY = (dataPos.Y - MetadataRow->grid_start_y) / MetadataRow->step_size;
    }

    // DEBUG: Log coordinates for velocity calculation
    if (bEnableVelocityLogging)
    {
        static int32 VelCoordLogCounter = 0;
        if (VelCoordLogCounter < 10)
        {
            UE_LOG(LogSurf, Warning, TEXT("[VEL COORD] World=(%.2f,%.2f,%.2f) Tile=(%d,%d) Grid=(%.2f,%.2f)"),
                location.X, location.Y, location.Z,
                CalculatedTileX, CalculatedTileY,
                gridX, gridY);
            VelCoordLogCounter++;
        }
    }

    // Sample velocity using bilinear interpolation (smooths over dead zones)
    FVector dataVelocity = getVelocityBilinear(gridX, gridY, FrameDataRow, MetadataRow);

    // DEBUG: Log grid coordinates with velocity to diagnose striping
    if (bEnableVelocityLogging)
    {
        static int32 GridVelLogCounter = 0;
        if (GridVelLogCounter < 100)
        {
            UE_LOG(LogSurf, Warning, TEXT("[GRID VEL] Grid=(%.2f,%.2f) DataVel=(%.2f,%.2f,%.2f)"),
                gridX, gridY,
                dataVelocity.X, dataVelocity.Y, dataVelocity.Z);
            GridVelLogCounter++;
        }
    }

    // Apply velocity-specific scaling (separate from CoordinateScale)
    FVector worldVelocity = FVector(
        dataVelocity.X * VelocityScale.X,
        dataVelocity.Y * VelocityScale.Y,
        dataVelocity.Z * VelocityScale.Z
    );

    // Note: Debug visualization is handled in DrawDebugVisualization() where ActorRot can be applied
    // Don't draw debug arrows here since this function doesn't have access to ActorRot

    return worldVelocity;
}

TArray<TTuple<double, FVector>> AWaveHeight::GetWaveDataAroundLocation(FVector centerLocation, int32 frame, float distanceBetweenPoints, int32 gridSizeX, int32 gridSizeY)
{
    // TODO: Implement this function for particle system support
    // For now, return empty array. Called per-tick by AWaveParticleSystemActor, so the
    // warning is dev-only and fired once to avoid per-frame log spam (and never in Shipping).
#if !UE_BUILD_SHIPPING
    static bool bWarnedNotImplemented = false;
    if (!bWarnedNotImplemented)
    {
        bWarnedNotImplemented = true;
        UE_LOG(LogSurf, Warning, TEXT("GetWaveDataAroundLocation not yet implemented for unified format"));
    }
#endif
    return TArray<TTuple<double, FVector>>();
}
