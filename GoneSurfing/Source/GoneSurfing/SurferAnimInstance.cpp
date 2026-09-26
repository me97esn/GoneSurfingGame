// Fill out your copyright notice in the Description page of Project Settings.

#include "SurferAnimInstance.h"
#include "Animation/AnimSequence.h"
#include "SurfLog.h"
#include "SurfDebug.h"
#include "EngineUtils.h"
#include "WeightDistribution.h"
#include "SurfTuningSubsystem.h"
#include "Engine/GameInstance.h"
#include "Components/SkeletalMeshComponent.h"
#include "HAL/IConsoleManager.h"

// Runtime override for bHipStabUseStoredCalibration, so a recapture can be driven from a headless
// -ExecCmds run instead of unticking the AnimBP Class Default by hand. The stored values live in
// the C++ defaults but the flag is a Blueprint CDO override, which nothing outside the editor can
// write — this is the way in. Recapture is needed whenever the stance pose asset or the rider's
// placement on the board changes. See specs/surfer-rider-hip-ik.md.
static TAutoConsoleVariable<int32> CVarHipStabRecapture(
	TEXT("surf.hipstab.recapture"),
	0,
	TEXT("1 = ignore bHipStabUseStoredCalibration and run the runtime hip-stab capture, logging a paste-ready values block. 0 = honor the flag."),
	ECVF_Default);

void USurferAnimInstance::SetReplayWeights(float AmountInFront, float AmountToTheRight)
{
	this->bReplayWeightOverride = true;
	this->ReplayAmountInFront = AmountInFront;
	this->ReplayAmountToTheRight = AmountToTheRight;
}

void USurferAnimInstance::SetReplayPump(bool bPumpingNow, float Phase)
{
	this->ReplayPumping = bPumpingNow;
	this->ReplayPumpPhase = Phase;
}

AWeightDistribution* USurferAnimInstance::ResolveWeightDistribution()
{
	if (this->cachedWeightDistribution)
	{
		return this->cachedWeightDistribution;
	}

	UWorld* world = GetWorld();
	AActor* owner = GetOwningActor();
	if (!world || !owner)
	{
		return nullptr;
	}

	// Prefer the WeightDistribution wired to the board this mannequin is attached to; fall back
	// to the nearest one (single-board levels, or the mesh living on its own actor).
	AWeightDistribution* nearest = nullptr;
	float nearestDistSq = TNumericLimits<float>::Max();
	for (TActorIterator<AWeightDistribution> it(world); it; ++it)
	{
		AWeightDistribution* wd = *it;
		if (wd->Surfboard && wd->Surfboard == owner)
		{
			this->cachedWeightDistribution = wd;
			return wd;
		}
		const float distSq = FVector::DistSquared(wd->GetActorLocation(), owner->GetActorLocation());
		if (distSq < nearestDistSq)
		{
			nearestDistSq = distSq;
			nearest = wd;
		}
	}
	this->cachedWeightDistribution = nearest;
	return nearest;
}

void USurferAnimInstance::StartFall(bool bToBoardRight)
{
	this->bFalling = true;
	this->FallSequence = bToBoardRight ? this->FallRightSequence : this->FallLeftSequence;
	this->FallSequenceTime = 0.0f;
	UE_LOG(LogSurf, Display, TEXT("SurferAnim: fall clip %s (%.2f s) to the board's %s"),
		this->FallSequence ? *this->FallSequence->GetName() : TEXT("<none>"),
		GetFallClipLength(), bToBoardRight ? TEXT("right") : TEXT("left"));
}

void USurferAnimInstance::StopFall()
{
	this->bFalling = false;
	this->FallSequenceTime = 0.0f;
}

float USurferAnimInstance::GetFallClipLength() const
{
	if (!this->bFalling || !this->FallSequence)
	{
		return 0.0f;
	}
	return this->FallSequence->GetPlayLength() / FMath::Max(this->FallPlayRate, 0.1f);
}

bool USurferAnimInstance::IsFallClipFinished() const
{
	return !this->bFalling || !this->FallSequence
		|| this->FallSequenceTime >= this->FallSequence->GetPlayLength();
}

void USurferAnimInstance::UpdateFallClip(float DeltaSeconds)
{
	if (!this->bFalling || !this->FallSequence)
	{
		// At rest the evaluator still has a clip to look at: the left fall's first frame, which is
		// the authored stance - the same trick the tired evaluator uses.
		this->FallSequence = this->FallLeftSequence;
		this->FallSequenceTime = 0.0f;
		return;
	}
	// Once, at FallPlayRate, then hold the last frame: the rider stays lying in the water under
	// the card (and sinks - the pawn's job).
	this->FallSequenceTime = FMath::Min(this->FallSequenceTime + DeltaSeconds * FMath::Max(this->FallPlayRate, 0.1f),
		this->FallSequence->GetPlayLength());
}

void USurferAnimInstance::UpdateTiredClips(float DeltaSeconds)
{
	UAnimSequence* In    = this->TiredFadeInSequence;
	UAnimSequence* Loop  = this->TiredBreathingSequence;
	UAnimSequence* Out   = this->TiredFadeOutSequence;
	if (!In || !Loop || !Out)
	{
		// Clips not assigned in the AnimBP: nothing to show, the flag alone says tired.
		this->TiredClip = ETiredClip::None;
		this->TiredClipTime = 0.0f;
		this->TiredSequence = nullptr;
		this->TiredSequenceTime = 0.0f;
		this->TiredStanceBlendAlpha = 0.0f;
		return;
	}
	const float InLen   = FMath::Max(KINDA_SMALL_NUMBER, In->GetPlayLength());
	const float LoopLen = FMath::Max(KINDA_SMALL_NUMBER, Loop->GetPlayLength());
	const float OutLen  = FMath::Max(KINDA_SMALL_NUMBER, Out->GetPlayLength());

	// The machine: None -(tired)-> FadeIn -(clip ends)-> Breathing -(not tired, at the end of the
	// current breath)-> FadeOut -(clip ends)-> None. Two interruptions are handled by MIRRORING the
	// time rather than restarting: recovering mid-fade-in jumps to the fade-out at the matching
	// depth, and tiring again mid-fade-out jumps back into the fade-in likewise, so a rider who
	// recovers a second after tiring un-slumps from where they are instead of snapping to the full
	// slump first. This assumes the two fades are each other's reverse, which is how they were
	// authored. The breath is always finished before fading out: it starts and ends in the tired
	// stance, so leaving at its boundary is seamless and leaving mid-inhale would not be.
	switch (this->TiredClip)
	{
	case ETiredClip::None:
		if (this->bTired)
		{
			this->TiredClip = ETiredClip::FadeIn;
			this->TiredClipTime = 0.0f;
		}
		break;

	case ETiredClip::FadeIn:
		if (!this->bTired)
		{
			const float Depth = FMath::Clamp(this->TiredClipTime / InLen, 0.0f, 1.0f);
			this->TiredClip = ETiredClip::FadeOut;
			this->TiredClipTime = (1.0f - Depth) * OutLen;
		}
		else
		{
			this->TiredClipTime += DeltaSeconds;
			if (this->TiredClipTime >= InLen)
			{
				this->TiredClip = ETiredClip::Breathing;
				this->TiredClipTime = this->TiredClipTime - InLen;
			}
		}
		break;

	case ETiredClip::Breathing:
		this->TiredClipTime += DeltaSeconds;
		if (this->TiredClipTime >= LoopLen)
		{
			if (this->bTired)
			{
				this->TiredClipTime = FMath::Fmod(this->TiredClipTime, LoopLen);
			}
			else
			{
				this->TiredClip = ETiredClip::FadeOut;
				this->TiredClipTime = this->TiredClipTime - LoopLen;
			}
		}
		break;

	case ETiredClip::FadeOut:
		if (this->bTired)
		{
			const float Recovered = FMath::Clamp(this->TiredClipTime / OutLen, 0.0f, 1.0f);
			this->TiredClip = ETiredClip::FadeIn;
			this->TiredClipTime = (1.0f - Recovered) * InLen;
		}
		else
		{
			this->TiredClipTime += DeltaSeconds;
			if (this->TiredClipTime >= OutLen)
			{
				this->TiredClip = ETiredClip::None;
				this->TiredClipTime = 0.0f;
			}
		}
		break;
	}

	switch (this->TiredClip)
	{
	case ETiredClip::FadeIn:
		this->TiredSequence = In;
		this->TiredSequenceTime = FMath::Clamp(this->TiredClipTime, 0.0f, InLen);
		this->TiredStanceBlendAlpha = 1.0f;
		break;
	case ETiredClip::Breathing:
		this->TiredSequence = Loop;
		this->TiredSequenceTime = FMath::Clamp(this->TiredClipTime, 0.0f, LoopLen);
		this->TiredStanceBlendAlpha = 1.0f;
		break;
	case ETiredClip::FadeOut:
		this->TiredSequence = Out;
		this->TiredSequenceTime = FMath::Clamp(this->TiredClipTime, 0.0f, OutLen);
		this->TiredStanceBlendAlpha = 1.0f;
		break;
	case ETiredClip::None:
	default:
		// At rest: the fade-in's first frame is the stance, so an evaluator that runs anyway shows
		// the right pose. Alpha 0 is what actually hides it.
		this->TiredSequence = In;
		this->TiredSequenceTime = 0.0f;
		this->TiredStanceBlendAlpha = 0.0f;
		break;
	}
}

void USurferAnimInstance::NativeUpdateAnimation(float DeltaSeconds)
{
	// Walk AnimState toward the requested state, one link per update, and report the result.
	//
	// The AnimBP's rules are chained (Paddle -> Cobra -> PopUp), each keyed on AnimState being
	// exactly the next state. Skip a link and no rule matches and the machine never moves again — see
	// SetAnimState for the measurement. One link per update gives the machine a full frame to take
	// each transition, which is what the rails driver was already doing successfully.
	{
		const int32 MachineIdx = GetStateMachineIndex(this->riderStateMachineName);
		const FName MachineState = (MachineIdx != INDEX_NONE)
			? GetCurrentStateName(MachineIdx) : FName(TEXT("<no state machine>"));

		if ((uint8)this->AnimState < (uint8)this->DesiredAnimState)
		{
			const ESurferAnimState Next = (ESurferAnimState)((uint8)this->AnimState + 1);

			// Surf is the one link the machine takes BY ITSELF, when the pop-up clip ends. Entering it
			// here on schedule rather than on evidence is what desynchronises the variable from the
			// pose — and the hip-stab calibration gate keys on this variable, so a premature Surf
			// records a mid-pop-up crouch as the stance. So only follow the machine into it.
			const bool bMayEnterSurf = (MachineIdx == INDEX_NONE) || (MachineState == this->surfStateName);
			if (Next != ESurferAnimState::Surf || bMayEnterSurf)
			{
				this->AnimState = Next;
			}
		}

		// Diagnostic: the requested state, the walked state, and the state the machine is actually IN
		// are three different things, and only the last is the pose on screen. When the rider is stuck
		// prone this says immediately which of them stopped moving. Logged on change only.
		if (MachineState != this->LastLoggedMachineState || this->AnimState != this->LastLoggedAnimState)
		{
			UE_LOG(LogSurf, Display, TEXT("SurferAnim: AnimState=%d (target %d) | machine state='%s'"),
				(int32)this->AnimState, (int32)this->DesiredAnimState, *MachineState.ToString());
			this->LastLoggedMachineState = MachineState;
			this->LastLoggedAnimState = this->AnimState;
		}
	}

	Super::NativeUpdateAnimation(DeltaSeconds);

	// Weight amounts: recorded values during kinematic replay (the live WeightDistribution is
	// tick-frozen there), live actor reads otherwise.
	float amountInFront = 0.5f;
	float amountToTheRight = 0.5f;
	bool bHaveWeights = false;
	if (this->bReplayWeightOverride)
	{
		amountInFront = this->ReplayAmountInFront;
		amountToTheRight = this->ReplayAmountToTheRight;
		bHaveWeights = true;
	}
	else if (const AWeightDistribution* wd = ResolveWeightDistribution())
	{
		amountInFront = wd->amountInFront;
		amountToTheRight = wd->amountToTheRight;
		bHaveWeights = true;
	}

	// Pump stroke: the recorded phase during kinematic replay (the live WeightDistribution is
	// tick-frozen there), straight from the physics otherwise, so the clip cannot drift against it.
	if (this->bReplayWeightOverride)
	{
		// The trace carries the whole gesture in one number: 0..1 crouch, 1..2 extension.
		this->bPumping         = this->ReplayPumping;
		this->PumpPhase        = FMath::Clamp(this->ReplayPumpPhase, 0.0f, 2.0f);
		this->bPumpReleasing   = this->PumpPhase > 1.0f;
		this->PumpCharge       = this->bPumpReleasing ? 0.0f : this->PumpPhase;
		this->PumpReleasePhase = this->bPumpReleasing ? (this->PumpPhase - 1.0f) : 0.0f;
		// The trace carries one number, so the charge at release is reconstructed: it is the last
		// crouch depth seen before the extension began. Cheaper than a second trace column, and
		// exact, because the crouch is sampled every frame right up to the release.
		if (!this->bPumpReleasing)
		{
			this->PumpReleaseFromCharge = this->PumpCharge;
		}
	}
	else if (const AWeightDistribution* wdPump = ResolveWeightDistribution())
	{
		this->bPumping         = wdPump->bPumpActive;
		this->PumpPhase        = FMath::Clamp(wdPump->PumpStrokePhase, 0.0f, 2.0f);
		this->PumpCharge       = FMath::Clamp(wdPump->PumpCharge, 0.0f, 1.0f);
		this->bPumpReleasing   = wdPump->bPumpReleasing;
		this->PumpReleasePhase = FMath::Clamp(wdPump->PumpReleasePhase, 0.0f, 1.0f);
		this->PumpReleaseFromCharge = FMath::Clamp(wdPump->PumpReleaseFromCharge, 0.0f, 1.0f);
	}
	else
	{
		this->bPumping = false;
		this->bPumpReleasing = false;
	}

	// The crouch clip is STILL ON SCREEN while the rise blends in, so its explicit time must not be
	// zeroed the instant the button comes up - and the pawn zeroes its own PumpCharge on exactly
	// that tick (UpdateHeldPump: the charge has been spent into PumpReleaseStrength, so the force
	// path is done with it). The AnimBP crossfades crouch -> rise over ~0.05 s and a Blend Poses by
	// bool evaluates BOTH sides across that window, so a charge of 0 puts SK_crouch_down on frame 0
	// - which IS the standing stance - at ~full weight on the first frame of the blend. The rider
	// snapped up to standing, then slid back down into the rise clip's first frame, then rose:
	// "pops half way up, then back down again, and then slowly rises" (owner, 2026-09-22, seen on
	// PC and on the phone). Frame-rate independent, because it keys off the blend time.
	//
	// Hold the crouch at the depth it actually had, so the outgoing pose is where the rider is. The
	// incoming pose is the rise clip at t0 = 1 - PumpReleaseFromCharge, which is that same depth -
	// that is what t0 is for - so the crossfade now has nothing to travel. Visual only: nothing but
	// the AnimBP reads this value, and the pawn's own PumpCharge is untouched.
	if (this->bPumpReleasing)
	{
		this->PumpCharge = this->PumpReleaseFromCharge;
	}

	// --- The pump's third movement: settling back into the stance ---
	//
	// The crouch and the rise are clips, but the return to the stance is the AnimBP's outer blend
	// against the surf pose, and left alone it is a snap. `bPumping` and `bPumpReleasing` both drop
	// on the SAME tick the extension completes, so at the instant the outer blend starts, the inner
	// gate has already swapped to the crouch clip at time zero - which IS the stance. The blend then
	// has nothing to travel: it crossfades stance to stance while the rider teleports down from tall.
	//
	// Hold the rise clip pinned on its last, tall frame for PumpRecoverySeconds after the extension
	// ends, with bPumping already false so the outer blend runs across exactly that window. Visual
	// only, and deliberately AFTER the force window: PumpInput, the attenuations and the next pump's
	// charge are all the pawn's and none of them see this. A fresh stroke cancels it outright - the
	// player crouching again must beat the settle, not queue behind it.
	{
		const USurfTuningSubsystem* PumpTuning = nullptr;
		if (const UWorld* TuningWorld = GetWorld())
		{
			if (const UGameInstance* GI = TuningWorld->GetGameInstance())
			{
				PumpTuning = GI->GetSubsystem<USurfTuningSubsystem>();
			}
		}
		const float RecoverySeconds = PumpTuning ? FMath::Max(PumpTuning->PumpRecoverySeconds, 0.0f) : 0.45f;

		// End of extension: was releasing, no longer is, and no new stroke has started.
		if (this->bPumpWasReleasing && !this->bPumpReleasing && !this->bPumping && RecoverySeconds > 0.0f)
		{
			this->PumpSettleRemaining  = RecoverySeconds;
			// Keep the rise clip's start point, or its explicit time jumps as the settle begins.
			this->PumpSettleFromCharge = this->PumpReleaseFromCharge;
		}
		this->bPumpWasReleasing = this->bPumpReleasing;

		if (this->bPumping)
		{
			this->PumpSettleRemaining = 0.0f;
		}
		else if (this->PumpSettleRemaining > 0.0f)
		{
			this->PumpSettleRemaining = FMath::Max(0.0f, this->PumpSettleRemaining - DeltaSeconds);

			// bPumping stays false - the outer blend to the stance is what we are giving time to.
			this->bPumpReleasing        = true;
			this->PumpReleasePhase      = 1.0f;
			this->PumpReleaseFromCharge = this->PumpSettleFromCharge;
			this->PumpCharge            = 0.0f;
			this->PumpPhase             = 2.0f;
		}

		this->bPumpSettling = this->PumpSettleRemaining > 0.0f;

		// The pump pair's share of the pose: full through the stroke, easing to nothing across the
		// settle. Smoothstepped because a body settling out of an extension decelerates - a linear
		// ramp arrives at the stance still moving and reads as a stop, not an arrival.
		// It steps straight to 1 when a stroke starts, which needs no blend: the crouch clip's first
		// frame IS the stance, so there is nothing to cross-fade at that end.
		const float SettleFraction = (this->bPumpSettling && RecoverySeconds > 0.0f)
			? FMath::Clamp(this->PumpSettleRemaining / RecoverySeconds, 0.0f, 1.0f)
			: 0.0f;
		this->PumpStanceBlendAlpha = this->bPumping
			? 1.0f
			: FMath::SmoothStep(0.0f, 1.0f, SettleFraction);
	}

	if (SurfDebug::IsFlagSet(TEXT("pump")))
	{
		// The pawn's own "Pump:" line reports what the physics holds; this reports what the AnimBP
		// is handed, which is where the crouch clip's time and the settle can disagree with it.
		UE_LOG(LogSurf, Warning,
			TEXT("PumpAnim: pumping=%d charge=%.3f releasing=%d relPhase=%.3f fromCharge=%.3f settling=%d stanceAlpha=%.3f"),
			this->bPumping ? 1 : 0, this->PumpCharge, this->bPumpReleasing ? 1 : 0,
			this->PumpReleasePhase, this->PumpReleaseFromCharge,
			this->bPumpSettling ? 1 : 0, this->PumpStanceBlendAlpha);
	}

	// Tired: which of the three clips, and how far into it. See the Tired block in the header.
	UpdateTiredClips(DeltaSeconds);

	// Fall: the one clip, once, then held. See the Fall block in the header.
	UpdateFallClip(DeltaSeconds);

	float targetLeanRight = 0.0f;
	float targetLeanForward = 0.0f;
	float targetTwist = this->neutralTwistYawDeg;
	if (bHaveWeights)
	{
		const float lateral = (amountToTheRight - 0.5f) * 2.0f;  // -1 (left) .. +1 (right)
		const float foreAft = (amountInFront - 0.5f) * 2.0f;     // -1 (tail) .. +1 (nose)
		targetLeanRight = lateral * this->maxLeanRightDeg;
		targetLeanForward = foreAft * this->maxLeanForwardDeg;
		targetTwist = this->neutralTwistYawDeg + lateral * this->maxTwistYawDeg;
	}

	// Board-attitude compensation: the mesh is attached to the board, so the body inherits deck
	// roll and then adds the commanded lean on top — double-leaning. Subtract the board's world
	// attitude so the torso stays balanced over the feet while the deck tilts beneath. Computed
	// GEOMETRICALLY from the owning board actor's transform (board.forwards = local +Y,
	// board.left = local -X, the project-wide convention) — correct live AND during kinematic
	// replay, where the SC signal is tick-frozen but the replayed transform is the recording.
	if (const AActor* boardActor = GetOwningActor())
	{
		const FTransform boardTF = boardActor->GetActorTransform();
		const FVector boardLeft = boardTF.TransformVectorNoScale(FVector(-1.0, 0.0, 0.0)).GetSafeNormal();
		const float boardRollDeg = FMath::RadiansToDegrees(FMath::Asin(
			FMath::Clamp((float)boardLeft.Z, -1.0f, 1.0f)));
		this->LastBoardRollDeg = boardRollDeg;   // diagnostic only
		targetLeanRight -= this->rollCompensation * boardRollDeg;
		if (this->pitchCompensation != 0.0f)
		{
			const FVector boardFwd = boardTF.TransformVectorNoScale(FVector(0.0, 1.0, 0.0)).GetSafeNormal();
			const float boardPitchDeg = FMath::RadiansToDegrees(FMath::Asin(
				FMath::Clamp((float)boardFwd.Z, -1.0f, 1.0f)));
			targetLeanForward -= this->pitchCompensation * boardPitchDeg;
		}
	}

	this->LeanRightDeg = FMath::FInterpTo(this->LeanRightDeg, targetLeanRight, DeltaSeconds, this->leanInterpSpeed);
	this->LeanForwardDeg = FMath::FInterpTo(this->LeanForwardDeg, targetLeanForward, DeltaSeconds, this->leanInterpSpeed);
	this->TwistYawDeg = FMath::FInterpTo(this->TwistYawDeg, targetTwist, DeltaSeconds, this->leanInterpSpeed);

	UpdateHipStabilization(DeltaSeconds);

	// Per-frame rider diagnostic: surf.debug.flags rider. Unthrottled on purpose - the thing being
	// hunted is a value that alternates between consecutive frames, which any throttle hides.
	if (SurfDebug::IsFlagSet(TEXT("rider")))
	{
		UE_LOG(LogSurf, Warning,
			TEXT("RIDER dt=%.4f state=%d wRight=%.4f wFront=%.4f boardRoll=%.4f targetLeanR=%.4f ")
			TEXT("leanR=%.4f leanF=%.4f twist=%.4f | hipStab en=%d ready=%d pelvis=(%.3f,%.3f,%.3f) ")
			TEXT("| pump=%d phase=%.3f replay=%d"),
			DeltaSeconds, (int32)this->AnimState, amountToTheRight, amountInFront,
			this->LastBoardRollDeg, targetLeanRight,
			this->LeanRightDeg, this->LeanForwardDeg, this->TwistYawDeg,
			this->bHipStabEnabled ? 1 : 0, this->bHipStabReady ? 1 : 0,
			this->PelvisStabLocation.X, this->PelvisStabLocation.Y, this->PelvisStabLocation.Z,
			this->bPumping ? 1 : 0, this->PumpPhase, this->bReplayWeightOverride ? 1 : 0);
	}
}

void USurferAnimInstance::UpdateHipStabilization(float DeltaSeconds)
{
	if (!this->bHipStabEnabled)
	{
		this->bHipStabReady = false;
		return;
	}

	USkeletalMeshComponent* mesh = GetSkelMeshComponent();
	const AActor* boardActor = GetOwningActor();
	if (!mesh || !boardActor)
	{
		this->bHipStabReady = false;
		return;
	}

	// Board tilt + stable frame (board location + yaw only), computed geometrically from the
	// actor transform (board.forwards = local +Y, board.left = local -X, the project-wide
	// convention) — correct live AND during kinematic replay, like the lean compensation was.
	const FTransform boardTF = boardActor->GetActorTransform();
	const FVector boardFwd = boardTF.TransformVectorNoScale(FVector(0.0, 1.0, 0.0)).GetSafeNormal();
	const FVector boardLeft = boardTF.TransformVectorNoScale(FVector(-1.0, 0.0, 0.0)).GetSafeNormal();
	const float pitchDeg = FMath::RadiansToDegrees(FMath::Asin(FMath::Clamp((float)boardFwd.Z, -1.0f, 1.0f)));
	const float rollDeg = FMath::RadiansToDegrees(FMath::Asin(FMath::Clamp((float)boardLeft.Z, -1.0f, 1.0f)));

	// Actor-equivalent world yaw from the horizontal forward direction: forwards is local +Y,
	// 90 deg ahead of the actor's X axis, so subtract 90 to express the stable frame in actor-
	// rotation terms (flat board => stable frame == actor transform). Held at the last valid
	// value if the board ever points near-vertical (unreachable while riding).
	const FVector fwdHoriz(boardFwd.X, boardFwd.Y, 0.0);
	if (fwdHoriz.SizeSquared() > KINDA_SMALL_NUMBER)
	{
		this->StableFrameYawDeg = FMath::RadiansToDegrees(FMath::Atan2(fwdHoriz.Y, fwdHoriz.X)) - 90.0f;
	}
	const FTransform stableFrame(FRotator(0.0f, this->StableFrameYawDeg, 0.0f), boardTF.GetLocation());

	// One-time calibration on near-flat water. bHipStabReady is still false here, so the AnimBP's
	// pelvis Replace node (gated on it) hasn't touched the pose yet: the captured transforms are
	// the unmodified stance — feedback-free. Component space is rigidly attached to the board, so
	// the captured foot transforms ARE deck positions and stay valid for the whole session.
	// Stored calibration: values captured once on the flat map (where the post-pop-up water is
	// still flat) and baked into the AnimBP Class Defaults. Wave levels need them because after
	// pop-up the board is on a wave with no flat window for the runtime capture.
	const bool bRecapture = CVarHipStabRecapture.GetValueOnAnyThread() != 0;
	if (bRecapture && this->bHipStabCalibratedFromStored)
	{
		// The CVar arrived after the stored values were already applied (frame 0 beats -ExecCmds).
		// Drop back to uncalibrated so the runtime capture below can still run this session.
		this->bHipStabCalibrated = false;
		this->bHipStabCalibratedFromStored = false;
		this->HipStabUpdatesSeen = 0;
		UE_LOG(LogSurf, Display,
			TEXT("SurferAnimInstance: surf.hipstab.recapture=1 — discarding stored calibration, recapturing"));
	}

	const bool bUseStored = this->bHipStabUseStoredCalibration && !bRecapture;
	if (!this->bHipStabCalibrated && bUseStored)
	{
		this->PelvisOffsetInStableFrame = FTransform(this->storedPelvisOffsetStableRotation, this->storedPelvisOffsetStableLocation);
		this->PelvisSourceCS = FTransform(this->storedPelvisSourceRotation, this->storedPelvisSourceLocation);
		this->FootTargetLeft = this->storedFootTargetLeft;
		this->FootTargetRight = this->storedFootTargetRight;
		this->FootRotationLeft = this->storedFootRotationLeft;
		this->FootRotationRight = this->storedFootRotationRight;
		this->PelvisStabLocation = this->PelvisSourceCS.GetLocation();
		this->PelvisStabRotation = this->PelvisSourceCS.GetRotation().Rotator();
		this->bHipStabCalibrated = true;
		this->bHipStabCalibratedFromStored = true;
		UE_LOG(LogSurf, Display, TEXT("SurferAnimInstance: hip-stab using STORED calibration"));
	}

	if (!this->bHipStabCalibrated)
	{
		// The capture must see the CLIP's pose, not the IK's. bHipStabReady gates the AnimBP's
		// pelvis Replace / TwoBoneIK nodes, and it is otherwise only cleared by the early-outs
		// above — so a recapture that follows an applied stored calibration would leave it true
		// and measure the pose the OLD targets had already forced. That fed the previous targets
		// straight back into the new ones (identical values every run, regardless of anim state).
		this->bHipStabReady = false;

		// The capture must see the pure stance. Outside the Surf state the paddle/cobra/pop-up
		// clips are playing (every level intro runs them, including Boards_on_flat_water), and
		// for a short while after entering Surf the pop-up->stance transition is still blending.
		// So: count evaluated frames only while IN the Surf state, resetting on any other state,
		// and wait out the blend before trusting the pose. (The count also covers the original
		// concern that the first evaluated frames return the reference pose, which once folded
		// the rider onto the deck.)
		// Two different notions of "in the stance", and only one of them is the pose on screen:
		// AnimState is the autopilot's PUSHED intent, set the moment the stance step activates,
		// but the state machine leaves PopUp only when the pop-up CLIP finishes (a time-based
		// transition, a second or more later). Gating on the variable alone captured the rider
		// mid-pop-up, still prone — feet 7cm apart, pelvis at foot height — and stored that as
		// the stance. Require the machine to actually be in the Surf state.
		bool bMachineInSurf = true;
		const int32 machineIdx = GetStateMachineIndex(this->riderStateMachineName);
		if (machineIdx != INDEX_NONE)
		{
			bMachineInSurf = GetCurrentStateName(machineIdx) == this->surfStateName;
		}
		else if (!this->bLoggedMissingRiderStateMachine)
		{
			// Fall back to the AnimState-only gate rather than never calibrating, but say so.
			this->bLoggedMissingRiderStateMachine = true;
			UE_LOG(LogSurf, Warning,
				TEXT("SurferAnimInstance: state machine '%s' not found — hip-stab capture is falling back to the pushed AnimState, which can record a mid-pop-up pose"),
				*this->riderStateMachineName.ToString());
		}
		if (this->AnimState != ESurferAnimState::Surf || !bMachineInSurf)
		{
			this->HipStabUpdatesSeen = 0;
			return;
		}
		if (++this->HipStabUpdatesSeen <= this->hipStabCalibrationFramesInStance)
		{
			return;
		}
		if (FMath::Abs(pitchDeg) > this->hipStabCalibrationMaxTiltDeg ||
			FMath::Abs(rollDeg) > this->hipStabCalibrationMaxTiltDeg)
		{
			return;
		}
		const int32 pelvisIdx = mesh->GetBoneIndex(this->pelvisBoneName);
		const int32 footLIdx = mesh->GetBoneIndex(this->footBoneNameLeft);
		const int32 footRIdx = mesh->GetBoneIndex(this->footBoneNameRight);
		if (pelvisIdx == INDEX_NONE || footLIdx == INDEX_NONE || footRIdx == INDEX_NONE)
		{
			if (!this->bLoggedMissingHipStabBones)
			{
				this->bLoggedMissingHipStabBones = true;
				UE_LOG(LogSurf, Warning,
					TEXT("SurferAnimInstance: hip-stab bones not found (pelvis='%s' foot_l='%s' foot_r='%s') — hip stabilization disabled"),
					*this->pelvisBoneName.ToString(), *this->footBoneNameLeft.ToString(), *this->footBoneNameRight.ToString());
			}
			return;
		}

		const FTransform meshTF = mesh->GetComponentTransform();
		const FTransform pelvisWorld = mesh->GetBoneTransform(pelvisIdx);
		this->PelvisOffsetInStableFrame = pelvisWorld.GetRelativeTransform(stableFrame);
		this->PelvisSourceCS = pelvisWorld.GetRelativeTransform(meshTF);

		const FTransform footLCS = mesh->GetBoneTransform(footLIdx).GetRelativeTransform(meshTF);
		const FTransform footRCS = mesh->GetBoneTransform(footRIdx).GetRelativeTransform(meshTF);
		this->FootTargetLeft = footLCS.GetLocation();
		this->FootTargetRight = footRCS.GetLocation();
		this->FootRotationLeft = footLCS.GetRotation().Rotator();
		this->FootRotationRight = footRCS.GetRotation().Rotator();

		this->PelvisStabLocation = this->PelvisSourceCS.GetLocation();
		this->PelvisStabRotation = this->PelvisSourceCS.GetRotation().Rotator();
		this->bHipStabCalibrated = true;
		// Paste-ready block for the AnimBP Class Defaults' Stored Calibration section (needed
		// once a paddling/pop-up animation exists — see bHipStabUseStoredCalibration).
		const FVector p0l = this->PelvisOffsetInStableFrame.GetLocation();
		const FRotator p0r = this->PelvisOffsetInStableFrame.GetRotation().Rotator();
		const FVector psl = this->PelvisSourceCS.GetLocation();
		const FRotator psr = this->PelvisSourceCS.GetRotation().Rotator();
		UE_LOG(LogSurf, Display,
			TEXT("SurferAnimInstance: calibrating on actor='%s' component='%s' mesh='%s' state='%s'"),
			*GetNameSafe(boardActor), *GetNameSafe(mesh), *GetNameSafe(mesh->GetSkeletalMeshAsset()),
			machineIdx != INDEX_NONE ? *GetCurrentStateName(machineIdx).ToString() : TEXT("<no machine>"));
		UE_LOG(LogSurf, Display,
			TEXT("SurferAnimInstance: hip-stab calibrated after %d updates (board pitch=%.1f roll=%.1f). Stored-calibration values:\n")
			TEXT("  PelvisOffsetStable loc=(X=%.3f, Y=%.3f, Z=%.3f) rot=(P=%.3f, Y=%.3f, R=%.3f)\n")
			TEXT("  PelvisSource       loc=(X=%.3f, Y=%.3f, Z=%.3f) rot=(P=%.3f, Y=%.3f, R=%.3f)\n")
			TEXT("  FootTargetLeft     loc=(X=%.3f, Y=%.3f, Z=%.3f) rot=(P=%.3f, Y=%.3f, R=%.3f)\n")
			TEXT("  FootTargetRight    loc=(X=%.3f, Y=%.3f, Z=%.3f) rot=(P=%.3f, Y=%.3f, R=%.3f)"),
			this->HipStabUpdatesSeen, pitchDeg, rollDeg,
			p0l.X, p0l.Y, p0l.Z, p0r.Pitch, p0r.Yaw, p0r.Roll,
			psl.X, psl.Y, psl.Z, psr.Pitch, psr.Yaw, psr.Roll,
			this->FootTargetLeft.X, this->FootTargetLeft.Y, this->FootTargetLeft.Z,
			this->FootRotationLeft.Pitch, this->FootRotationLeft.Yaw, this->FootRotationLeft.Roll,
			this->FootTargetRight.X, this->FootTargetRight.Y, this->FootTargetRight.Z,
			this->FootRotationRight.Pitch, this->FootRotationRight.Yaw, this->FootRotationRight.Roll);
	}

	// Stabilized pelvis: hold the calibrated offset in the stable frame, expressed in component
	// space (world = relative * parent in UE transform composition).
	const FTransform targetWorld = this->PelvisOffsetInStableFrame * stableFrame;
	const FTransform targetCS = targetWorld.GetRelativeTransform(mesh->GetComponentTransform());

	const FQuat blendedRot = FQuat::Slerp(this->PelvisSourceCS.GetRotation(), targetCS.GetRotation(),
		FMath::Clamp(this->hipStabRotationAlpha, 0.0f, 1.0f));
	const FVector blendedLoc = FMath::Lerp(this->PelvisSourceCS.GetLocation(), targetCS.GetLocation(),
		FMath::Clamp(this->hipStabTranslationAlpha, 0.0f, 1.0f));

	if (this->hipStabInterpSpeed > 0.0f && this->bHipStabReady)
	{
		this->PelvisStabLocation = FMath::VInterpTo(this->PelvisStabLocation, blendedLoc, DeltaSeconds, this->hipStabInterpSpeed);
		this->PelvisStabRotation = FMath::RInterpTo(this->PelvisStabRotation, blendedRot.Rotator(), DeltaSeconds, this->hipStabInterpSpeed);
	}
	else
	{
		this->PelvisStabLocation = blendedLoc;
		this->PelvisStabRotation = blendedRot.Rotator();
	}
	this->bHipStabReady = true;
}
