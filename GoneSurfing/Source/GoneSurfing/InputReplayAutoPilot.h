// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "InputReplayAutoPilot.generated.h"

class ASurfboardPawn;
class AWeightDistribution;
class ASharedCalculations;

/**
 * Replay autopilot that drives an ASurfboardPawn's CurrentWeightOffset from a
 * pre-recorded CSV trace produced by the pawn's input-trace recorder.
 *
 * Dev-only — see specs/input-trace-replay.md. Editor-only at runtime: BeginPlay
 * hard-disables itself when !WITH_EDITOR, so it never runs in a packaged Android
 * build even if a shipped level still has the actor present. Within the editor,
 * if both this and player input are active in the same level they will fight (no
 * runtime gating against player input).
 */
UCLASS()
class GONESURFING_API AInputReplayAutoPilot : public AActor
{
	GENERATED_BODY()

public:
	AInputReplayAutoPilot();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Replay")
	bool enabled = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Replay")
	bool includeInSuite = true;

	/** Trace filename. Resolved against <Project>/Tests/InputTraces/ first, then
	 *  <Project>/Saved/InputTraces/, then taken as an absolute path. Required. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Replay")
	FString TraceFileName;

	/** Pawn whose weight-input state is driven by the replay. If null, the first
	 *  ASurfboardPawn in the world is used. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Replay")
	ASurfboardPawn* TargetPawn = nullptr;

	/** Optional explicit reference to the WeightDistribution actor. Resolved from
	 *  TargetPawn->WeightDistribution at BeginPlay when not set. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Replay")
	AWeightDistribution* WeightDistribution = nullptr;

	/** Surfboard actor (kept for parity with StateTriggerAutoPilot; not used yet,
	 *  but reserved for future force/turn replay extensions). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Replay")
	AActor* surfboard = nullptr;

	/** If true, re-run the tilt deadzone/sensitivity/inversion pipeline using
	 *  TargetPawn's *current* UPROPERTY values. If false, use the parameters
	 *  captured in the trace's metadata header. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Replay")
	bool bUseCurrentTiltParams = true;

	/** If non-empty, the replay also records a trajectory CSV to
	 *  Saved/Tests/latest/<TestName>.csv at end-of-replay (snapshot-test style). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Replay")
	FString TestName;

	/** Quit the editor in -game mode when the trace ends (parallels
	 *  AStateTriggerAutoPilot::bAutoQuitOnComplete). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Replay")
	bool bAutoQuitOnComplete = true;

	/** After the trace's last row, keep applying the final input value and keep
	 *  recording the trajectory for this many seconds before finishing — so the
	 *  post-input de-plane / slide-through is captured in the CSV and force log.
	 *  0 = finish immediately at trace end (default; leaves snapshot baselines and
	 *  on-device behavior unchanged). Overridden at runtime by the `surf.replay.hold`
	 *  CVar when that is set > 0. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Replay")
	float PostTraceHoldSeconds = 0.0f;

	/** Flipped true the tick after the last row of the trace has been applied. */
	UPROPERTY(BlueprintReadOnly, Category = "Replay")
	bool bFinished = false;

protected:
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaTime) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	struct FTraceRow
	{
		float t = 0.0f;
		float tiltPitchDeg = 0.0f;
		float tiltRollDeg = 0.0f;
		float stickX = 0.0f;
		float stickY = 0.0f;
		FString source;
		// Post-mapping weight the WeightDistribution actually received (0.5/0.5 = centred).
		// Only meaningful when bHasWeight; drives mouse-source rows, see ApplyRow.
		bool  bHasWeight = false;
		float weightFront = 0.5f;
		float weightRight = 0.5f;
	};

	TArray<FTraceRow> Trace;
	int32 WeightFrontCol = -1;
	int32 WeightRightCol = -1;
	bool bLoaded = false;
	bool bHandoffSeen = false;
	// Scheduled tuning override (-ReplayOverridesAt=<t> -ReplayOverrides=name=value,...): applied
	// in memory on the first tick with PlaybackTime >= t, so an A/B pair rides IDENTICAL physics
	// up to the event under test and differs only from that tick. Restored in EndPlay.
	float ScheduledOverrideTime = -1.0f;
	TArray<TPair<FName, float>> ScheduledOverrides;
	TArray<TPair<FName, float>> ScheduledOverrideRestore;
	bool bScheduledOverridesApplied = false;
	// -ReplayUseWeights: drive every row from the recorded weight_front/right instead of decoding
	// tilt/stick. For physics A/B this is what the board actually received; the raw-tilt path is
	// for re-tuning the tilt mapping and depends on the PC pawn's tilt params matching the phone's.
	bool bUseRecordedWeights = false;
	// True when -ReplayTrace= forced this replay to run (bypasses the surf.autopilots
	// filter and still allows auto-quit at trace end). Set in BeginPlay.
	bool bTraceOverrideActive = false;
	float PlaybackTime = 0.0f;
	int32 LastRowHint = 0;

	// Parked-board fixture (-ParkBehind=<cm> -ParkCross=<cm> [-ParkNoseDeg=<deg>] [-ParkName=<test>]):
	// on the handoff tick the board is teleported to a spot fixed in the WAVE frame — `behind` cm
	// up the line of the nearest wave's impact point (negative = ahead of the peel, so the lip
	// arrives at 622 cm/s), `cross` cm toward the back of the wave from the break line — set
	// broadside (nose along the line; ParkNoseDeg rotates it toward the back) and stopped. The
	// trace then only supplies weights, so "the wave breaks onto a stopped board" reproduces on a
	// known clock without a recorded ride's takeoff or inputs. One PARK line per recorder sample
	// carries the service's zone/Broken at the board and the board's and the water's cross-shore
	// velocity — the instrument for A1/A5. specs/broken-wave-no-consequences.md T1.
	bool  bParkRequested = false;
	bool  bParked = false;
	float ParkBehindCm = 0.0f;
	float ParkCrossCm = 0.0f;
	float ParkNoseDeg = 0.0f;
	float ParkSpeed = 0.0f;     // -ParkSpeed=<cm/s>: initial velocity along the nose (0 = stopped)
	void ParkBoard();
	void LogParkSample();

	// Parameters captured from the trace's metadata block.
	float RecordedTiltPitchForFull = 18.0f;
	float RecordedTiltRollForFull  = 18.0f;
	float RecordedTiltDeadzoneDeg  = 2.0f;
	bool  bRecordedInvertPitch     = true;
	bool  bRecordedInvertRoll      = false;

	void LoadTrace();
	bool ResolveTracePath(const FString& Name, FString& OutPath) const;
	void ApplyAtTime(float t);
	FVector2D OffsetFromTilt(float PitchDeg, float RollDeg) const;

	// Trajectory recorder (mirrors AStateTriggerAutoPilot, only enabled if
	// TestName is set).
	bool bRecorderActive = false;
	bool bRecorderFlushed = false;
	float TimeSinceLastSample = 0.0f;
	FString RecorderBuffer;
	TArray<ASharedCalculations*> CachedSharedCalcs;
	bool bSharedCalcsResolved = false;

	void ResolveSharedCalcsIfNeeded();
	void RecordTrajectorySample();
	void FlushTrajectoryRecorder();
};
