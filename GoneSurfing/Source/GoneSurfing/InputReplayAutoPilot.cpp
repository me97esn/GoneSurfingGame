// Copyright Epic Games, Inc. All Rights Reserved.

#include "InputReplayAutoPilot.h"
#include "SurfLog.h"
#include "SurfboardPawn.h"
#include "WeightDistribution.h"
#include "SharedCalculations.h"
#include "SurfDebug.h"
#include "SurfTuningSubsystem.h"
#include "WaveGeometrySubsystem.h"
#include "WaveHeight.h"
#include "EngineUtils.h"
#include "Engine/StaticMeshActor.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"

// Runtime override for AInputReplayAutoPilot::PostTraceHoldSeconds. Lets a headless
// run extend the post-trace hold (to capture the de-plane after the input ends) via
// -ExecCmds without editing the level. When > 0 it takes precedence over the UPROPERTY.
static TAutoConsoleVariable<float> CVarReplayHoldSeconds(
	TEXT("surf.replay.hold"),
	0.0f,
	TEXT("Seconds to keep applying the last trace input and recording after a replay trace ends. 0 = use the actor's PostTraceHoldSeconds."),
	ECVF_Default);


namespace
{
	// Parse "# key=value" lines from the metadata block at the top of the CSV.
	// Returns true if a line was consumed as metadata. Unknown keys are ignored.
	bool TryParseMetadataLine(const FString& Line,
		float& OutPitchForFull, float& OutRollForFull, float& OutDeadzone,
		bool& OutInvertPitch, bool& OutInvertRoll)
	{
		if (!Line.StartsWith(TEXT("#"))) return false;

		FString Body = Line.Mid(1).TrimStartAndEnd();
		int32 EqIdx = INDEX_NONE;
		if (!Body.FindChar(TEXT('='), EqIdx)) return true; // unrecognized comment, ignore
		const FString Key = Body.Mid(0, EqIdx).TrimStartAndEnd();
		const FString Val = Body.Mid(EqIdx + 1).TrimStartAndEnd();

		if (Key == TEXT("tilt_pitch_for_full"))      OutPitchForFull = FCString::Atof(*Val);
		else if (Key == TEXT("tilt_roll_for_full"))  OutRollForFull  = FCString::Atof(*Val);
		else if (Key == TEXT("tilt_deadzone_deg"))   OutDeadzone     = FCString::Atof(*Val);
		else if (Key == TEXT("invert_pitch"))        OutInvertPitch  = (Val == TEXT("true"));
		else if (Key == TEXT("invert_roll"))         OutInvertRoll   = (Val == TEXT("true"));
		return true;
	}
}

AInputReplayAutoPilot::AInputReplayAutoPilot()
{
	PrimaryActorTick.bCanEverTick = true;
}

void AInputReplayAutoPilot::BeginPlay()
{
	Super::BeginPlay();

	// Editor-only tuning tool. WITH_EDITOR is true in the editor and in -game mode
	// launched from the editor binary, but false in a packaged Android build, so this
	// replay never drives the pawn on-device even if a level ships with the actor
	// present. (Belt-and-suspenders on top of the content-level "dev-only" convention.)
#if !WITH_EDITOR
	UE_LOG(LogSurf, Display,
		TEXT("InputReplayAutoPilot[%s]: packaged (non-editor) build — replay disabled."),
		*GetName());
	enabled = false;
	return;
#else

	// -ReplayTrace=<file> command-line override: replay a specific trace on this run,
	// force-enabling regardless of the placed actor's `enabled` flag and bypassing the
	// surf.autopilots filter. Read from the command line (not a CVar) because it must be
	// available HERE in BeginPlay — -ExecCmds CVars aren't applied until the first tick,
	// one frame too late (the trace would already be loaded). Output goes to
	// Saved/Tests/latest/<TestName>.csv (TestName defaults to the trace's base filename).
	// See specs/trace-trajectory-comparison.md.
	// Scheduled tuning override, command line only (same reason as -ReplayTrace: it must exist
	// before the first tick). Commas separate entries, so pass it as one space-free argument —
	// and tell FParse NOT to stop at the first comma (its default separator handling silently
	// dropped every entry after the first until 2026-09-18).
	{
		FString OverrideList;
		if (FParse::Value(FCommandLine::Get(), TEXT("ReplayOverridesAt="), ScheduledOverrideTime)
			&& FParse::Value(FCommandLine::Get(), TEXT("ReplayOverrides="), OverrideList, /*bShouldStopOnSeparator*/ false))
		{
			TArray<FString> Entries;
			OverrideList.TrimQuotes().ParseIntoArray(Entries, TEXT(","), /*CullEmpty*/true);
			for (const FString& Entry : Entries)
			{
				FString Key, Val;
				if (Entry.Split(TEXT("="), &Key, &Val))
				{
					ScheduledOverrides.Emplace(FName(*Key.TrimStartAndEnd()), FCString::Atof(*Val));
				}
			}
			UE_LOG(LogSurf, Display,
				TEXT("InputReplayAutoPilot[%s]: %d tuning override(s) scheduled at trace t=%.3f"),
				*GetName(), ScheduledOverrides.Num(), ScheduledOverrideTime);
		}
	}

	bUseRecordedWeights = FParse::Param(FCommandLine::Get(), TEXT("ReplayUseWeights"));

	// Parked-board fixture (see the header). Both distances are required; a name lets several
	// park variants of the same neutral trace land in their own CSVs.
	{
		FString ParkName;
		bParkRequested = FParse::Value(FCommandLine::Get(), TEXT("ParkBehind="), ParkBehindCm)
			&& FParse::Value(FCommandLine::Get(), TEXT("ParkCross="), ParkCrossCm);
		FParse::Value(FCommandLine::Get(), TEXT("ParkNoseDeg="), ParkNoseDeg);
		FParse::Value(FCommandLine::Get(), TEXT("ParkSpeed="), ParkSpeed);
		if (bParkRequested && FParse::Value(FCommandLine::Get(), TEXT("ParkName="), ParkName))
		{
			TestName = ParkName.TrimQuotes();
		}
	}

	FString OverrideTrace;
	if (FParse::Value(FCommandLine::Get(), TEXT("ReplayTrace="), OverrideTrace))
	{
		OverrideTrace = OverrideTrace.TrimQuotes();
	}
	if (!OverrideTrace.IsEmpty())
	{
		TraceFileName = OverrideTrace;
		enabled = true;
		bTraceOverrideActive = true;
		if (TestName.IsEmpty())
		{
			TestName = FPaths::GetBaseFilename(OverrideTrace);
		}
		UE_LOG(LogSurf, Display,
			TEXT("InputReplayAutoPilot[%s]: -ReplayTrace override -> trace='%s' TestName='%s'"),
			*GetName(), *TraceFileName, *TestName);
	}
	// surf.autopilots filter — same convention as AStateTriggerAutoPilot.
	else if (SurfDebug::CVarAutopilots.GetValueOnGameThread().Len() > 0)
	{
		if (SurfDebug::ShouldRunAutopilot(TestName))
		{
			if (!enabled)
			{
				UE_LOG(LogSurf, Display,
					TEXT("InputReplayAutoPilot[%s]: TestName '%s' matched surf.autopilots filter; force-enabling"),
					*GetName(), *TestName);
				enabled = true;
			}
		}
		else
		{
			UE_LOG(LogSurf, Display,
				TEXT("InputReplayAutoPilot[%s]: TestName '%s' filtered out by surf.autopilots; not starting"),
				*GetName(), *TestName);
			enabled = false;
			return;
		}
	}

	if (!enabled)
	{
		return;
	}

	// Resolve TargetPawn — first explicit, else first ASurfboardPawn in world.
	if (!TargetPawn)
	{
		TArray<AActor*> Found;
		UGameplayStatics::GetAllActorsOfClass(GetWorld(), ASurfboardPawn::StaticClass(), Found);
		if (Found.Num() > 0)
		{
			TargetPawn = Cast<ASurfboardPawn>(Found[0]);
		}
	}

	if (!TargetPawn)
	{
		UE_LOG(LogSurf, Warning, TEXT("InputReplayAutoPilot[%s]: No SurfboardPawn found; disabling."), *GetName());
		enabled = false;
		return;
	}

	if (!WeightDistribution)
	{
		WeightDistribution = TargetPawn->WeightDistribution;
	}

	LoadTrace();

	if (!bLoaded || Trace.Num() == 0)
	{
		UE_LOG(LogSurf, Warning,
			TEXT("InputReplayAutoPilot[%s]: Trace not loaded (file '%s'); disabling."),
			*GetName(), *TraceFileName);
		enabled = false;
		return;
	}

	// Take over weight on the pawn.
	TargetPawn->bExternalWeightOverride = true;

	// Make the pawn tick *after* us so the offset we write each frame propagates
	// to WeightDistribution in the same tick, not one frame stale. Without this
	// the pawn might tick first, write the prior frame's offset, then we update
	// it — same intra-group ordering hazard the pawn already guards against for
	// StateTriggerAutoPilot and WeightDistribution.
	TargetPawn->AddTickPrerequisiteActor(this);

	bRecorderActive = !TestName.IsEmpty();
	if (bRecorderActive)
	{
		RecorderBuffer = TEXT("t,gameSeconds,frame,x,y,z,vx,vy,vz,roll,pitch,yaw,step,slopeSin,planing,underwater\n");
		UE_LOG(LogSurf, Display,
			TEXT("InputReplayAutoPilot[%s]: Trajectory recorder ACTIVE -> Saved/Tests/latest/%s.csv"),
			*GetName(), *TestName);
	}

	UE_LOG(LogSurf, Display,
		TEXT("InputReplayAutoPilot[%s]: Replay STARTED — %d rows, duration=%.2fs, source=%s, useCurrentTiltParams=%s"),
		*GetName(), Trace.Num(),
		Trace.Last().t,
		Trace.Num() > 0 ? *Trace[0].source : TEXT("?"),
		bUseCurrentTiltParams ? TEXT("true") : TEXT("false"));
#endif // WITH_EDITOR
}

void AInputReplayAutoPilot::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (!enabled || !bLoaded || bFinished || !TargetPawn)
	{
		return;
	}

	// Defer playback until the pawn signals handoff. The recorded t=0 corresponds
	// to the moment OnPlayerControlsEnabled() fired on the phone (where tilt
	// calibration + trace recording both kicked off); the PC needs to wait for
	// the same condition so the trace's wave-catch context lines up. While the
	// intro autopilot is still running, do not advance PlaybackTime and do not
	// touch CurrentWeightOffset — the intro autopilot's BP mirror is what should
	// be driving WeightDistribution during that phase.
	if (!TargetPawn->IsPlayerControlsEnabled())
	{
		return;
	}

	if (!bHandoffSeen)
	{
		bHandoffSeen = true;
		// Rails fixture (-RailsTrace + -RailsUntil): the pawn carried the board along the recording's
		// own pose track and released it into physics partway through the trace. Playback has to pick
		// up at THAT instant, not at t=0 - otherwise the board is at trace t=3.2 receiving the inputs
		// from t=0 and the fixture is worse than no fixture. < 0 means the rails never ran.
		// See specs/replay-rails-until.md.
		const float RailsReleaseT = TargetPawn->GetRailsReleaseTraceTime();
		if (RailsReleaseT >= 0.0f)
		{
			PlaybackTime = RailsReleaseT;
			LastRowHint = 0;
			UE_LOG(LogSurf, Display,
				TEXT("InputReplayAutoPilot[%s]: Handoff observed — resuming playback at trace t=%.3f (rails fixture)"),
				*GetName(), PlaybackTime);
		}
		else
		{
			UE_LOG(LogSurf, Display,
				TEXT("InputReplayAutoPilot[%s]: Handoff observed — starting playback (trace t=0 aligns to now)"),
				*GetName());
		}
		if (bParkRequested)
		{
			ParkBoard();
		}
	}

	PlaybackTime += DeltaTime;
	TimeSinceLastSample += DeltaTime;

	const float LastT = Trace.Last().t;
	if (PlaybackTime >= LastT)
	{
		// Past the last row. Hold the final input value (so the board keeps the last
		// commanded weight rather than snapping mid-interpolation) and, if a hold
		// window is configured, keep recording for a few extra seconds so the
		// post-input de-plane / slide-through lands in the CSV + force log. The CVar
		// overrides the UPROPERTY so a headless run can enable this without a umap edit.
		ApplyAtTime(LastT);

		const float CVarHold = CVarReplayHoldSeconds.GetValueOnGameThread();
		const float HoldSeconds = CVarHold > 0.0f ? CVarHold : PostTraceHoldSeconds;
		if (PlaybackTime < LastT + HoldSeconds)
		{
			// Still inside the hold window: keep the weight override engaged and keep
			// sampling the trajectory at the normal cadence; don't finish yet.
			if (bRecorderActive && TimeSinceLastSample >= 0.05f)
			{
				TimeSinceLastSample -= 0.05f;
				RecordTrajectorySample();
				LogParkSample();
			}
			return;
		}

		if (bRecorderActive && !bRecorderFlushed)
		{
			RecordTrajectorySample();
			FlushTrajectoryRecorder();
		}

		bFinished = true;
		TargetPawn->bExternalWeightOverride = false;

		UE_LOG(LogSurf, Display,
			TEXT("InputReplayAutoPilot[%s]: Replay FINISHED at t=%.2fs (hold=%.2fs)"),
			*GetName(), PlaybackTime, HoldSeconds);

		// Match StateTriggerAutoPilot's gate: only auto-quit when an explicit
		// autopilot filter is set (test runs), so the packaged Android app
		// doesn't QuitGame on the player when a trace replay happens to be
		// configured in the level.
		const bool bRunningFilteredAutopilots = SurfDebug::CVarAutopilots.GetValueOnGameThread().Len() > 0;
		if (bAutoQuitOnComplete && (bRunningFilteredAutopilots || bTraceOverrideActive) && GetWorld() && GetWorld()->WorldType == EWorldType::Game)
		{
			UE_LOG(LogSurf, Display, TEXT(":::::: -game mode + autopilot/replay-trace active, calling QuitGame"));
			UKismetSystemLibrary::QuitGame(GetWorld(), nullptr, EQuitPreference::Quit, false);
		}
		return;
	}

	// Lands on exactly one physics tick; the FluidDynamics actors re-read the subsystem in
	// setup() every tick, so the new value is in force from this tick's impulses on.
	if (!bScheduledOverridesApplied && ScheduledOverrides.Num() > 0 && PlaybackTime >= ScheduledOverrideTime)
	{
		bScheduledOverridesApplied = true;
		if (USurfTuningSubsystem* Tuning = SurfTuning::Get(this))
		{
			for (const TPair<FName, float>& KV : ScheduledOverrides)
			{
				const float Old = Tuning->GetByName(KV.Key);
				ScheduledOverrideRestore.Emplace(KV.Key, Old);
				Tuning->SetTransient(KV.Key, KV.Value);
				UE_LOG(LogSurf, Warning, TEXT("REPLAY-OVERRIDE [%s] t=%.3f %s %g -> %g"),
					*GetName(), PlaybackTime, *KV.Key.ToString(), Old, KV.Value);
			}
		}
	}

	ApplyAtTime(PlaybackTime);

	if (bRecorderActive && TimeSinceLastSample >= 0.05f)
	{
		TimeSinceLastSample -= 0.05f;
		RecordTrajectorySample();
		LogParkSample();
	}
}

void AInputReplayAutoPilot::ParkBoard()
{
	bParked = false;
	UWorld* World = GetWorld();
	UWaveGeometrySubsystem* Geo = World ? World->GetSubsystem<UWaveGeometrySubsystem>() : nullptr;
	UStaticMeshComponent* Mesh = (TargetPawn && TargetPawn->SurfboardActor)
		? TargetPawn->SurfboardActor->FindComponentByClass<UStaticMeshComponent>() : nullptr;
	AWaveHeight* WH = nullptr;
	if (World) { for (TActorIterator<AWaveHeight> It(World); It; ++It) { WH = *It; break; } }
	if (!Geo || !Geo->HasModel() || !Mesh || !WH)
	{
		UE_LOG(LogSurf, Error, TEXT("PARK [%s]: cannot park - geo=%s model=%s mesh=%s waveHeight=%s"),
			*GetName(), Geo ? TEXT("ok") : TEXT("none"), (Geo && Geo->HasModel()) ? TEXT("ok") : TEXT("NONE"),
			Mesh ? TEXT("ok") : TEXT("none"), WH ? TEXT("ok") : TEXT("none"));
		return;
	}

	// The spot, in the wave frame of the wave nearest the board right now. DistBehindImpact is
	// ImpactS - S, so "behind" moves DOWN in s; CrossFromImpact is C - ImpactC, so "cross" moves
	// UP in c (toward the back). The peel of this wave reaches s = ImpactS when behind = 0.
	const int32   Frame   = Geo->CurrentFrame();
	const FVector BoardPos = Mesh->GetComponentLocation();
	const int32   K       = Geo->NearestWaveIndex(Frame, Geo->S(BoardPos));
	const FVector2D Impact = Geo->ImpactXY(Frame, K);
	const FVector ImpactW(Impact.X, Impact.Y, 0.0f);
	const float   TargetS = Geo->S(ImpactW) - ParkBehindCm;
	const float   TargetC = Geo->C(ImpactW) + ParkCrossCm;
	const FVector XY      = Geo->World(TargetS, TargetC, 0.0f);
	const TArray<FVector> Surf = WH->calculateWaveLocationAndNormal(FVector(XY.X, XY.Y, 0.0f), Frame);
	const FVector Spot = Surf.Num() > 0 ? Surf[0] : FVector(XY.X, XY.Y, BoardPos.Z);

	// Broadside = nose along the line. The mesh's forwards is local +Y, so the actor yaw is the
	// nose heading minus 90 (P1.2 of the spec: nose = board_yaw + 90). ParkNoseDeg rotates the nose
	// from the line toward the back of the wave.
	const FVector Line = Geo->LineDir();
	const FVector Back = Geo->BackDir();
	const float   TowardBackSign = (FVector::CrossProduct(Line, Back).Z > 0.0f) ? 1.0f : -1.0f;
	const float   NoseYaw  = FMath::RadiansToDegrees(FMath::Atan2(Line.Y, Line.X)) + TowardBackSign * ParkNoseDeg;
	const FRotator Rot(0.0f, NoseYaw - 90.0f, 0.0f);

	Mesh->SetWorldLocationAndRotation(Spot, Rot, /*bSweep*/false, nullptr, ETeleportType::TeleportPhysics);
	const FVector NoseDir(FMath::Cos(FMath::DegreesToRadians(NoseYaw)), FMath::Sin(FMath::DegreesToRadians(NoseYaw)), 0.0f);
	Mesh->SetPhysicsLinearVelocity(NoseDir * ParkSpeed);
	Mesh->SetPhysicsAngularVelocityInDegrees(FVector::ZeroVector);
	bParked = true;

	const FWaveGeoSample Smp = Geo->Sample(Spot, Frame);
	UE_LOG(LogSurf, Warning,
		TEXT("PARK [%s]: frame=%d wave=%d impact=(%.0f,%.0f) behind=%.0f cross=%.0f noseDeg=%.0f speed=%.0f -> pos=(%.0f,%.0f,%.0f) yaw=%.1f | zone=%s broken=%.2f sinceBroken=%.2fs | lip due in %.2fs"),
		*GetName(), Frame, K, Impact.X, Impact.Y, ParkBehindCm, ParkCrossCm, ParkNoseDeg, ParkSpeed,
		Spot.X, Spot.Y, Spot.Z, Rot.Yaw, UWaveGeometrySubsystem::ZoneName(Smp.Zone), Smp.Broken, Smp.SecondsSinceBroken,
		-Smp.SecondsSinceBroken);
}

void AInputReplayAutoPilot::LogParkSample()
{
	if (!bParked) { return; }
	UWorld* World = GetWorld();
	UWaveGeometrySubsystem* Geo = World ? World->GetSubsystem<UWaveGeometrySubsystem>() : nullptr;
	UStaticMeshComponent* Mesh = (TargetPawn && TargetPawn->SurfboardActor)
		? TargetPawn->SurfboardActor->FindComponentByClass<UStaticMeshComponent>() : nullptr;
	if (!Geo || !Mesh) { return; }
	ResolveSharedCalcsIfNeeded();
	const ASharedCalculations* SC = CachedSharedCalcs.Num() > 0 ? CachedSharedCalcs[0] : nullptr;

	const int32   Frame = Geo->CurrentFrame();
	const FVector Pos   = Mesh->GetComponentLocation();
	const FVector Vel   = Mesh->GetPhysicsLinearVelocity();
	const FVector Back  = Geo->BackDir();
	const FVector Line  = Geo->LineDir();
	const FVector WaterVel = SC ? SC->absoluteWaterVelocity : FVector::ZeroVector;
	const FWaveGeoSample Smp = Geo->Sample(Pos, Frame);
	// Cross-shore components: + = toward the back of the wave, so a shove toward the shore is
	// negative for both the board and the water. Along-line: + = down the line.
	UE_LOG(LogSurf, Warning,
		TEXT("PARK-TICK t=%.2f frame=%d zone=%s behind=%.0f cross=%.0f broken=%.2f | boardVn=%.0f waterVn=%.0f boardVs=%.0f waterVs=%.0f speed=%.0f | yawRate=%.1f deg/s | z=%.0f underwater=%.2f slopeSin=%.2f"),
		PlaybackTime, Frame, UWaveGeometrySubsystem::ZoneName(Smp.Zone), Smp.DistBehindImpact, Smp.CrossFromImpact, Smp.Broken,
		FVector::DotProduct(Vel, Back), FVector::DotProduct(WaterVel, Back),
		FVector::DotProduct(Vel, Line), FVector::DotProduct(WaterVel, Line), Vel.Size2D(),
		Mesh->GetPhysicsAngularVelocityInDegrees().Z,
		Pos.Z, SC ? SC->amountUnderWater : 0.0f, SC ? (float)SC->waveSlopeDownVec.Size() : 0.0f);
}

void AInputReplayAutoPilot::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (TargetPawn)
	{
		TargetPawn->bExternalWeightOverride = false;
	}
	if (ScheduledOverrideRestore.Num() > 0)
	{
		if (USurfTuningSubsystem* Tuning = SurfTuning::Get(this))
		{
			for (const TPair<FName, float>& KV : ScheduledOverrideRestore)
			{
				Tuning->SetTransient(KV.Key, KV.Value);
			}
		}
		ScheduledOverrideRestore.Reset();
	}
	if (bRecorderActive && !bRecorderFlushed)
	{
		FlushTrajectoryRecorder();
	}
	Super::EndPlay(EndPlayReason);
}

bool AInputReplayAutoPilot::ResolveTracePath(const FString& Name, FString& OutPath) const
{
	if (Name.IsEmpty()) return false;

	// 1) <Project>/Tests/InputTraces/<Name> — checked-in / curated traces.
	{
		const FString Candidate = FPaths::Combine(
			FPaths::ProjectDir(), TEXT("Tests"), TEXT("InputTraces"), Name);
		if (IFileManager::Get().FileExists(*Candidate))
		{
			OutPath = Candidate;
			return true;
		}
	}

	// 2) <Project>/Saved/InputTraces/<Name> — fresh dumps from device pulls.
	{
		const FString Candidate = FPaths::Combine(
			FPaths::ProjectSavedDir(), TEXT("InputTraces"), Name);
		if (IFileManager::Get().FileExists(*Candidate))
		{
			OutPath = Candidate;
			return true;
		}
	}

	// 3) Treat as absolute / project-relative path.
	if (IFileManager::Get().FileExists(*Name))
	{
		OutPath = Name;
		return true;
	}

	return false;
}

void AInputReplayAutoPilot::LoadTrace()
{
	FString ResolvedPath;
	if (!ResolveTracePath(TraceFileName, ResolvedPath))
	{
		UE_LOG(LogSurf, Warning,
			TEXT("InputReplayAutoPilot[%s]: Trace file '%s' not found under Tests/InputTraces, Saved/InputTraces, or as absolute path."),
			*GetName(), *TraceFileName);
		return;
	}

	FString FileContents;
	if (!FFileHelper::LoadFileToString(FileContents, *ResolvedPath))
	{
		UE_LOG(LogSurf, Warning, TEXT("InputReplayAutoPilot[%s]: Failed to read trace '%s'"),
			*GetName(), *ResolvedPath);
		return;
	}

	TArray<FString> Lines;
	FileContents.ParseIntoArrayLines(Lines, /*CullEmpty*/false);

	Trace.Empty(Lines.Num());
	int32 ParsedRows = 0;
	int32 SkippedRows = 0;

	for (int32 LineIdx = 0; LineIdx < Lines.Num(); ++LineIdx)
	{
		FString Line = Lines[LineIdx];
		Line.TrimStartAndEndInline();
		if (Line.IsEmpty()) continue;

		if (Line.StartsWith(TEXT("#")))
		{
			TryParseMetadataLine(Line,
				RecordedTiltPitchForFull, RecordedTiltRollForFull,
				RecordedTiltDeadzoneDeg,
				bRecordedInvertPitch, bRecordedInvertRoll);
			continue;
		}

		// Header row: starts with "t,". Locate the weight columns by name - they were added
		// after the stick columns and their position is not fixed.
		if (Line.StartsWith(TEXT("t,")))
		{
			TArray<FString> HeaderCols;
			Line.ParseIntoArray(HeaderCols, TEXT(","), /*CullEmpty*/false);
			WeightFrontCol = HeaderCols.IndexOfByKey(TEXT("weight_front"));
			WeightRightCol = HeaderCols.IndexOfByKey(TEXT("weight_right"));
			continue;
		}

		TArray<FString> Cols;
		Line.ParseIntoArray(Cols, TEXT(","), /*CullEmpty*/false);
		if (Cols.Num() < 6)
		{
			++SkippedRows;
			UE_LOG(LogSurf, Warning,
				TEXT("InputReplayAutoPilot[%s]: Trace line %d malformed (%d cols, expected 6): '%s'"),
				*GetName(), LineIdx, Cols.Num(), *Line);
			continue;
		}

		FTraceRow Row;
		Row.t            = FCString::Atof(*Cols[0]);
		Row.tiltPitchDeg = FCString::Atof(*Cols[1]);
		Row.tiltRollDeg  = FCString::Atof(*Cols[2]);
		Row.stickX       = FCString::Atof(*Cols[3]);
		Row.stickY       = FCString::Atof(*Cols[4]);
		Row.source       = Cols[5].TrimStartAndEnd();
		if (WeightFrontCol >= 0 && WeightRightCol >= 0
			&& Cols.Num() > FMath::Max(WeightFrontCol, WeightRightCol))
		{
			Row.bHasWeight  = true;
			Row.weightFront = FCString::Atof(*Cols[WeightFrontCol]);
			Row.weightRight = FCString::Atof(*Cols[WeightRightCol]);
		}
		Trace.Add(Row);
		++ParsedRows;
	}

	bLoaded = (Trace.Num() > 0);
	UE_LOG(LogSurf, Display,
		TEXT("InputReplayAutoPilot[%s]: Loaded %d rows (%d skipped) from %s"),
		*GetName(), ParsedRows, SkippedRows, *ResolvedPath);
}

FVector2D AInputReplayAutoPilot::OffsetFromTilt(float PitchDeg, float RollDeg) const
{
	// The deadzone-and-scale is SurfTilt::AxisOffset, the same function the pawn's live tilt path
	// runs. This used to be a verbatim copy of it, commented "replicate ...", which is exactly the
	// arrangement that drifts the first time one side is tuned.
	//
	// Parameters come from either the pawn's *current* UPROPERTYs or those captured in the trace
	// header — that choice is the point of replay, so it stays here.
	const float PitchFull = bUseCurrentTiltParams && TargetPawn
		? TargetPawn->GetEffectiveTiltPitchForFull() : RecordedTiltPitchForFull;
	const float RollFull = bUseCurrentTiltParams && TargetPawn
		? TargetPawn->GetEffectiveTiltRollForFull() : RecordedTiltRollForFull;
	const float Deadzone = bUseCurrentTiltParams && TargetPawn
		? TargetPawn->TiltAngleDeadzoneDegrees : RecordedTiltDeadzoneDeg;
	const bool bInvertPitch = bUseCurrentTiltParams && TargetPawn
		? TargetPawn->bInvertTiltPitch : bRecordedInvertPitch;
	const bool bInvertRoll = bUseCurrentTiltParams && TargetPawn
		? TargetPawn->bInvertTiltRoll : bRecordedInvertRoll;

	FVector2D Offset;
	Offset.Y = SurfTilt::AxisOffset(PitchDeg, PitchFull, Deadzone, bInvertPitch);
	Offset.X = SurfTilt::AxisOffset(RollDeg,  RollFull,  Deadzone, bInvertRoll);
	return Offset;
}

void AInputReplayAutoPilot::ApplyAtTime(float t)
{
	if (Trace.Num() == 0 || !TargetPawn) return;

	// Find the row pair straddling t. Use LastRowHint to amortize the search —
	// in steady playback we advance one row at a time.
	int32 Hi = LastRowHint;
	while (Hi < Trace.Num() && Trace[Hi].t < t) ++Hi;
	if (Hi >= Trace.Num()) Hi = Trace.Num() - 1;
	const int32 Lo = FMath::Max(Hi - 1, 0);
	LastRowHint = Lo;

	const FTraceRow& A = Trace[Lo];
	const FTraceRow& B = Trace[Hi];

	float Alpha = 0.0f;
	if (B.t > A.t)
	{
		Alpha = FMath::Clamp((t - A.t) / (B.t - A.t), 0.0f, 1.0f);
	}

	const float PitchDeg = FMath::Lerp(A.tiltPitchDeg, B.tiltPitchDeg, Alpha);
	const float RollDeg  = FMath::Lerp(A.tiltRollDeg,  B.tiltRollDeg,  Alpha);
	const float StickX   = FMath::Lerp(A.stickX,       B.stickX,       Alpha);
	const float StickY   = FMath::Lerp(A.stickY,       B.stickY,       Alpha);

	FVector2D Offset;
	const bool bFromWeights = A.bHasWeight && B.bHasWeight
		&& (bUseRecordedWeights || A.source == TEXT("mouse"));
	if (bFromWeights)
	{
		// PC mouse: stick_x/y hold the per-frame mouse DELTA the pawn integrates (pixels), not an
		// offset, so replaying them as +-1 gives a no-input ride that just runs down the line.
		// Invert the pawn's offset->weight mapping from the recorded weight instead. That value is
		// post-cap and post-assist, so run these with AssistDisable=1 or the assist is applied twice.
		const float WeightFront = FMath::Lerp(A.weightFront, B.weightFront, Alpha);
		const float WeightRight = FMath::Lerp(A.weightRight, B.weightRight, Alpha);
		Offset = FVector2D((WeightRight - 0.5f) * 2.0f, (WeightFront - 0.5f) * 2.0f);
		Offset.X = FMath::Clamp(Offset.X, -1.0f, 1.0f);
		Offset.Y = FMath::Clamp(Offset.Y, -1.0f, 1.0f);
	}
	else if (A.source == TEXT("tilt"))
	{
		Offset = OffsetFromTilt(PitchDeg, RollDeg);
	}
	else
	{
		// Stick / mouse: the recorded value is already in IA_Weight units (±1).
		Offset = FVector2D(StickX, StickY);
		Offset.X = FMath::Clamp(Offset.X, -1.0f, 1.0f);
		Offset.Y = FMath::Clamp(Offset.Y, -1.0f, 1.0f);
	}

	TargetPawn->SetCurrentWeightOffsetFromReplay(Offset);

	if (SurfDebug::IsFlagSet(TEXT("replay")))
	{
		UE_LOG(LogSurf, Display,
			TEXT("Replay t=%.3f src=%s rawPitch=%.2f rawRoll=%.2f stick=(%.3f,%.3f) -> offset=(%.3f,%.3f) -> amountToTheRight=%.3f amountInFront=%.3f"),
			t, *A.source, PitchDeg, RollDeg, StickX, StickY,
			Offset.X, Offset.Y,
			0.5f + Offset.X * 0.5f, 0.5f + Offset.Y * 0.5f);
	}
}

// ===== Trajectory recorder (snapshot-test parity with StateTriggerAutoPilot) =====

void AInputReplayAutoPilot::ResolveSharedCalcsIfNeeded()
{
	if (bSharedCalcsResolved) return;
	bSharedCalcsResolved = true;

	AActor* TargetSurfboard = surfboard;
	if (!TargetSurfboard && TargetPawn)
	{
		TargetSurfboard = TargetPawn->SurfboardActor.Get();
	}
	if (!TargetSurfboard) return;

	TArray<AActor*> Found;
	UGameplayStatics::GetAllActorsOfClass(GetWorld(), ASharedCalculations::StaticClass(), Found);
	for (AActor* A : Found)
	{
		if (ASharedCalculations* SC = Cast<ASharedCalculations>(A))
		{
			if (SC->Surfboard == TargetSurfboard)
			{
				CachedSharedCalcs.Add(SC);
			}
		}
	}
}

void AInputReplayAutoPilot::RecordTrajectorySample()
{
	if (!TargetPawn) return;

	AActor* TargetSurfboard = surfboard ? surfboard : TargetPawn->SurfboardActor.Get();
	if (!TargetSurfboard) return;

	UStaticMeshComponent* Mesh = TargetSurfboard->FindComponentByClass<UStaticMeshComponent>();
	if (!Mesh) return;

	ResolveSharedCalcsIfNeeded();

	float slopeSinSum = 0.0f;
	float planingSum = 0.0f;
	float underWaterSum = 0.0f;
	int32 nValid = 0;
	for (ASharedCalculations* SC : CachedSharedCalcs)
	{
		if (!SC) continue;
		slopeSinSum   += SC->waveSlopeDownVec.Size();
		planingSum    += SC->AmountPlaning;
		underWaterSum += SC->amountUnderWater;
		++nValid;
	}
	const float slopeSin = (nValid > 0) ? slopeSinSum   / nValid : 0.0f;
	const float planing  = (nValid > 0) ? planingSum    / nValid : 0.0f;
	const float underW   = (nValid > 0) ? underWaterSum / nValid : 0.0f;

	const FVector Pos = Mesh->GetComponentLocation();
	const FVector Vel = Mesh->GetPhysicsLinearVelocity();
	const FRotator Rot = Mesh->GetComponentRotation();

	RecorderBuffer += FString::Printf(
		TEXT("%.3f,%.3f,%d,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.2f,%.2f,%.2f,%d,%.4f,%.3f,%.3f\n"),
		PlaybackTime, /*gameSeconds*/PlaybackTime, /*frame*/0,
		Pos.X, Pos.Y, Pos.Z,
		Vel.X, Vel.Y, Vel.Z,
		Rot.Roll, Rot.Pitch, Rot.Yaw,
		/*step*/0,
		slopeSin, planing, underW);
}

void AInputReplayAutoPilot::FlushTrajectoryRecorder()
{
	if (!bRecorderActive || bRecorderFlushed) return;
	bRecorderFlushed = true;

	const FString Path = FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("Tests"), TEXT("latest"),
		TestName + TEXT(".csv"));

	if (FFileHelper::SaveStringToFile(RecorderBuffer, *Path))
	{
		UE_LOG(LogSurf, Display, TEXT("InputReplayAutoPilot[%s]: Trajectory flushed %d bytes -> %s"),
			*GetName(), RecorderBuffer.Len(), *Path);
	}
	else
	{
		UE_LOG(LogSurf, Warning, TEXT("InputReplayAutoPilot[%s]: Trajectory FAILED to write %s"),
			*GetName(), *Path);
	}
}
