#include "WaveGeoDebugSubsystem.h"
#include "WaveHeight.h"
#include "ParticleSystemsController.h"
#include "SharedCalculations.h"
#include "WaveGeometrySubsystem.h"
#include "InfiniteWaveManager.h"
#include "SurfLog.h"
#include "EngineUtils.h"
#include "DrawDebugHelpers.h"
#include "Kismet/GameplayStatics.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "UnrealClient.h"
#include "HAL/IConsoleManager.h"
#include "Engine/Engine.h"

namespace
{
	// Read directly here (not pushed from a subsystem) so they stick from a launch -ExecCmds.
	static int32  WaveGeo_On = 0;
	static FAutoConsoleVariableRef CVarWaveGeoOn(TEXT("surf.debug.wavegeo"), WaveGeo_On,
		TEXT("Draw the crest line (yellow), the impact point (orange), the whitewater band (cyan) and the foam breaks; log WAVEGEO per tick. 0 = off."));

	static float  WaveGeo_Bore = 1500.0f;
	static FAutoConsoleVariableRef CVarWaveGeoBore(TEXT("surf.debug.wavegeo.bore"), WaveGeo_Bore,
		TEXT("Fresh-foam band used only to pick the foam-derived (orange) impact; the drawn band comes from the service model."));

	static FString WaveGeo_Cam;
	static FAutoConsoleVariableRef CVarWaveGeoCam(TEXT("surf.debug.wavegeo.cam"), WaveGeo_Cam,
		TEXT("Park the view on the shore side of the board: \"dShore/up/dLine[/fov]\" (cm, cm, cm, deg). Empty = leave the pawn's camera."));

	static FString WaveGeo_Shots;
	static FAutoConsoleVariableRef CVarWaveGeoShots(TEXT("surf.debug.wavegeo.shots"), WaveGeo_Shots,
		TEXT("Frames to screenshot: \"start:step:count\" (e.g. 886:24:8) or a comma list (single-quote it in -ExecCmds)."));

	static int32  WaveGeo_ShotLoop = 1;
	static FAutoConsoleVariableRef CVarWaveGeoShotLoop(TEXT("surf.debug.wavegeo.shotloop"), WaveGeo_ShotLoop,
		TEXT("Which pass through the loop takes the screenshots (0 = the first; -1 = every loop)."));

	static int32  WaveGeo_CamRef = 0;
	static FAutoConsoleVariableRef CVarWaveGeoCamRef(TEXT("surf.debug.wavegeo.camref"), WaveGeo_CamRef,
		TEXT("0 = the camera offsets are relative to the board; 1 = relative to the MODEL impact point, looking at it (e.g. cam 0/300/2500: on the impact's path 25 m down the line, looking back up the line at the lip)."));

	static int32  WaveGeo_Foam = 1;
	static FAutoConsoleVariableRef CVarWaveGeoFoam(TEXT("surf.debug.wavegeo.foam"), WaveGeo_Foam,
		TEXT("0 = hide the whitewater: no Niagara particles (culled) and no raw foam debug points, so the rendered lip is visible."));

	static int32  WaveGeo_Thin = 0;
	static FAutoConsoleVariableRef CVarWaveGeoThin(TEXT("surf.debug.wavegeo.thin"), WaveGeo_Thin,
		TEXT("1 = small markers (thin verticals, small spheres) so the wave and the rider under them stay visible."));

	static float  WaveGeo_Zones = 0.0f;
	static FAutoConsoleVariableRef CVarWaveGeoZones(TEXT("surf.debug.wavegeo.zones"), WaveGeo_Zones,
		TEXT("Zone map: cell size in cm (0 = off; 250 is a good start). Points on the water around the board, coloured by the wave-geometry service's zone - green shoulder, yellow pocket, white whitewater, blue flat, grey behind - plus every wave's impact point (magenta) and a legend."));

	static float  WaveGeo_ZonesAlong = 7500.0f;
	static FAutoConsoleVariableRef CVarWaveGeoZonesAlong(TEXT("surf.debug.wavegeo.zones.along"), WaveGeo_ZonesAlong,
		TEXT("Zone map half-extent along the line, cm (7500 = about three tiles each way)."));

	static float  WaveGeo_ZonesAcross = 2500.0f;
	static FAutoConsoleVariableRef CVarWaveGeoZonesAcross(TEXT("surf.debug.wavegeo.zones.across"), WaveGeo_ZonesAcross,
		TEXT("Zone map half-extent cross-shore, cm, centred on the impact point's cross-shore position."));

	FColor WaveGeo_ZoneColor(EWaveZone Z)
	{
		switch (Z)
		{
		case EWaveZone::Shoulder:   return FColor(40, 200, 60);
		case EWaveZone::Pocket:     return FColor(255, 210, 0);
		case EWaveZone::Whitewater: return FColor(255, 255, 255);
		case EWaveZone::Flat:       return FColor(40, 90, 255);
		case EWaveZone::Behind:     return FColor(120, 120, 130);
		default:                    return FColor(255, 0, 255);
		}
	}

	// Lifetime of every draw. A one-frame draw (-1) disappears the moment the world is paused; 0.1 s
	// keeps the picture on screen through a pause so it can be inspected (owner, 2026-09-20).
	static float  WaveGeo_Duration = 0.1f;
	static FAutoConsoleVariableRef CVarWaveGeoDuration(TEXT("surf.debug.wavegeo.duration"), WaveGeo_Duration,
		TEXT("Lifetime (s) of the wave-geometry debug draws; > 0 so they survive a pause. Default 0.1."));

	static int32  WaveGeo_Fix = 0;
	static FAutoConsoleVariableRef CVarWaveGeoFix(TEXT("surf.debug.wavegeo.fix"), WaveGeo_Fix,
		TEXT("1 = anchor the scan, the camera and the profile at the pawn's position on the first tick and keep them there (the wave train passes a fixed point)."));

	// The wave's cross-shore axis from the tile layout, as ASharedCalculations resolves it (copy with a
	// file-unique name: the Android unity build collides anonymous-namespace names across files).
	FVector WaveGeo_ResolveBackDir(UWorld* World)
	{
		if (World)
		{
			for (TActorIterator<AInfiniteWaveManager> It(World); It; ++It)
			{
				if (FMath::Abs(It->ActorSpacing) > KINDA_SMALL_NUMBER)
				{
					FVector back = FVector(It->ActorSpacing, -It->YOffsetPerActor, 0.0f).GetSafeNormal();
					if (!back.IsNearlyZero())
					{
						if (back.X < 0.0f) back = -back;
						return back;
					}
				}
			}
		}
		return FVector(1.0f, 0.0f, 0.0f);
	}

	// The rendered meshes sit ~40 cm below the height data (memory wave-render-meshes-lowered-40cm);
	// the draw is offset by the same so a sphere "on the surface" is on the mesh.
	constexpr float kMeshZBias = -40.0f;

	constexpr float kLineStepCm     = 200.0f;  // spacing of crest samples along the line
	constexpr int32 kLineHalfCount  = 12;      // +-12 samples = +-2400 cm along the line
	constexpr float kCrossStepCm    = 100.0f;  // cross-shore scan step
	constexpr float kCrossRangeCm   = 3000.0f; // +-3000 cm across
	constexpr float kCrestMinProminenceCm = 40.0f;

	bool ParseFloats(const FString& S, TArray<float>& Out)
	{
		// Space, comma or slash separated; slashes survive -ExecCmds (which splits on commas).
		TArray<FString> Parts;
		S.Replace(TEXT("/"), TEXT(" ")).Replace(TEXT(","), TEXT(" ")).ParseIntoArrayWS(Parts);
		Out.Reset();
		for (const FString& P : Parts)
		{
			if (P.IsNumeric()) { Out.Add(FCString::Atof(*P)); }
		}
		return Out.Num() > 0;
	}
}

bool UWaveGeoDebugSubsystem::IsTickable() const
{
	return Super::IsTickable() && WaveGeo_On > 0;
}

void UWaveGeoDebugSubsystem::LookUp()
{
	bLookedUp = true;
	UWorld* World = GetWorld();
	if (!World) { return; }
	for (TActorIterator<AWaveHeight> It(World); It; ++It) { WaveHeight = *It; break; }
	for (TActorIterator<AParticleSystemsController> It(World); It; ++It) { Foam = *It; break; }
	if (AParticleSystemsController* F = Foam.Get())
	{
		// Clusters every tile every tick and draws the raw foam + centroid (green) + front (red).
		F->bShowBreakPointDebug = true;
		F->BreakPointDebugDrawDuration = WaveGeo_Duration; // survives a pause; short enough for no trail
		if (WaveGeo_Thin) { F->bBreakPointDebugDrawSpheres = false; }
		if (WaveGeo_Foam == 0)
		{
			F->bBreakPointDebugDrawFoam = false;
			F->MaxParticlesPerChannel = 1;          // the clustering reads the RAW rows, so it still works
			F->MaxParticleDistanceFromCamera = 1.0f;
		}
	}
	UWaveGeometrySubsystem* Geo = World->GetSubsystem<UWaveGeometrySubsystem>();
	UE_LOG(LogSurf, Warning, TEXT("WAVEGEO: on. WaveHeight=%s Foam=%s bore=%.0f cam='%s' shots='%s' shotloop=%d model=%s (%s)"),
		WaveHeight.IsValid() ? *WaveHeight->GetName() : TEXT("none"),
		Foam.IsValid() ? *Foam->GetName() : TEXT("none"),
		WaveGeo_Bore, *WaveGeo_Cam, *WaveGeo_Shots, WaveGeo_ShotLoop,
		(Geo && Geo->HasModel()) ? TEXT("service") : TEXT("NONE"), Geo ? *Geo->GetModel().Source : TEXT("no subsystem"));
}

FVector UWaveGeoDebugSubsystem::SurfaceAt(const FVector& XY, int32 Frame) const
{
	AWaveHeight* WH = WaveHeight.Get();
	if (!WH) { return XY; }
	const TArray<FVector> R = WH->calculateWaveLocationAndNormal(FVector(XY.X, XY.Y, 0.0f), Frame);
	return R.Num() > 0 ? R[0] : XY;
}

void UWaveGeoDebugSubsystem::UpdateCamera(const FVector& RefPos, const FVector& BackDir, const FVector& LineDir)
{
	const FVector PawnPos = RefPos;
	UWorld* World = GetWorld();
	if (!World || WaveGeo_Cam.IsEmpty()) { return; }
	TArray<float> V;
	if (!ParseFloats(WaveGeo_Cam, V) || V.Num() < 3) { return; }
	const float dShore = V[0], up = V[1], dLine = V[2];
	const float fov = V.Num() > 3 ? V[3] : 70.0f;

	ACameraActor* Cam = Camera.Get();
	if (!Cam)
	{
		FActorSpawnParameters P;
		P.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Cam = World->SpawnActor<ACameraActor>(ACameraActor::StaticClass(), PawnPos, FRotator::ZeroRotator, P);
		Camera = Cam;
		if (Cam && Cam->GetCameraComponent())
		{
			Cam->GetCameraComponent()->SetFieldOfView(fov);
			Cam->GetCameraComponent()->bConstrainAspectRatio = false;
		}
	}
	if (!Cam) { return; }

	const FVector Pos    = PawnPos - BackDir * dShore + LineDir * dLine + FVector(0, 0, up);
	const FVector Target = PawnPos + (WaveGeo_CamRef == 1 ? FVector::ZeroVector : LineDir * 400.0f);
	FRotator Rot = (Target - Pos).Rotation();
	if (FMath::IsNearlyZero(dShore) && FMath::IsNearlyZero(dLine))
	{
		// Straight down, with the line direction up the screen (down the line = screen top).
		Rot = FRotationMatrix::MakeFromXZ(FVector(0, 0, -1), LineDir).Rotator();
	}
	Cam->SetActorLocationAndRotation(Pos, Rot);

	if (APlayerController* PC = World->GetFirstPlayerController())
	{
		if (PC->GetViewTarget() != Cam) { PC->SetViewTarget(Cam); }
	}
}

void UWaveGeoDebugSubsystem::MaybeScreenshot(int32 Frame)
{
	if (WaveGeo_Shots.IsEmpty()) { return; }
	if (WaveGeo_ShotLoop >= 0 && Loop != WaveGeo_ShotLoop) { return; }

	// Target frames; a hitch can skip frame numbers, so a shot fires on the first tick at or past
	// its target (within 12 frames of it), once per loop.
	TArray<int32> Targets;
	TArray<FString> Parts;
	if (WaveGeo_Shots.Contains(TEXT(":")))
	{
		WaveGeo_Shots.ParseIntoArray(Parts, TEXT(":"));
		if (Parts.Num() == 3)
		{
			const int32 Start = FCString::Atoi(*Parts[0]), Step = FMath::Max(1, FCString::Atoi(*Parts[1])), Count = FCString::Atoi(*Parts[2]);
			for (int32 i = 0; i < Count; ++i) { Targets.Add(Start + i * Step); }
		}
	}
	else
	{
		WaveGeo_Shots.ParseIntoArray(Parts, TEXT(","));
		for (const FString& P : Parts) { Targets.Add(FCString::Atoi(*P)); }
	}
	for (const int32 T : Targets)
	{
		if (Frame < T || Frame > T + 12 || ShotsTakenThisLoop.Contains(T)) { continue; }
		ShotsTakenThisLoop.Add(T);
		const FString Name = FString::Printf(TEXT("wavegeo_loop%d_f%04d.png"), Loop, Frame);
		FScreenshotRequest::RequestScreenshot(Name, /*bShowUI*/ false, /*bAddFilenameSuffix*/ false);
		UE_LOG(LogSurf, Warning, TEXT("WAVEGEO: screenshot requested target=%d frame=%d loop=%d -> %s"), T, Frame, Loop, *FScreenshotRequest::GetFilename());
		break;
	}
}

void UWaveGeoDebugSubsystem::Tick(float DeltaTime)
{
	UWorld* World = GetWorld();
	if (!World) { return; }
	if (!bLookedUp) { LookUp(); }
	AWaveHeight* WH = WaveHeight.Get();
	if (!WH) { return; }

	const int32 Frame = WH->GetCurrentFrameFromWaterController();
	if (LastFrame >= 0 && Frame < LastFrame - 50) { ++Loop; ShotsTakenThisLoop.Reset(); }
	LastFrame = Frame;

	// Reference = the board (its SharedCalculations actor), not the player pawn: the pawn is the
	// chase camera, ~9 m behind the crest, and a nearest-peak scan from there latches onto the swell.
	APawn* Pawn = UGameplayStatics::GetPlayerPawn(World, 0);
	FVector PawnPos = Pawn ? Pawn->GetActorLocation() : FVector::ZeroVector;
	for (TActorIterator<ASharedCalculations> It(World); It; ++It) { PawnPos = It->GetActorLocation(); break; }
	if (WaveGeo_Fix > 0)
	{
		if (!bAnchored) { Anchor = PawnPos; bAnchored = true; }
		PawnPos = Anchor; // everything below is relative to the fixed anchor
	}

	// Wave-frame axes: back = cross-shore toward the back of the wave; line = along the crest, oriented
	// down the line (+Y on this level's diagonal tiling).
	const FVector BackDir = WaveGeo_ResolveBackDir(World);
	FVector LineDir = FVector(-BackDir.Y, BackDir.X, 0.0f);
	if (LineDir.Y < 0.0f) { LineDir = -LineDir; }
	auto S = [&](const FVector& P) { return (float)FVector::DotProduct(FVector(P.X, P.Y, 0), LineDir); };
	auto C = [&](const FVector& P) { return (float)FVector::DotProduct(FVector(P.X, P.Y, 0), BackDir); };

	// ---- Crest line: scan across the wave at positions along the line, follow the pawn's wave ----
	struct FCrestSample { FVector Pos; float Offset; bool bValid; };
	TArray<FCrestSample> Crest;
	Crest.SetNum(2 * kLineHalfCount + 1);
	const int32 NCross = 2 * FMath::RoundToInt(kCrossRangeCm / kCrossStepCm) + 1;
	TArray<float> H; H.SetNum(NCross);
	TArray<FVector> Surf; Surf.SetNum(NCross);
	auto ScanAt = [&](int32 i, float FollowOffset, FCrestSample& Out)
	{
		const FVector Base = PawnPos + LineDir * (i * kLineStepCm);
		for (int32 k = 0; k < NCross; ++k)
		{
			const float off = -kCrossRangeCm + k * kCrossStepCm;
			Surf[k] = SurfaceAt(Base + BackDir * off, Frame);
			H[k] = (float)Surf[k].Z;
		}
		// Local maxima with some prominence; pick the one nearest FollowOffset.
		Out.bValid = false;
		float bestD = 1e9f;
		for (int32 k = 1; k < NCross - 1; ++k)
		{
			if (!(H[k] > H[k - 1] && H[k] >= H[k + 1])) { continue; }
			float lo = H[k];
			for (int32 j = FMath::Max(0, k - 8); j <= FMath::Min(NCross - 1, k + 8); ++j) { lo = FMath::Min(lo, H[j]); }
			if (H[k] - lo < kCrestMinProminenceCm) { continue; }
			const float off = -kCrossRangeCm + k * kCrossStepCm;
			const float d = FMath::Abs(off - FollowOffset);
			if (d < bestD) { bestD = d; Out.Pos = Surf[k]; Out.Offset = off; Out.bValid = true; }
		}
	};
	ScanAt(0, 0.0f, Crest[kLineHalfCount]);
	FString ProfileLog;
	for (int32 k = 0; k < NCross; ++k) { ProfileLog += FString::Printf(TEXT(" %.0f"), H[k]); }
	float follow = Crest[kLineHalfCount].bValid ? Crest[kLineHalfCount].Offset : 0.0f;
	for (int32 i = 1; i <= kLineHalfCount; ++i)
	{
		ScanAt(i, follow, Crest[kLineHalfCount + i]);
		if (Crest[kLineHalfCount + i].bValid) { follow = Crest[kLineHalfCount + i].Offset; }
	}
	follow = Crest[kLineHalfCount].bValid ? Crest[kLineHalfCount].Offset : 0.0f;
	for (int32 i = 1; i <= kLineHalfCount; ++i)
	{
		ScanAt(-i, follow, Crest[kLineHalfCount - i]);
		if (Crest[kLineHalfCount - i].bValid) { follow = Crest[kLineHalfCount - i].Offset; }
	}
	const FVector ZBias(0, 0, kMeshZBias);
	for (int32 i = 0; i < Crest.Num(); ++i)
	{
		if (!Crest[i].bValid) { continue; }
		DrawDebugSphere(World, Crest[i].Pos + ZBias, WaveGeo_Thin ? 10.0f : (i == kLineHalfCount ? 45.0f : 30.0f), 8, FColor::Yellow, false, WaveGeo_Duration, 0, WaveGeo_Thin ? 1.5f : 3.0f);
		if (i > 0 && Crest[i - 1].bValid)
		{
			DrawDebugLine(World, Crest[i - 1].Pos + ZBias, Crest[i].Pos + ZBias, FColor::Yellow, false, WaveGeo_Duration, 0, WaveGeo_Thin ? 2.0f : 6.0f);
		}
	}
	const FCrestSample& Crest0 = Crest[kLineHalfCount];
	const float cCrest0 = Crest0.bValid ? C(Crest0.Pos) : C(PawnPos);
	// Crest profile along the line: (c, z) per sample from -kLineHalfCount to +kLineHalfCount.
	FString CrestLog;
	for (int32 i = 0; i < Crest.Num(); ++i)
	{
		CrestLog += Crest[i].bValid ? FString::Printf(TEXT(" %.0f/%.0f"), C(Crest[i].Pos), Crest[i].Pos.Z) : TEXT(" -");
	}

	// ---- Impact point: the furthest-down-line foam front in the band shoreward of this crest ----
	FVector Impact = FVector::ZeroVector;
	bool bImpact = false;
	int32 ImpactCount = 0, NFronts = 0;
	FString FrontsLog;
	if (AParticleSystemsController* F = Foam.Get())
	{
		int32 TileIdx = 0;
		for (const TArray<AParticleSystemsController::FFoamBreakCluster>& Tile : F->GetCachedBreaks())
		{
			for (const AParticleSystemsController::FFoamBreakCluster& B : Tile)
			{
				++NFronts;
				FrontsLog += FString::Printf(TEXT(" t%d:s=%.0f,c=%.0f,n=%d,cs=%.0f,cc=%.0f"), TileIdx, S(B.Front), C(B.Front), B.Count, S(B.Centroid), C(B.Centroid));
				// Fresh foam sits 1-3 m shoreward of the breaking crest; the aging trail drifts further
				// shoreward with the bore. Measured 2026-09-18 (WAVEGEO-FRONTS): fresh fronts at
				// crest-100..-300, older ones at crest-500..-1500, and a new front is born every loop
				// 5138 cm down the line, so the board's wave is the fresh front nearest it along the line.
				// Fresh band only, no more than half a period ahead of the pawn (the previous wave's
				// trail starts 5000 cm on), furthest down the line: a tile's cluster is cut at the tile
				// edge, so the up-line tiles' fronts are edges, not the peel.
				const float cF = C(B.Front);
				if (cF > cCrest0 + 400.0f || cF < cCrest0 - 450.0f || B.Count < 2000) { continue; }
				if (S(B.Front) > S(PawnPos) + 2600.0f) { continue; }
				if (!bImpact || S(B.Front) > S(Impact)) { Impact = B.Front; bImpact = true; ImpactCount = B.Count; }
			}
			++TileIdx;
		}
	}
	if (bImpact)
	{
		const FVector P = SurfaceAt(Impact, Frame) + ZBias;
		const float r = WaveGeo_Thin ? 25.0f : 120.0f;
		DrawDebugSphere(World, P, r, 12, FColor::Orange, false, WaveGeo_Duration, 0, WaveGeo_Thin ? 2.0f : 5.0f);
		DrawDebugLine(World, P, P + FVector(0, 0, 600), FColor::Orange, false, WaveGeo_Duration, 0, WaveGeo_Thin ? 2.0f : 5.0f);
	}

	// ---- Whitewater band: the service's bore band from the MODEL impact (smooth) - drawn below. The
	//      foam front (orange) sits on the clustering's 400 cm cells and hops between tiles; a band
	//      hung on it jumped (owner, 2026-09-20).

	// ---- The service's peel model (specs/wave-geometry.md), drawn magenta to compare with the foam ----
	FVector ModelImpact = FVector::ZeroVector;
	bool bModel = false;
	FWaveGeoSample BoardSample;
	if (UWaveGeometrySubsystem* Geo = World->GetSubsystem<UWaveGeometrySubsystem>())
	{
		BoardSample = Geo->Sample(PawnPos, Frame);
		if (BoardSample.bValid)
		{
			bModel = true;
			ModelImpact = FVector(BoardSample.ImpactWorld.X, BoardSample.ImpactWorld.Y, 0.0f);
			const FVector P = SurfaceAt(ModelImpact, Frame) + ZBias;
			const float r = WaveGeo_Thin ? 20.0f : 100.0f;
			DrawDebugSphere(World, P, r, 12, FColor::Magenta, false, WaveGeo_Duration, 0, WaveGeo_Thin ? 2.0f : 5.0f);
			DrawDebugLine(World, P, P + FVector(0, 0, 500), FColor::Magenta, false, WaveGeo_Duration, 0, WaveGeo_Thin ? 2.0f : 5.0f);
			const float sM = S(ModelImpact), cM = C(ModelImpact);
			for (float s = sM - 3000.0f; s < sM + 3000.0f; s += 300.0f)
			{
				DrawDebugLine(World, SurfaceAt(LineDir * s + BackDir * cM, Frame) + ZBias,
				              SurfaceAt(LineDir * (s + 300.0f) + BackDir * cM, Frame) + ZBias, FColor::Magenta, false, WaveGeo_Duration, 0, WaveGeo_Thin ? 1.5f : 3.0f);
			}
			// The bore band behind this impact, as the service zones it: front and back edges drift
			// shoreward with the age of the bore at each point along the line (FR4 step 2).
			const FWaveGeoModel& M = Geo->GetModel();
			const float peelCmPerSec = FMath::Abs(Geo->PeelSpeedCmPerFrame()) * 24.0f;
			auto Edge = [&](float s, bool bFront)
			{
				const float age = peelCmPerSec > 1.0f ? (sM - s) / peelCmPerSec : 0.0f;
				const float c = cM + (bFront ? -M.BoreFrontStart : M.ImpactToCrestC) - M.BoreDriftRate * age;
				return SurfaceAt(LineDir * s + BackDir * c, Frame) + ZBias;
			};
			const float sEnd = sM - FMath::Min(4000.0f, peelCmPerSec * M.ShoulderAfterSeconds);
			const float w = WaveGeo_Thin ? 2.0f : 4.0f;
			for (float s = sM; s > sEnd; s -= 250.0f)
			{
				const float s2 = FMath::Max(sEnd, s - 250.0f);
				DrawDebugLine(World, Edge(s, true),  Edge(s2, true),  FColor::Cyan, false, WaveGeo_Duration, 0, w);
				DrawDebugLine(World, Edge(s, false), Edge(s2, false), FColor::Cyan, false, WaveGeo_Duration, 0, w);
			}
			DrawDebugLine(World, Edge(sM, true), Edge(sM, false), FColor::Cyan, false, WaveGeo_Duration, 0, w);
			DrawDebugLine(World, Edge(sEnd, true), Edge(sEnd, false), FColor::Cyan, false, WaveGeo_Duration, 0, w);
		}
	}

	// ---- Zone map over several tiles (specs/wave-geometry.md FR5): what every consumer would read ----
	if (WaveGeo_Zones > 0.0f)
	{
		if (UWaveGeometrySubsystem* Geo = World->GetSubsystem<UWaveGeometrySubsystem>(); Geo && Geo->HasModel())
		{
			const float cell = FMath::Max(50.0f, WaveGeo_Zones);
			const float s0 = S(PawnPos);
			// Centre the cross-shore extent on the impact line so the band and the face are both in view.
			const float c0 = bModel ? C(ModelImpact) : C(PawnPos);
			int32 counts[6] = { 0, 0, 0, 0, 0, 0 };
			for (float s = s0 - WaveGeo_ZonesAlong; s <= s0 + WaveGeo_ZonesAlong; s += cell)
			{
				for (float c = c0 - WaveGeo_ZonesAcross; c <= c0 + WaveGeo_ZonesAcross; c += cell)
				{
					const FVector P = LineDir * s + BackDir * c;
					const FWaveGeoSample Smp = Geo->Sample(P, Frame);
					if (!Smp.bValid) { continue; }
					++counts[FMath::Clamp((int32)Smp.Zone, 0, 5)];
					FColor Col = WaveGeo_ZoneColor(Smp.Zone);
					// Whitewater: dim with Broken so the ramp behind the pocket is visible.
					if (Smp.Zone == EWaveZone::Whitewater) { const uint8 v = (uint8)(120 + 135 * Smp.Broken); Col = FColor(v, v, v); }
					DrawDebugPoint(World, SurfaceAt(P, Frame) + ZBias + FVector(0, 0, 12), cell * 0.06f, Col, false, WaveGeo_Duration, 0);
				}
			}
			// Every wave's impact point in range, and the bore band's edges for the age at each s.
			const int32 kNear = Geo->NearestWaveIndex(Frame, s0);
			const int32 span = FMath::CeilToInt(WaveGeo_ZonesAlong / FMath::Max(1.0f, FMath::Abs(Geo->PeriodS()))) + 1;
			for (int32 k = kNear - span; k <= kNear + span; ++k)
			{
				const FVector2D I = Geo->ImpactXY(Frame, k);
				const FVector IP(I.X, I.Y, 0.0f);
				if (FMath::Abs(S(IP) - s0) > WaveGeo_ZonesAlong) { continue; }
				const FVector P = SurfaceAt(IP, Frame) + ZBias;
				DrawDebugSphere(World, P, 60.0f, 12, FColor::Magenta, false, WaveGeo_Duration, 0, 3.0f);
				DrawDebugLine(World, P, P + FVector(0, 0, 800), FColor::Magenta, false, WaveGeo_Duration, 0, 3.0f);
			}
			if (GEngine)
			{
				GEngine->AddOnScreenDebugMessage(3001, WaveGeo_Duration, FColor::White,
					FString::Printf(TEXT("WAVEGEO zones (cell %.0f cm): green=shoulder  yellow=pocket  white=whitewater(brightness=Broken)  blue=flat  grey=behind  magenta=impact points | cells: shoulder %d pocket %d whitewater %d flat %d behind %d"),
						cell, counts[3], counts[4], counts[5], counts[2], counts[1]));
				if (BoardSample.bValid)
				{
					GEngine->AddOnScreenDebugMessage(3002, WaveGeo_Duration, WaveGeo_ZoneColor(BoardSample.Zone),
						FString::Printf(TEXT("board: %s  behind impact %.0f cm  cross %.0f cm  broken %.2f  since broken %.1f s  (frame %d)"),
							UWaveGeometrySubsystem::ZoneName(BoardSample.Zone), BoardSample.DistBehindImpact, BoardSample.CrossFromImpact,
							BoardSample.Broken, BoardSample.SecondsSinceBroken, Frame));
				}
			}
		}
	}

	if (WaveGeo_CamRef == 1 && bModel) { UpdateCamera(SurfaceAt(ModelImpact, Frame), BackDir, LineDir); }
	else                                { UpdateCamera(PawnPos, BackDir, LineDir); }
	MaybeScreenshot(Frame);

	UE_LOG(LogSurf, Warning,
		TEXT("WAVEGEO frame=%d loop=%d back=(%.3f, %.3f) line=(%.3f, %.3f) pawn s=%.0f c=%.0f | crest0 %s c=%.0f h=%.0f at=(%.0f, %.0f) | impact %s s=%.0f c=%.0f at=(%.0f, %.0f) n=%d fronts=%d | model %s s=%.0f c=%.0f"),
		Frame, Loop, BackDir.X, BackDir.Y, LineDir.X, LineDir.Y, S(PawnPos), C(PawnPos),
		Crest0.bValid ? TEXT("ok") : TEXT("none"), cCrest0, Crest0.bValid ? (float)Crest0.Pos.Z : 0.0f, Crest0.Pos.X, Crest0.Pos.Y,
		bImpact ? TEXT("ok") : TEXT("none"), bImpact ? S(Impact) : 0.0f, bImpact ? C(Impact) : 0.0f, Impact.X, Impact.Y, ImpactCount, NFronts,
		bModel ? TEXT("ok") : TEXT("-"), bModel ? S(ModelImpact) : 0.0f, bModel ? C(ModelImpact) : 0.0f);
	if (BoardSample.bValid)
	{
		UE_LOG(LogSurf, Warning, TEXT("WAVEGEO-BOARD frame=%d zone=%s behind=%.0f cross=%.0f broken=%.2f sinceBroken=%.2fs"),
			Frame, UWaveGeometrySubsystem::ZoneName(BoardSample.Zone), BoardSample.DistBehindImpact, BoardSample.CrossFromImpact,
			BoardSample.Broken, BoardSample.SecondsSinceBroken);
	}
	UE_LOG(LogSurf, Warning, TEXT("WAVEGEO-CREST frame=%d pawn_s=%.0f step=%.0f c/z:%s"), Frame, S(PawnPos), kLineStepCm, *CrestLog);
	UE_LOG(LogSurf, Warning, TEXT("WAVEGEO-FRONTS frame=%d%s"), Frame, *FrontsLog);
	// Cross-shore surface-Z profile through the reference point: offsets -kCrossRangeCm..+kCrossRangeCm
	// in kCrossStepCm steps along backDir (+ = toward the back of the wave).
	UE_LOG(LogSurf, Warning, TEXT("WAVEGEO-PROFILE frame=%d ref_s=%.0f ref_c=%.0f range=%.0f step=%.0f z:%s"),
		Frame, S(PawnPos), C(PawnPos), kCrossRangeCm, kCrossStepCm, *ProfileLog);
}
