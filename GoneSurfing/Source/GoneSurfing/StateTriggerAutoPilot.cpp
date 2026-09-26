// Fill out your copyright notice in the Description page of Project Settings.

#include "StateTriggerAutoPilot.h"
#include "SurfLog.h"
#include "SurfDebug.h"
#include "SurfRails.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet/GameplayStatics.h"
#include "Engine/World.h"
#include "GridLODActor.h"
#include "SharedCalculations.h"
#include "UObject/UnrealType.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "SurferAnimInstance.h"

namespace
{
	// Read the WaterController BP's CurrentFrame via reflection. Mirrors
	// AGridLODActor::GetCurrentFrameFromController but without the per-tile
	// FrameOffset, so this is the controller's own counter. Returns -1 if
	// the property can't be found.
	int32 ReadWaterControllerFrame(AActor* Controller)
	{
		if (!Controller) return -1;
		UClass* Cls = Controller->GetClass();
		if (!Cls) return -1;
		FProperty* Prop = Cls->FindPropertyByName(TEXT("CurrentFrame"));
		if (!Prop) Prop = Cls->FindPropertyByName(TEXT("Current Frame"));
		if (!Prop) Prop = Cls->FindPropertyByName(TEXT("current_frame"));
		if (!Prop) return -1;
		if (FIntProperty* I = CastField<FIntProperty>(Prop))
			return I->GetPropertyValue_InContainer(Controller);
		if (FFloatProperty* F = CastField<FFloatProperty>(Prop))
			return FMath::RoundToInt(F->GetPropertyValue_InContainer(Controller));
		return -1;
	}

	// Read the WaterController BP's SecondsElapsed (BP-set canonical game-seconds anchor).
	// Returns -1.0f if the property isn't found so callers can detect "not wired up yet".
	float ReadWaterControllerSeconds(AActor* Controller)
	{
		if (!Controller) return -1.0f;
		UClass* Cls = Controller->GetClass();
		if (!Cls) return -1.0f;
		FProperty* Prop = Cls->FindPropertyByName(TEXT("SecondsElapsed"));
		if (!Prop) Prop = Cls->FindPropertyByName(TEXT("Seconds Elapsed"));
		if (!Prop) Prop = Cls->FindPropertyByName(TEXT("seconds_elapsed"));
		if (!Prop) return -1.0f;
		if (FFloatProperty* F = CastField<FFloatProperty>(Prop))
			return F->GetPropertyValue_InContainer(Controller);
		if (FDoubleProperty* D = CastField<FDoubleProperty>(Prop))
			return static_cast<float>(D->GetPropertyValue_InContainer(Controller));
		if (FIntProperty* I = CastField<FIntProperty>(Prop))
			return static_cast<float>(I->GetPropertyValue_InContainer(Controller));
		return -1.0f;
	}
}

AStateTriggerAutoPilot::AStateTriggerAutoPilot()
{
	PrimaryActorTick.bCanEverTick = true;
}

void AStateTriggerAutoPilot::BeginPlay()
{
	Super::BeginPlay();

	// Auto-resolve WaterController via the first GridLODActor that has one wired up.
	if (!waterController)
	{
		TArray<AActor*> Found;
		UGameplayStatics::GetAllActorsOfClass(GetWorld(), AGridLODActor::StaticClass(), Found);
		for (AActor* A : Found)
		{
			if (AGridLODActor* Grid = Cast<AGridLODActor>(A))
			{
				if (Grid->WaterController)
				{
					waterController = Grid->WaterController;
					break;
				}
			}
		}
		UE_LOG(LogSurf, Display, TEXT("StateTriggerAutoPilot: WaterController auto-resolved -> %s"),
			waterController ? *waterController->GetName() : TEXT("(none found)"));
	}

	// Snap the surfboard to its configured start pose NOW, at BeginPlay, instead of inside the
	// deferred Start() below. Start() runs ~1s late (so the surf.autopilots cvar set via -ExecCmds
	// can arrive), and the board simulates physics during that gap — so repositioning in Start()
	// made the board visibly "teleport" a second into play (it settled under physics, then got
	// snapped back to StartLocation). Applying the pose at frame 0 means the board spawns at the
	// start pose and only gently settles before the autopilot takes over. Velocity is re-zeroed in
	// Start() so any settle drift doesn't carry into the driven trajectory.
	//
	// GATE ON `enabled`: every autopilot in the level shares one surfboard, and BeginPlay runs for
	// ALL of them. An unconditional snap let disabled siblings clobber the pose (last BeginPlay wins),
	// so editing the enabled autopilot's StartLocation appeared to do nothing. Only the umap-enabled
	// autopilot snaps here. A test run that force-enables a umap-disabled autopilot via the
	// surf.autopilots filter re-applies the pose in the deferred block below (headless, so the ~1s
	// settle teleport there is invisible and Start() re-zeroes velocity anyway).
	if (enabled)
	{
		// Location/rotation pre-positioning only. The full state injection (velocity + wave-frame
		// jump, specs/skip-paddle-intro.md) happens atomically in the deferred Start() instead:
		//   1. Injecting at BeginPlay left the board simulating the live catch moment for the
		//      ~1s defer with NO autopilot driving it (no step weight, no jet) — it got shoved
		//      over the back of the wave and missed the catch entirely.
		//   2. A BeginPlay wave-clock jump leaks into filtered test runs on the same level: the
		//      filter force-disables this autopilot ~1s in, but the jumped clock would persist
		//      and shift the test's wave timing.
		ApplyStartPose();
		// Push the first step's rider animation NOW, at frame 0, so the mannequin starts in the
		// intro pose (e.g. Paddle) instead of holding the default Surf stance until the ~1s-deferred
		// Start() applies step 0. The rider is cosmetic; if its AnimInstance isn't ready this early
		// the push is a no-op and Start()'s ApplyStep(steps[0]) still covers it.
		if (steps.Num() > 0)
		{
			PushRiderAnimState(steps[0].riderAnimState);
		}
	}

	// Defer Start() so the surf.autopilots cvar (set via -ExecCmds after world BeginPlay) has
	// time to be processed before we check the filter. The 1s wait is only needed on headless
	// runner launches, which are exactly the -unattended ones (same signal the start-screen
	// gate uses). Interactive runs get a next-tick start instead: with state injection the
	// board+wave must not free-run undriven between BeginPlay and Start — a 1s gap showed as
	// "plays for a second, then resets" (the Start()-time injection snapping the drift away).
	// One tick is still enough to guarantee every BeginPlay (WaterController BP included) has
	// run before we write CurrentFrame. Interactive sessions never set the filter via -ExecCmds;
	// a console-set filter persists and is still honored by the check below.
	const float StartDeferSeconds = FApp::IsUnattended() ? 1.0f : 0.05f;
	FTimerHandle DeferHandle;
	FTimerDelegate DeferDelegate;
	DeferDelegate.BindLambda([this]()
	{
		// When a filter is explicitly set:
		//   - matching TestName → force enabled=true (overrides umap default; otherwise
		//     intentionally-disabled-in-umap autopilots like surfing-down-the-line stay off)
		//   - non-matching TestName → force enabled=false
		// Empty filter (no override) → respect umap `enabled`.
		if (SurfDebug::CVarAutopilots.GetValueOnGameThread().Len() > 0)
		{
			if (SurfDebug::ShouldRunAutopilot(this->TestName))
			{
				if (!this->enabled)
				{
					UE_LOG(LogSurf, Display, TEXT("StateTriggerAutoPilot[%s]: TestName '%s' matched surf.autopilots filter; force-enabling (umap default was disabled)"),
						*GetName(), *this->TestName);
					this->enabled = true;
					// BeginPlay's snap only fired for umap-enabled autopilots, so this one's start pose
					// was never applied. Apply it now that the filter has selected it to drive.
					// (State injection, if configured, happens inside Start() below.)
					ApplyStartPose();
				}
			}
			else
			{
				UE_LOG(LogSurf, Display, TEXT("StateTriggerAutoPilot[%s]: TestName '%s' filtered out by surf.autopilots; not starting"),
					*GetName(), *this->TestName);
				this->enabled = false;
				return;
			}
		}
		Start();
	});
	GetWorldTimerManager().SetTimer(DeferHandle, DeferDelegate, StartDeferSeconds, false);
}

void AStateTriggerAutoPilot::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	// Only advance if autopilot is started and enabled
	if (!bStarted || !enabled || steps.Num() == 0)
	{
		return;
	}

	// Recorder: tick the t-axis and sample at ~50ms cadence. Done before the
	// "no more steps" early-return so the final step is still sampled until
	// the autopilot is destroyed (FlushRecorder makes this a no-op afterwards).
	TimeSinceStart += DeltaTime;
	TimeSinceLastSample += DeltaTime;
	if (bRecorderActive && !bRecorderFlushed && TimeSinceLastSample >= 0.05f)
	{
		TimeSinceLastSample -= 0.05f;
		RecordSample();
	}

	// Ease the active weight toward the current step's target instead of snapping.
	// A large step-to-step jump in weightRight/weightNose would tilt the board in a
	// single tick; slew at a bounded rate so BP subclasses (which mirror
	// currentStep.weight* into the WeightDistribution actor every tick) see a smooth
	// ramp. Runs before the "no more steps" return so the final ease-to-center completes.
	if (WeightTransitionDuration > KINDA_SMALL_NUMBER)
	{
		const float interpSpeed = 1.0f / WeightTransitionDuration;
		currentStep.weightRight = FMath::FInterpConstantTo(currentStep.weightRight, TargetWeightRight, DeltaTime, interpSpeed);
		currentStep.weightNose  = FMath::FInterpConstantTo(currentStep.weightNose,  TargetWeightNose,  DeltaTime, interpSpeed);
	}
	else
	{
		currentStep.weightRight = TargetWeightRight;
		currentStep.weightNose  = TargetWeightNose;
	}

	// Check if we've completed all steps
	// Current step is already applied, so we check if there's a NEXT step to activate
	int32 NextStepIndex = CurrentStepIndex + 1;
	if (NextStepIndex >= steps.Num())
	{
		return; // No more steps to activate
	}

	// Track time waiting for conditions (includes settle time)
	TimeWaitingForConditions += DeltaTime;

	// Periodic WaterController.CurrentFrame snapshot for the active step (every 0.5s).
	TimeSinceLastFrameLog += DeltaTime;
	if (TimeSinceLastFrameLog >= 0.5f)
	{
		TimeSinceLastFrameLog -= 0.5f;
		const int32 wcFrame = ReadWaterControllerFrame(waterController);
		const float wcSecs = ReadWaterControllerSeconds(waterController);
		UE_LOG(LogSurf, Display, TEXT("StateTriggerAutoPilot: step %d (%s) t=%.2fs (WaterController.SecondsElapsed=%.2fs CurrentFrame=%d)"),
			CurrentStepIndex,
			steps.IsValidIndex(CurrentStepIndex) ? *steps[CurrentStepIndex].description : TEXT("?"),
			TimeWaitingForConditions, wcSecs, wcFrame);
		// Full paste-ready state at the same cadence, so the state-injection fields can be
		// measured from ANY moment of a run — not just step boundaries. Used to dial in the
		// injection point: inject at the knife-edge catch moment and run-to-run tick noise
		// flips the (bistable) catch branch; inject ~1s+ earlier and the step triggers
		// re-synchronize each run onto the catch, like the un-trimmed intro always did.
		// See specs/skip-paddle-intro.md "Picking the injection point".
		LogStartPoseSnapshot(CurrentStepIndex);
	}

	// Wait for initial settle delay before checking conditions (only for first transition).
	// Skipped for state-injected starts: the board begins mid-action at the measured moment,
	// and holding step 0 for the settle window delays every subsequent step past the catch
	// window the sequence was measured against (observed: "Pop up" ~1s late -> board pitched
	// up 24deg and took the straight-down-the-face branch instead of the carve).
	const float EffectiveSettleDelay = UsesStateInjection() ? 0.0f : InitialSettleDelay;
	if (CurrentStepIndex == 0 && TimeWaitingForConditions < EffectiveSettleDelay)
	{
		return; // Still in settle period, don't check conditions yet
	}

	// Get NEXT step's trigger conditions (this controls when next step becomes active)
	const FStateTrigger& trigger = steps[NextStepIndex].triggerConditions;

	// Check if conditions are met
	bool conditionsMet = AreConditionsMet(trigger);

	// Check for timeout (timeout starts counting AFTER settle delay for first step)
	float timeAfterSettle = (CurrentStepIndex == 0)
		? TimeWaitingForConditions - EffectiveSettleDelay
		: TimeWaitingForConditions;
	bool timedOut = (trigger.TimeoutDuration > 0.0f && timeAfterSettle >= trigger.TimeoutDuration);

	// Emergency fallback: under physics-tuning iteration the board sometimes diverges and
	// misses the trigger window (e.g. pitch never reaches 23deg because the wave loops past
	// the board). Force-advance after 30s so the autopilot completes and a CSV is written.
	const float kEmergencyTimeout = 30.0f;
	if (!timedOut && !conditionsMet && timeAfterSettle >= kEmergencyTimeout)
	{
		UE_LOG(LogSurf, Warning, TEXT("StateTriggerAutoPilot: EMERGENCY FORCE-ADVANCE step %d (%s waited %.1fs, no triggered or timed-out trigger configured)"),
			NextStepIndex, *steps[NextStepIndex].description, timeAfterSettle);
		timedOut = true;
	}

	if (conditionsMet || timedOut)
	{
		const int32 wcFrame = ReadWaterControllerFrame(waterController);
		const float wcSecs = ReadWaterControllerSeconds(waterController);
		if (timedOut)
		{
			UE_LOG(LogSurf, Warning, TEXT("StateTriggerAutoPilot: Activating step %d (%s) - timed out after %.2fs (WaterController.SecondsElapsed=%.2fs CurrentFrame=%d)"),
				NextStepIndex, *steps[NextStepIndex].description, timeAfterSettle, wcSecs, wcFrame);
		}
		else
		{
			UE_LOG(LogSurf, Display, TEXT("StateTriggerAutoPilot: Activating step %d (%s) - conditions met after %.2fs (WaterController.SecondsElapsed=%.2fs CurrentFrame=%d)"),
				NextStepIndex, *steps[NextStepIndex].description, timeAfterSettle, wcSecs, wcFrame);
		}

		// Activate next step
		CurrentStepIndex = NextStepIndex;
		TimeWaitingForConditions = 0.0f;
		TimeSinceLastFrameLog = 0.0f;

		// Apply the step's settings
		ApplyStep(steps[CurrentStepIndex]);
		LogStartPoseSnapshot(CurrentStepIndex);

		// If this was the last step, report completion and (in -game) quit.
		if (CurrentStepIndex == steps.Num() - 1)
		{
			// Capture one final sample at the moment the last step activates,
			// then flush the recorder before the game tears down.
			if (bRecorderActive && !bRecorderFlushed)
			{
				RecordSample();
				FlushRecorder();
			}
			bFinished = true;
			// Hand WeightDistribution back to the player by neutralizing the
			// active step. BP subclasses that mirror currentStep.weightRight/
			// weightNose into the WeightDistribution actor every tick will now
			// push 0.5/0.5, so the last step's lean stops clobbering player
			// input once SurfboardPawn enables controls. Retarget rather than
			// snap so the board eases back to center over WeightTransitionDuration.
			TargetWeightRight = 0.5f;
			TargetWeightNose = 0.5f;
			UE_LOG(LogSurf, Display, TEXT(":::::: StateTriggerAutoPilot completed all steps"));
			// Auto-quit fires only when the surf.autopilots filter cvar is set,
			// which RunGameAndCollectLogs.* sets via -ExecCmds for test runs.
			// Without this extra gate, the packaged Android app (which always
			// runs in EWorldType::Game) would QuitGame right after the intro
			// autopilot's pop-up step — looking like a crash to the player at
			// the exact moment they expect to take control. See specs/pumping.md
			// debugging notes / autopilot suite chain in CLAUDE.md.
			const bool bRunningFilteredAutopilots = SurfDebug::CVarAutopilots.GetValueOnGameThread().Len() > 0;
			const bool bIsGameWorld = GetWorld() && GetWorld()->WorldType == EWorldType::Game;

			// A headless test run must quit, and level data must not be able to veto that.
			//
			// -unattended + a non-empty filter is RunGameAndCollectLogs and nothing else: a human
			// asked for this specific autopilot, headless, from the command line. If the quit does
			// not happen the runner sits for its full 600s timeout and exits 2 with a perfectly good
			// CSV already on disk — which reads as a hang or a failing test, not as a stray bool.
			// That is exactly what both shipped autopilots were doing (measured 2026-08-26: the
			// pop-up and surfing-down-the-line actors both carry bAutoQuitOnComplete=false, against
			// a C++ default of true, so it comes from the shared Blueprint).
			//
			// bAutoQuitOnComplete survives as the opt-out for an INTERACTIVE filtered run — someone
			// driving an autopilot by hand in -game who does not want the app to close under them.
			// The case the original gate existed to protect (a packaged Android build quitting right
			// after the intro pop-up) is untouched: that is neither unattended nor filtered.
			const bool bHeadlessTestRun = FApp::IsUnattended() && bRunningFilteredAutopilots;
			if ((bAutoQuitOnComplete || bHeadlessTestRun) && bRunningFilteredAutopilots && bIsGameWorld)
			{
				UE_LOG(LogSurf, Display, TEXT(":::::: -game mode + autopilot filter active, calling QuitGame%s"),
					(!bAutoQuitOnComplete && bHeadlessTestRun)
						? TEXT(" (bAutoQuitOnComplete is false, but a headless test run overrides it)")
						: TEXT(""));
				UKismetSystemLibrary::QuitGame(GetWorld(), nullptr, EQuitPreference::Quit, false);
			}
			else
			{
				// Say WHICH gate closed. A silent no-quit costs a full 600s runner timeout and then
				// looks like a hang or a test failure, and the three conditions are not guessable
				// from the outside (one is a per-actor property set in the level, one is a cvar that
				// arrives a frame after BeginPlay, one is the world type).
				UE_LOG(LogSurf, Warning,
					TEXT(":::::: NOT quitting after completion — bAutoQuitOnComplete=%s (actor '%s') ")
					TEXT("filterCVar='%s' (needs non-empty) worldType=%d (needs %d=Game). ")
					TEXT("The runner will now sit until its timeout."),
					bAutoQuitOnComplete ? TEXT("true") : TEXT("FALSE"),
					*GetName(),
					*SurfDebug::CVarAutopilots.GetValueOnGameThread(),
					GetWorld() ? (int32)GetWorld()->WorldType : -1,
					(int32)EWorldType::Game);
			}
		}
	}
}

void AStateTriggerAutoPilot::Start()
{
	if (!enabled)
	{
		return;
	}

	// Prevent restarting if already running
	if (bStarted)
	{
		UE_LOG(LogSurf, Warning, TEXT("StateTriggerAutoPilot::Start - Already started, ignoring restart request"));
		return;
	}

	if (steps.Num() == 0)
	{
		UE_LOG(LogSurf, Warning, TEXT("StateTriggerAutoPilot::Start - No steps defined!"));
		return;
	}

	if (!surfboard)
	{
		UE_LOG(LogSurf, Error, TEXT("StateTriggerAutoPilot::Start - No surfboard assigned!"));
		return;
	}

	// State injection (specs/skip-paddle-intro.md) must be ATOMIC with the autopilot starting to
	// drive: the measured pose/velocity/wave-frame describe a live catch moment, and the board
	// only follows the measured trajectory if step 0's weight + jet are driving it from that same
	// tick. Injecting at BeginPlay instead left the board undriven for the ~1s defer — it was
	// shoved over the back of the wave and the catch window (cobra→pop-up is <1s) was gone before
	// Start() ever ran. So: re-apply the full start state NOW, then fall through to ApplyStep(0).
	// The ~1s of divergent drift since BeginPlay is snapped away (~tens of cm, barely visible).
	if (UsesStateInjection())
	{
		ApplyStartPose();
		ApplyStartWaveFrame();
	}
	else
	{
		// Legacy path: pose was applied at BeginPlay (so the board doesn't visibly teleport when
		// this deferred Start() fires); only re-zero velocity to clear any drift the board
		// accumulated while settling during the defer (and so back-to-back runs don't inherit
		// momentum from a prior test).
		UStaticMeshComponent* SurfboardMesh = GetSurfboardMesh();
		if (SurfboardMesh && SurfboardMesh->IsSimulatingPhysics())
		{
			SurfboardMesh->SetPhysicsLinearVelocity(FVector::ZeroVector);
			SurfboardMesh->SetPhysicsAngularVelocityInRadians(FVector::ZeroVector);
		}
	}

	// Initialize state
	CurrentStepIndex = 0;
	TimeWaitingForConditions = 0.0f;
	TimeSinceLastFrameLog = 0.0f;
	TimeSinceStart = 0.0f;
	TimeSinceLastSample = 0.0f;
	bRecorderFlushed = false;
	RecorderBuffer.Empty();
	bRecorderActive = !TestName.IsEmpty();
	bSharedCalcsResolved = false;
	CachedSharedCalcs.Reset();
	bFinished = false;
	if (bRecorderActive)
	{
		RecorderBuffer = TEXT("t,gameSeconds,frame,x,y,z,vx,vy,vz,roll,pitch,yaw,step,slopeSin,planing,underwater\n");
		UE_LOG(LogSurf, Display, TEXT("StateTriggerAutoPilot: Recorder ACTIVE -> Saved/Tests/latest/%s.csv"), *TestName);
	}
	bStarted = true;

	UE_LOG(LogSurf, Display, TEXT("StateTriggerAutoPilot::Start - Starting autopilot with %d steps"), steps.Num());

	// Apply first step immediately — no prior lean to ease from, so snap the weight.
	ApplyStep(steps[0], /*bSnapWeight=*/true);
	LogStartPoseSnapshot(0);
}

bool AStateTriggerAutoPilot::AreConditionsMet(const FStateTrigger& Trigger)
{
	// If no condition checks are enabled, this trigger has nothing to satisfy —
	// return false so the only activation path is TimeoutDuration. (Without this
	// guard the "all checks pass vacuously" return at the bottom would fire on
	// the first tick, skipping the timeout entirely.)
	const bool bHasAnyCheck = Trigger.bCheckPitch || Trigger.bCheckYaw
		|| Trigger.bCheckRoll || Trigger.bCheckVelocity || Trigger.bCheckPlaning
		|| Trigger.bCheckSlopeSin;
	if (!bHasAnyCheck)
	{
		return false;
	}

	UStaticMeshComponent* SurfboardMesh = GetSurfboardMesh();
	if (!SurfboardMesh)
	{
		return false;
	}

	// Get surfboard state
	FRotator rotation = SurfboardMesh->GetComponentRotation();
	FVector velocity = SurfboardMesh->GetPhysicsLinearVelocity();
	float velocityMagnitude = velocity.Size();

	// NOTE: Surfboard mesh is rotated 90 degrees, so:
	// - Nose up/down (what we call "pitch") is stored in rotation.Roll
	// - Left/right lean (what we call "roll") is stored in rotation.Pitch
	// - Yaw is still yaw
	float actualPitch = rotation.Roll;    // Nose up/down
	float actualRoll = rotation.Pitch;    // Left/right lean
	float actualYaw = rotation.Yaw;       // Rotation around vertical axis

	// Check pitch condition (nose up/down)
	if (Trigger.bCheckPitch)
	{
		if (bDebugLogging)
		{
			UE_LOG(LogSurf, Display, TEXT("StateTriggerAutoPilot: Checking pitch (nose up/down) - Current: %.2f, Required: %.2f to %.2f"),
				actualPitch, Trigger.MinPitch, Trigger.MaxPitch);
		}

		if (actualPitch < Trigger.MinPitch || actualPitch > Trigger.MaxPitch)
		{
			return false;
		}
	}

	// Check yaw condition
	if (Trigger.bCheckYaw)
	{
		// Normalize yaw to -180..180 range
		float normalizedYaw = FMath::UnwindDegrees(actualYaw);
		float minYaw = FMath::UnwindDegrees(Trigger.MinYaw);
		float maxYaw = FMath::UnwindDegrees(Trigger.MaxYaw);

		if (bDebugLogging)
		{
			UE_LOG(LogSurf, Display, TEXT("StateTriggerAutoPilot: Checking yaw - Current: %.2f, Required: %.2f to %.2f"),
				normalizedYaw, minYaw, maxYaw);
		}

		if (normalizedYaw < minYaw || normalizedYaw > maxYaw)
		{
			return false;
		}
	}

	// Check roll condition (left/right lean)
	if (Trigger.bCheckRoll)
	{
		if (bDebugLogging)
		{
			UE_LOG(LogSurf, Display, TEXT("StateTriggerAutoPilot: Checking roll (left/right lean) - Current: %.2f, Required: %.2f to %.2f"),
				actualRoll, Trigger.MinRoll, Trigger.MaxRoll);
		}

		if (actualRoll < Trigger.MinRoll || actualRoll > Trigger.MaxRoll)
		{
			return false;
		}
	}

	// Check velocity condition
	if (Trigger.bCheckVelocity)
	{
		if (bDebugLogging)
		{
			UE_LOG(LogSurf, Display, TEXT("StateTriggerAutoPilot: Checking velocity - Current: %.2f, Required: %.2f to %.2f"),
				velocityMagnitude, Trigger.MinVelocity, Trigger.MaxVelocity);
		}

		if (velocityMagnitude < Trigger.MinVelocity)
		{
			return false;
		}
		if (Trigger.MaxVelocity >= 0.0f && velocityMagnitude > Trigger.MaxVelocity)
		{
			return false;
		}
	}

	// Check planing condition
	if (Trigger.bCheckPlaning)
	{
		if (!planingCalculator)
		{
			UE_LOG(LogSurf, Warning, TEXT("StateTriggerAutoPilot: bCheckPlaning is true but planingCalculator is not assigned!"));
			return false;
		}

		// TODO: Get planing amount from planingCalculator
		// For now, skip this check if planingCalculator exists but doesn't have the property yet
		// float planingAmount = planingCalculator->GetPlaningAmount();
		// if (planingAmount < Trigger.MinPlaning || planingAmount > Trigger.MaxPlaning)
		// {
		// 	return false;
		// }
	}

	// Check wave-slope-under-board condition. Uses the same average across this
	// surfboard's SC actors as the recorder's 'slopeSin' CSV column, so values
	// read from a trajectory CSV translate directly into Min/MaxSlopeSin.
	if (Trigger.bCheckSlopeSin)
	{
		ResolveSharedCalcsIfNeeded();

		if (CachedSharedCalcs.Num() == 0)
		{
			// No SC actors for this surfboard — can't evaluate. Same shape as
			// the planingCalculator-missing case: trigger advances only via
			// TimeoutDuration or the 30s emergency force-advance.
			if (bDebugLogging)
			{
				UE_LOG(LogSurf, Warning, TEXT("StateTriggerAutoPilot: bCheckSlopeSin is true but no SharedCalculations actors resolved for surfboard!"));
			}
			return false;
		}

		float slopeSinSum = 0.0f;
		int32 nValid = 0;
		for (ASharedCalculations* SC : CachedSharedCalcs)
		{
			if (!SC) continue;
			slopeSinSum += SC->waveSlopeDownVec.Size();
			++nValid;
		}
		const float slopeSin = (nValid > 0) ? slopeSinSum / nValid : 0.0f;

		if (bDebugLogging)
		{
			UE_LOG(LogSurf, Display, TEXT("StateTriggerAutoPilot: Checking slopeSin - Current: %.4f, Required: %.4f to %.4f"),
				slopeSin, Trigger.MinSlopeSin, Trigger.MaxSlopeSin);
		}

		if (slopeSin < Trigger.MinSlopeSin || slopeSin > Trigger.MaxSlopeSin)
		{
			return false;
		}
	}

	// All checked conditions are met
	if (bDebugLogging)
	{
		UE_LOG(LogSurf, Display, TEXT("StateTriggerAutoPilot: All conditions met"));
	}
	return true;
}

void AStateTriggerAutoPilot::ApplyStep(const FStateTriggerStep& Step, bool bSnapWeight)
{
	// Full applied-step config in one line, so step data entered in the editor is auditable
	// from any run's log. (A previous jet-change log compared Step against currentStep AFTER
	// the copy below, so it could never fire — jet state was invisible in logs, which hid a
	// trimmed intro step missing its jetEngineOn. See specs/skip-paddle-intro.md.)
	UE_LOG(LogSurf, Display, TEXT("StateTriggerAutoPilot: Applying step - %s (weightRight=%.2f weightNose=%.2f jet=%s x%.2f rider=%d)"),
		*Step.description, Step.weightRight, Step.weightNose,
		Step.jetEngineOn ? TEXT("ON") : TEXT("off"), Step.jetMultiplier,
		static_cast<int32>(Step.riderAnimState));

	// Preserve the currently-applied (possibly mid-slew) weight before copying the
	// new step, so a step change doesn't snap the lean. The step's weight becomes
	// the target; Tick eases currentStep.weight* toward it over WeightTransitionDuration.
	const float prevWeightRight = currentStep.weightRight;
	const float prevWeightNose  = currentStep.weightNose;

	// Update current step
	currentStep = Step;

	TargetWeightRight = Step.weightRight;
	TargetWeightNose  = Step.weightNose;
	if (!bSnapWeight)
	{
		// Resume the slew from where the previous step left off rather than
		// jumping to the new step's weight in one tick.
		currentStep.weightRight = prevWeightRight;
		currentStep.weightNose  = prevWeightNose;
	}

	// Push the step's scripted rider animation to the mannequin's AnimInstance. Only the enabled
	// autopilot runs ApplyStep, so this can't fight a sibling. Surf (the default) is set on any
	// untagged step, which is a no-op once the rider has already popped up. See
	// specs/surfer-popup-animation-states.md.
	PushRiderAnimState(Step.riderAnimState);
}

void AStateTriggerAutoPilot::PushRiderAnimState(ESurferAnimState NewState)
{
	// Same lookup the pawn uses (SurfboardPawn RiderMesh): the skeletal mesh attached to this
	// autopilot's board. No-op if the board/mesh/anim instance isn't resolvable yet (e.g. an early
	// BeginPlay call before the mesh's AnimInstance exists) — the caller path via Start()/ApplyStep
	// pushes again once it is.
	if (!surfboard)
	{
		return;
	}
	if (USkeletalMeshComponent* RiderMesh = surfboard->FindComponentByClass<USkeletalMeshComponent>())
	{
		if (USurferAnimInstance* RiderAnim = Cast<USurferAnimInstance>(RiderMesh->GetAnimInstance()))
		{
			RiderAnim->SetAnimState(NewState);
		}
	}
}

void AStateTriggerAutoPilot::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// Safety net: if the autopilot is torn down before the last step activates
	// (force-killed, level switched, crash on the way out), still flush whatever
	// trajectory data we have.
	if (bRecorderActive && !bRecorderFlushed)
	{
		FlushRecorder();
	}
	Super::EndPlay(EndPlayReason);
}

void AStateTriggerAutoPilot::ResolveSharedCalcsIfNeeded()
{
	if (bSharedCalcsResolved || !surfboard) return;
	bSharedCalcsResolved = true;
	TArray<AActor*> Found;
	UGameplayStatics::GetAllActorsOfClass(GetWorld(), ASharedCalculations::StaticClass(), Found);
	for (AActor* A : Found)
	{
		if (ASharedCalculations* SC = Cast<ASharedCalculations>(A))
		{
			if (SC->Surfboard == surfboard)
			{
				CachedSharedCalcs.Add(SC);
			}
		}
	}
	UE_LOG(LogSurf, Display, TEXT("StateTriggerAutoPilot: SharedCalculations resolved -> %d actors"), CachedSharedCalcs.Num());
}

void AStateTriggerAutoPilot::RecordSample()
{
	UStaticMeshComponent* Mesh = GetSurfboardMesh();
	if (!Mesh) return;

	// Lazy-resolve done on first sample so we don't depend on actor spawn
	// ordering in BeginPlay. AreConditionsMet's slope check uses the same helper.
	ResolveSharedCalcsIfNeeded();

	// Aggregate per-tick wave/board scalars by mean across this board's SC actors.
	// slopeSin (=|waveSlopeDownVec|) and AmountPlaning are the two suspected drivers
	// of the trough slow-down + sharp re-acceleration in step 3; amountUnderWater
	// rules out the "board lifts out of water" alternative.
	float slopeSinSum = 0.0f;
	float planingSum  = 0.0f;
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
	const float slopeSin  = (nValid > 0) ? slopeSinSum   / nValid : 0.0f;
	const float planing   = (nValid > 0) ? planingSum    / nValid : 0.0f;
	const float underW    = (nValid > 0) ? underWaterSum / nValid : 0.0f;

	const FVector Pos = Mesh->GetComponentLocation();
	const FVector Vel = Mesh->GetPhysicsLinearVelocity();
	const FRotator Rot = Mesh->GetComponentRotation();
	const int32 Frame = ReadWaterControllerFrame(waterController);
	const float GameSecs = ReadWaterControllerSeconds(waterController);

	RecorderBuffer += FString::Printf(
		TEXT("%.3f,%.3f,%d,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.2f,%.2f,%.2f,%d,%.4f,%.3f,%.3f\n"),
		TimeSinceStart, GameSecs, Frame,
		Pos.X, Pos.Y, Pos.Z,
		Vel.X, Vel.Y, Vel.Z,
		Rot.Roll, Rot.Pitch, Rot.Yaw,
		CurrentStepIndex,
		slopeSin, planing, underW);
}

void AStateTriggerAutoPilot::FlushRecorder()
{
	if (!bRecorderActive || bRecorderFlushed) return;
	bRecorderFlushed = true;

	const FString Path = FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("Tests"), TEXT("latest"),
		TestName + TEXT(".csv"));

	if (FFileHelper::SaveStringToFile(RecorderBuffer, *Path))
	{
		UE_LOG(LogSurf, Display, TEXT("StateTriggerAutoPilot: Recorder flushed %d bytes -> %s"),
			RecorderBuffer.Len(), *Path);
	}
	else
	{
		UE_LOG(LogSurf, Warning, TEXT("StateTriggerAutoPilot: Recorder FAILED to write %s"), *Path);
	}
}

UStaticMeshComponent* AStateTriggerAutoPilot::GetSurfboardMesh()
{
	if (!surfboard)
	{
		return nullptr;
	}

	return surfboard->FindComponentByClass<UStaticMeshComponent>();
}

void AStateTriggerAutoPilot::ApplyStartPose()
{
	UStaticMeshComponent* StartMesh = GetSurfboardMesh();
	if (!StartMesh)
	{
		return;
	}
	// StartLocation zero = "don't move" sentinel; rotation is always applied.
	if (!StartLocation.IsZero())
	{
		StartMesh->SetWorldLocation(StartLocation);
	}
	StartMesh->SetWorldRotation(StartRotation);
	if (StartMesh->IsSimulatingPhysics())
	{
		// Zero defaults = legacy "start at rest". Measured values = state injection: the board
		// starts already moving with the (jumped) wave. See specs/skip-paddle-intro.md.
		StartMesh->SetPhysicsLinearVelocity(StartVelocity);
		StartMesh->SetPhysicsAngularVelocityInDegrees(StartAngularVelocityDeg);
	}
}

void AStateTriggerAutoPilot::ApplyStartWaveFrame()
{
	if (StartWaveFrame < 0 || !waterController)
	{
		return;
	}
	// One-time relative-timeline shift: the WaterController BP advances CurrentFrame by relative
	// increment each tick, so a single write sticks and the wave free-runs from the injected
	// frame. bManualFrameControl (the replay pin) is deliberately NOT set — we want free-run.
	// Same reflection shape as ASurfboardPawn::SetWaveFrame.
	const int32 PrevFrame = ReadWaterControllerFrame(waterController);
	FProperty* FrameProp = waterController->GetClass()->FindPropertyByName(TEXT("CurrentFrame"));
	if (!FrameProp) FrameProp = waterController->GetClass()->FindPropertyByName(TEXT("Current Frame"));
	if (!FrameProp) FrameProp = waterController->GetClass()->FindPropertyByName(TEXT("current_frame"));
	if (FIntProperty* I = CastField<FIntProperty>(FrameProp))
	{
		I->SetPropertyValue_InContainer(waterController, StartWaveFrame);
	}
	else if (FFloatProperty* F = CastField<FFloatProperty>(FrameProp))
	{
		F->SetPropertyValue_InContainer(waterController, static_cast<float>(StartWaveFrame));
	}
	else
	{
		UE_LOG(LogSurf, Warning, TEXT("StateTriggerAutoPilot: StartWaveFrame=%d set but no writable CurrentFrame property on %s"),
			StartWaveFrame, *waterController->GetName());
		return;
	}
	UE_LOG(LogSurf, Display, TEXT("StateTriggerAutoPilot: wave clock jumped %d -> %d (StartWaveFrame)"),
		PrevFrame, StartWaveFrame);
}

void AStateTriggerAutoPilot::LogStartPoseSnapshot(int32 StepIndex)
{
	UStaticMeshComponent* Mesh = GetSurfboardMesh();
	if (!Mesh)
	{
		return;
	}
	const FVector Loc = Mesh->GetComponentLocation();
	const FRotator Rot = Mesh->GetComponentRotation();
	// Kinematically-driven (rails) counts as having a velocity — otherwise a snapshot measured
	// during a rails run would report zeros and look like a dead board.
	const bool bHasVelocity = Mesh->IsSimulatingPhysics() || SurfRails::IsKinematicallyDriven();
	const FVector Vel = bHasVelocity ? Mesh->GetPhysicsLinearVelocity() : FVector::ZeroVector;
	const FVector AngVel = bHasVelocity ? Mesh->GetPhysicsAngularVelocityInDegrees() : FVector::ZeroVector;
	const int32 Frame = ReadWaterControllerFrame(waterController);
	// One line per step activation, named after the AStateTriggerAutoPilot UPROPERTYs the values
	// paste into (editor vector/rotator properties accept the parenthesized forms verbatim).
	// This is the measurement tool for the skip-paddle state injection: play one normal run, copy
	// the line for the step you want the game to start at. specs/skip-paddle-intro.md.
	UE_LOG(LogSurf, Display, TEXT("StateTriggerAutoPilot: START-POSE SNAPSHOT @ step %d (%s): StartWaveFrame=%d StartLocation=(X=%.1f,Y=%.1f,Z=%.1f) StartRotation=(Pitch=%.2f,Yaw=%.2f,Roll=%.2f) StartVelocity=(X=%.1f,Y=%.1f,Z=%.1f) StartAngularVelocityDeg=(X=%.1f,Y=%.1f,Z=%.1f)"),
		StepIndex,
		steps.IsValidIndex(StepIndex) ? *steps[StepIndex].description : TEXT("?"),
		Frame,
		Loc.X, Loc.Y, Loc.Z,
		Rot.Pitch, Rot.Yaw, Rot.Roll,
		Vel.X, Vel.Y, Vel.Z,
		AngVel.X, AngVel.Y, AngVel.Z);
}
