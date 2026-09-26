// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "SurferAnimInstance.h"  // ESurferAnimState (per-step rider animation)
#include "StateTriggerAutoPilot.generated.h"

class ASharedCalculations;

/**
 * Conditions that must be met to advance to next autopilot step
 */
USTRUCT(BlueprintType)
struct FStateTrigger
{
	GENERATED_BODY()

	/** If true, check pitch angle condition (nose up/down - actually uses Roll due to 90° mesh rotation) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	bool bCheckPitch = false;

	/** Minimum pitch angle (degrees, positive = nose up, negative = nose down) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(EditCondition="bCheckPitch"))
	float MinPitch = -90.0f;

	/** Maximum pitch angle (degrees, positive = nose up, negative = nose down) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(EditCondition="bCheckPitch"))
	float MaxPitch = 90.0f;

	/** If true, check yaw angle condition */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	bool bCheckYaw = false;

	/** Minimum yaw angle (degrees) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(EditCondition="bCheckYaw"))
	float MinYaw = -180.0f;

	/** Maximum yaw angle (degrees) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(EditCondition="bCheckYaw"))
	float MaxYaw = 180.0f;

	/** If true, check roll angle condition */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	bool bCheckRoll = false;

	/** Minimum roll angle (degrees) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(EditCondition="bCheckRoll"))
	float MinRoll = -180.0f;

	/** Maximum roll angle (degrees) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(EditCondition="bCheckRoll"))
	float MaxRoll = 180.0f;

	/** If true, check velocity condition */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	bool bCheckVelocity = false;

	/** Minimum velocity magnitude (cm/s) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(EditCondition="bCheckVelocity"))
	float MinVelocity = 0.0f;

	/** Maximum velocity magnitude (cm/s, negative = no max) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(EditCondition="bCheckVelocity"))
	float MaxVelocity = -1.0f;

	/** If true, check planing amount condition */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	bool bCheckPlaning = false;

	/** Minimum planing amount (0.0 = not planing, 1.0 = fully planing) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(EditCondition="bCheckPlaning", ClampMin="0.0", ClampMax="1.0"))
	float MinPlaning = 0.0f;

	/** Maximum planing amount */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(EditCondition="bCheckPlaning", ClampMin="0.0", ClampMax="1.0"))
	float MaxPlaning = 1.0f;

	/** If true, check wave-slope-under-board condition. Reads waveSlopeDownVec
	 *  magnitude averaged across the surfboard's ASharedCalculations actors
	 *  (same value the recorder writes as the 'slopeSin' CSV column). 0 = flat
	 *  water / trough, ~0.3 = moderate wave face, ~1 = vertical wall. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	bool bCheckSlopeSin = false;

	/** Minimum wave slope (0..1). 0 = no minimum. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(EditCondition="bCheckSlopeSin", ClampMin="0.0", ClampMax="1.0"))
	float MinSlopeSin = 0.0f;

	/** Maximum wave slope (0..1). 1 = no maximum. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(EditCondition="bCheckSlopeSin", ClampMin="0.0", ClampMax="1.0"))
	float MaxSlopeSin = 1.0f;

	/** Maximum time to wait for conditions (seconds, 0 = wait forever) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float TimeoutDuration = 0.0f;
};

/**
 * Autopilot step with state-based trigger conditions
 */
USTRUCT(BlueprintType)
struct FStateTriggerStep
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FString description;

	/** Weight distribution: 0.0 = left, 0.5 = center, 1.0 = right */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(ClampMin = "0.0", ClampMax = "1.0"))
	float weightRight = 0.5;

	/** Weight distribution: 0.0 = tail, 0.5 = center, 1.0 = nose */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(ClampMin = "0.0", ClampMax = "1.0"))
	float weightNose = 0.5;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	bool jetEngineOn = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float jetMultiplier = 1.0;

	/** Scripted rider animation this step drives (paddle / cobra / pop-up). Defaults to Surf so
	 *  untagged steps and the snapshot-test autopilots leave the rider in the stance — only the
	 *  cinematic pop-up steps need re-tagging. Pushed to the mannequin's USurferAnimInstance in
	 *  ApplyStep. See specs/surfer-popup-animation-states.md. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	ESurferAnimState riderAnimState = ESurferAnimState::Surf;

	/** Conditions that must be met for THIS step to become active (Step 0 activates immediately) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FStateTrigger triggerConditions;
};

UCLASS()
class GONESURFING_API AStateTriggerAutoPilot : public AActor
{
	GENERATED_BODY()

public:
	AStateTriggerAutoPilot();

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	bool enabled = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	bool includeInSuite = true;

	/** Current step being executed */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FStateTriggerStep currentStep;

	/** All autopilot steps */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	TArray<FStateTriggerStep> steps;

	/** Starting location for surfboard (zero = don't change) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FVector StartLocation = FVector::ZeroVector;

	/** Starting rotation for surfboard */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FRotator StartRotation = FRotator::ZeroRotator;

	/** Starting linear velocity for the surfboard (cm/s, world space). Zero = legacy behavior
	 *  (velocity is zeroed at start). Non-zero = state injection: the deferred Start() re-applies
	 *  the full start pose + this velocity + StartWaveFrame atomically with step 0 beginning to
	 *  drive. Paste from the START-POSE SNAPSHOT log line of a measured run.
	 *  See specs/skip-paddle-intro.md. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FVector StartVelocity = FVector::ZeroVector;

	/** Starting angular velocity for the surfboard (deg/s, world space). Zero = legacy zeroing.
	 *  Injecting zero spin while the board is pitching on a wave face causes a first-frame
	 *  hitch, so measure it together with StartVelocity. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FVector StartAngularVelocityDeg = FVector::ZeroVector;

	/** If >= 0, written once to WaterController.CurrentFrame in Start() — atomically with the
	 *  full state injection and step 0 beginning to drive, so the measured board state and wave
	 *  phase line up on the same tick. The BP advances CurrentFrame by relative increment, so the
	 *  one-time write sticks and the wave free-runs from here (bManualFrameControl is deliberately
	 *  NOT set). NOT written at BeginPlay: the board must not sit undriven in the live catch
	 *  moment during the ~1s deferred Start, and a BeginPlay jump would leak into filtered test
	 *  runs that force-disable this autopilot. -1 = leave the wave clock alone.
	 *  See specs/skip-paddle-intro.md. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	int32 StartWaveFrame = -1;

	/** Reference to the surfboard actor */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	AActor* surfboard;

	/** Reference to planing calculator actor (optional - only needed if checking planing conditions) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	AActor* planingCalculator;

	/** Initial delay before starting to check trigger conditions (gives board time to settle).
	 *  IGNORED when state injection is configured (see UsesStateInjection): an injected start is
	 *  mid-action by definition, and delaying step advance past the injected moment makes the
	 *  step sequence miss the catch window it was measured against. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float InitialSettleDelay = 2.0f;

	/** Seconds for a full 0->1 weight-distribution sweep when a step activates.
	 *  Going from centered to fully leaned in a single tick tilts the board
	 *  unrealistically fast, so on each step change the active weightRight/
	 *  weightNose are slewed toward the new step's values at (1/this) per second
	 *  instead of snapping. A half-sweep (0.5->1.0) takes half this long.
	 *  0 = instant (old behavior). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(ClampMin = "0.0"))
	float WeightTransitionDuration = 0.4f;

	/** Per-tick debug logging of yaw / roll / velocity checks and all-conditions-met */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	bool bDebugLogging = false;

	/** Optional reference to the WaterController BP. If null, auto-resolved at BeginPlay
	 *  via the first GridLODActor that has one wired up. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	AActor* waterController = nullptr;

	/** If non-empty, this autopilot is a snapshot test. The recorder writes a CSV
	 *  trajectory to Saved/Tests/latest/<TestName>.csv. Compare.ps1 diffs it against
	 *  Tests/baselines/<TestName>.csv to detect physics regressions. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FString TestName;

	/** When the last step activates, call QuitGame in -game mode. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	bool bAutoQuitOnComplete = true;

	/** Flipped true the tick the last step activates. */
	UPROPERTY(BlueprintReadOnly)
	bool bFinished = false;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

public:
	virtual void Tick(float DeltaTime) override;

	/** Start the autopilot sequence */
	UFUNCTION(BlueprintCallable, Category="Surfing")
	void Start();

	/** Check if all conditions for current step are met */
	bool AreConditionsMet(const FStateTrigger& Trigger);

private:
	/** Current step index */
	int32 CurrentStepIndex = 0;

	/** Time spent waiting for current step's conditions */
	float TimeWaitingForConditions = 0.0f;

	/** Time since the last periodic WaterController.CurrentFrame snapshot */
	float TimeSinceLastFrameLog = 0.0f;

	/** Game time accumulated since Start() — used as t-axis in the recorder CSV. */
	float TimeSinceStart = 0.0f;

	/** Time since last recorder sample; sampling cadence = ~50 ms. */
	float TimeSinceLastSample = 0.0f;

	/** Buffered CSV rows; flushed to disk when the last step activates. */
	FString RecorderBuffer;

	/** True after Start() if TestName is non-empty; false otherwise. */
	bool bRecorderActive = false;

	/** True once the CSV has been flushed to disk (so we don't double-write). */
	bool bRecorderFlushed = false;

	/** Cached SharedCalculations actors belonging to this autopilot's surfboard;
	 *  resolved lazily on the first RecordSample call so we don't depend on actor
	 *  spawn order in BeginPlay. */
	UPROPERTY()
	TArray<ASharedCalculations*> CachedSharedCalcs;
	bool bSharedCalcsResolved = false;

	/** Whether autopilot has been started */
	bool bStarted = false;

	/** Resolve CachedSharedCalcs once for this autopilot's surfboard. Called
	 *  by both RecordSample and AreConditionsMet so whichever fires first
	 *  populates the cache. */
	void ResolveSharedCalcsIfNeeded();

	/** Append one trajectory sample to RecorderBuffer. No-op if !bRecorderActive. */
	void RecordSample();

	/** Write RecorderBuffer to Saved/Tests/latest/<TestName>.csv. Idempotent. */
	void FlushRecorder();

	/** Target weight the active step is slewing toward. currentStep.weightRight/
	 *  weightNose ease toward these each Tick over WeightTransitionDuration so the
	 *  board doesn't tilt in a single tick on a step change. */
	float TargetWeightRight = 0.5f;
	float TargetWeightNose  = 0.5f;

	/** Apply a step's settings. When bSnapWeight is true (the initial step on
	 *  Start), weight is set immediately since there's no prior lean to ease from;
	 *  otherwise only the target is set and Tick slews the active weight toward it. */
	void ApplyStep(const FStateTriggerStep& Step, bool bSnapWeight = false);

	/** Resolve the mannequin attached to this autopilot's surfboard and set its scripted rider
	 *  animation state. No-op if the board/mesh/AnimInstance isn't resolvable. Called from ApplyStep
	 *  (per-step) and from BeginPlay (to push step 0 at frame 0, ahead of the deferred Start()). */
	void PushRiderAnimState(ESurferAnimState NewState);

	/** Get surfboard's static mesh component */
	UStaticMeshComponent* GetSurfboardMesh();

	/** Snap the shared surfboard to this autopilot's configured start pose (location if non-zero,
	 *  rotation) and set its velocities (StartVelocity/StartAngularVelocityDeg; zero = legacy
	 *  zeroing). Only the autopilot that will actually drive should call this — every autopilot
	 *  shares one board, so an unconditional snap lets disabled siblings clobber it. */
	void ApplyStartPose();

	/** One-time reflection write of StartWaveFrame to WaterController.CurrentFrame (no-op when
	 *  StartWaveFrame < 0). Called from Start() only — see the StartWaveFrame doc for why not
	 *  BeginPlay. */
	void ApplyStartWaveFrame();

	/** True when any state-injection field is set (specs/skip-paddle-intro.md). Injection means
	 *  Start() re-applies the full measured state atomically with step 0, and InitialSettleDelay
	 *  is skipped — the board starts mid-action, there is nothing to settle. */
	bool UsesStateInjection() const
	{
		return StartWaveFrame >= 0 || !StartVelocity.IsZero() || !StartAngularVelocityDeg.IsZero();
	}

	/** Log the copy-pasteable START-POSE SNAPSHOT line (wave frame + board pose + velocities,
	 *  named after the UPROPERTYs they belong in) for the step that just activated. This is the
	 *  measurement tool for the state-injection fields above — one normal run yields the values
	 *  to paste. See specs/skip-paddle-intro.md. */
	void LogStartPoseSnapshot(int32 StepIndex);
};
