// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimSequence.h"
#include "SurferAnimInstance.generated.h"

class AWeightDistribution;

/**
 * Which scripted body animation the rider is playing. The AnimBP's state machine reads AnimState
 * in its transition rules; AStateTriggerAutoPilot pushes it per-step (see riderAnimState on
 * FStateTriggerStep). Surf is the terminal ridden state — it is reached automatically when the
 * PopUp clip finishes, NOT pushed by the autopilot, and is the default so untagged steps and the
 * snapshot tests sit in the stance immediately. See specs/surfer-popup-animation-states.md.
 */
UENUM(BlueprintType)
enum class ESurferAnimState : uint8
{
	Paddle	UMETA(DisplayName = "Paddle"),		// looping paddle stroke (prone/kneeling)
	Cobra	UMETA(DisplayName = "Cobra"),		// paddle -> cobra pose, holds last frame
	PopUp	UMETA(DisplayName = "Pop Up"),		// cobra -> stand, plays once then auto-advances to Surf
	Surf	UMETA(DisplayName = "Surf")			// surf stance + procedural lean/hip-IK (default)
};

/**
 * Drives the surfer mannequin's procedural lean/twist from the SAME AWeightDistribution values
 * the physics consumes, so the visible rider and the board response cannot desynchronize.
 * Iteration 1: one fixed surf-stance pose + these offsets on top — no state machine, no clips.
 * The AnimBP (parent class = this) reads the three output floats into Transform (Modify) Bone
 * nodes; the mannequin mesh is attached to the surfboard, so board roll/pitch carries the rider
 * and the lean angles are deck-relative. Read-only w.r.t. the physics. See
 * specs/surfer-rider-lean.md.
 */
UCLASS()
class GONESURFING_API USurferAnimInstance : public UAnimInstance
{
	GENERATED_BODY()

public:
	// ========== Scripted animation state ==========

	/** Which scripted clip the rider plays. The AnimBP state machine reads this in its transition
	 *  rules; the autopilot pushes it per-step via SetAnimState. Surf is entered automatically when
	 *  the PopUp clip ends (a time-based transition) — the autopilot only pushes Paddle/Cobra/PopUp,
	 *  and a surfer cannot reach the stance without popping up first, so there is deliberately no
	 *  path into Surf other than the chain. See specs/surfer-popup-animation-states.md.
	 *
	 *  Defaults to Paddle to match the state machine's entry state. It used to default to Surf,
	 *  from before the pop-up clips existed, when the rider stood in the stance from frame one.
	 *  That default outlived its world: the hip-stab calibration gate keys on this variable, so
	 *  between BeginPlay and the autopilot's first push it read "Surf" while the machine was
	 *  actually playing the prone paddle clip — and the capture recorded the PADDLE pose as the
	 *  stance (feet 5 cm apart, pelvis 51 cm too low), which stored calibration then replayed
	 *  during the ride. Keep this in sync with the entry state. */
	UPROPERTY(BlueprintReadOnly, Category = "Surfer|State")
	ESurferAnimState AnimState = ESurferAnimState::Paddle;

	// ========== Pump ==========
	//
	// Deliberately NOT part of ESurferAnimState: pumping happens while surfing, on top of the
	// stance and its procedural lean, rather than instead of it. Drive the pump clip from a
	// Sequence EVALUATOR with its time set to PumpPhase x sequence length, not a Sequence Player -
	// that phase-locks the visible crouch to the physics stroke, so the rider compresses exactly
	// when the impulse is applied. A free-running player would drift against the cycle and the
	// animation would lie about when the pump is happening.

	/** True while a pump stroke is running. Unattenuated: it reports the stroke, not whether the
	 *  stroke is achieving anything, so the rider still visibly pumps at speeds where the force has
	 *  been attenuated away. See specs/pump-button-and-virtual-stick.md FR5b. */
	UPROPERTY(BlueprintReadOnly, Category = "Surfer|Pump")
	bool bPumping = false;

	/** Position within the current gesture as one number: 0..1 is the crouch, 1..2 the extension.
	 *  Kept for the input trace and for anything that just wants "how far through the pump". */
	UPROPERTY(BlueprintReadOnly, Category = "Surfer|Pump")
	float PumpPhase = 0.0f;

	/** How far into the crouch, 0..1. Drive the crunch-down clip's explicit time with
	 *  `PumpCharge x` its length: holding longer sinks the surfer further and holds him there.
	 *
	 *  HOLDS at the release depth (`PumpReleaseFromCharge`) for the whole extension, rather than
	 *  following the pawn's charge to 0 the tick the button comes up. The crouch clip is still on
	 *  screen while the rise blends in, and frame 0 of it is the standing stance - see the release
	 *  block in NativeUpdateAnimation for the pop-up-then-back-down this fixed. */
	UPROPERTY(BlueprintReadOnly, Category = "Surfer|Pump")
	float PumpCharge = 0.0f;

	/** True while the legs are extending after a release. Blend to the rise-up clip on this. */
	UPROPERTY(BlueprintReadOnly, Category = "Surfer|Pump")
	bool bPumpReleasing = false;

	/** 0..1 across the extension. Drive the rise-up clip's explicit time with
	 *  `PumpReleasePhase x` its length - it is also the window the board is being driven down. */
	UPROPERTY(BlueprintReadOnly, Category = "Surfer|Pump")
	float PumpReleasePhase = 0.0f;

	/** True while the rider is settling from the top of the extension back into the stance - the
	 *  pump's third movement. `bPumping` is already false here (so the outer stance blend is
	 *  running) while `bPumpReleasing` stays true with `PumpReleasePhase` pinned at 1, which holds
	 *  the rise clip on its last, TALL frame for the whole blend. Exposed for debugging and for
	 *  anything that wants to know the gesture is not visually finished; the AnimBP needs no wire
	 *  to it. Length is `PumpRecoverySeconds` on the tuning subsystem. */
	UPROPERTY(BlueprintReadOnly, Category = "Surfer|Pump")
	bool bPumpSettling = false;

	/** 1 = show the pump pair, 0 = show the surf stance. Holds at 1 for the whole stroke, then
	 *  eases to 0 across the settle (smoothstepped, so the rider decelerates into the stance rather
	 *  than arriving at a constant rate).
	 *
	 *  OPTIONAL, and it is what decides whether `PumpRecoverySeconds` is the real duration. The outer
	 *  Blend-by-bool on `bPumping` returns to the stance over ITS OWN Blend Time, so with that wiring
	 *  the settle only guarantees the blend has a tall pose to leave from - set that Blend Time to
	 *  match. Swap the node for a Blend taking this as its alpha and the AnimBP stops having an
	 *  opinion: the return takes exactly PumpRecoverySeconds, tunable on device from
	 *  Saved/TuningOverrides.json with no recompile. */
	UPROPERTY(BlueprintReadOnly, Category = "Surfer|Pump")
	float PumpStanceBlendAlpha = 0.0f;

	// ========== Tired (specs/stamina.md FR3) ==========
	//
	// Like the pump, deliberately NOT part of ESurferAnimState: tired happens while surfing, on top
	// of the stance, and the chain (Paddle -> Cobra -> PopUp -> Surf) must not grow a link the
	// one-step walk in SetAnimState could get stuck on.
	//
	// Three clips, driven from here the way the pump clips are - by explicit time, not a player -
	// so the whole sequence is one node pair in the AnimBP and every transition rule lives in code:
	//
	//   SK_tired_fade_in          surf stance -> slumped tired stance, once
	//   SK_tired_heavy_breathing  one deep breath, starts and ends in the tired stance, looped
	//   SK_tired_fade_out         tired stance -> surf stance, once
	//
	// AnimBP wiring: a Blend between the stance pose (A) and a Sequence Evaluator (B) whose
	// Sequence pin is bound to TiredSequence and Explicit Time to TiredSequenceTime, with
	// TiredStanceBlendAlpha as the alpha. Put it BEFORE the pump blend (a pump overrides tired)
	// and before the procedural lean bones (a tired rider still leans). No Blend Time is needed:
	// the fade-in's first frame and the fade-out's last frame ARE the stance, the same trick the
	// pump's crouch clip uses, so the alpha steps and nothing snaps.
	//
	// Assign the three sequences in the AnimBP's Class Defaults; with any of them missing the
	// tired pose is skipped and the flag alone says "tired".

	/** True while the rider is out of stamina (pool hit zero, not yet recovered to the exit
	 *  fraction). Set by the pawn; drives the clip phase below. */
	UPROPERTY(BlueprintReadOnly, Category = "Surfer|Tired")
	bool bTired = false;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Surfer|Tired")
	TObjectPtr<UAnimSequence> TiredFadeInSequence;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Surfer|Tired")
	TObjectPtr<UAnimSequence> TiredBreathingSequence;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Surfer|Tired")
	TObjectPtr<UAnimSequence> TiredFadeOutSequence;

	/** The clip to evaluate right now. Never null once the three are assigned: at rest it is the
	 *  fade-in at time 0 - the stance - so a Blend that still evaluates its B input shows nothing
	 *  wrong. Bind the Sequence Evaluator's Sequence pin to this. */
	UPROPERTY(BlueprintReadOnly, Category = "Surfer|Tired")
	TObjectPtr<UAnimSequence> TiredSequence;

	/** Explicit time into TiredSequence, seconds. Bind the evaluator's Explicit Time to this. */
	UPROPERTY(BlueprintReadOnly, Category = "Surfer|Tired")
	float TiredSequenceTime = 0.0f;

	/** 1 while any tired clip is on screen, 0 otherwise. Steps, never ramps - see above. */
	UPROPERTY(BlueprintReadOnly, Category = "Surfer|Tired")
	float TiredStanceBlendAlpha = 0.0f;

	// ========== Fall (specs/surfer-fall-ragdoll.md, "Authored fall") ==========
	//
	// The ride's ending, as an authored clip instead of the physics ragdoll: the rider goes off the
	// board to one side and ends lying in the water. Like tired and the pump, NOT a link in
	// ESurferAnimState - it is an overlay that replaces the whole pose, and the chain must not grow
	// a link the one-step walk could get stuck on. Driven by explicit time from here, once, and held
	// on the last frame for as long as the ride-over state lasts (the card comes up over it).
	//
	// AnimBP wiring, at the TOP LEVEL of the AnimGraph, last before Output Pose (after the state
	// machine and every procedural bone - a falling rider neither leans nor holds the deck):
	// a Blend Poses by Bool, Active Value = bFalling, True = a Sequence Evaluator (never a Player)
	// with Sequence bound to FallSequence and Explicit Time to FallSequenceTime, False = the pose
	// as it was, True Blend Time ~0.15 s (the clips start from the authored stance, but the live
	// rider carries lean + hip-IK on top of it, so this one needs a short cross-fade).
	//
	// Assign the two sequences in the AnimBP's Class Defaults; with either missing the pawn falls
	// back to the ragdoll (HasFallClips).

	/** The clip for going off the board's LEFT side (board.left = local -X), and the RIGHT. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Surfer|Fall")
	TObjectPtr<UAnimSequence> FallLeftSequence;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Surfer|Fall")
	TObjectPtr<UAnimSequence> FallRightSequence;

	/** True from StartFall until StopFall. Bind the Blend Poses by Bool's Active Value to this. */
	UPROPERTY(BlueprintReadOnly, Category = "Surfer|Fall")
	bool bFalling = false;

	/** The clip being evaluated: one of the two above while falling, else the left one at time 0
	 *  so an evaluator that still evaluates its input shows nothing wrong. Bind Sequence to this. */
	UPROPERTY(BlueprintReadOnly, Category = "Surfer|Fall")
	TObjectPtr<UAnimSequence> FallSequence;

	/** Explicit time into FallSequence, seconds; clamps at the end (holds the last frame). */
	UPROPERTY(BlueprintReadOnly, Category = "Surfer|Fall")
	float FallSequenceTime = 0.0f;

	/** Speed the fall clips play at (1 = as authored). The owner found the authored timing too slow
	 *  on the first look (2026-09-22); this is the knob, in the AnimBP's Class Defaults. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surfer|Fall", meta = (ClampMin = "0.1"))
	float FallPlayRate = 1.5f;

	/** Both fall clips assigned - the pawn's condition for the animated fall over the ragdoll. */
	bool HasFallClips() const { return this->FallLeftSequence && this->FallRightSequence; }

	/** Begin the fall clip for the given side of the BOARD. Restarts if already falling. */
	void StartFall(bool bToBoardRight);

	/** Back to the ordinary pose (the Replay path re-attaches the rider). */
	void StopFall();

	/** Wall-clock seconds the clip in play takes at FallPlayRate (0 when not falling), and whether
	 *  it has reached its end - the pawn holds the wipeout card until it has, then sinks the rider. */
	float GetFallClipLength() const;
	bool IsFallClipFinished() const;

	/** How deep the crouch was when the button came up, 0..1. A half-held pump must rise from
	 *  half-depth, not from the bottom. If the rise clip runs deep->tall over its length, start it
	 *  at `1 - PumpReleaseFromCharge` and play the remainder:
	 *      t0 = 1 - PumpReleaseFromCharge
	 *      ExplicitTime = (t0 + (1 - t0) * PumpReleasePhase) x ClipLength
	 *  A full-charge pump gives t0 = 0 and the whole clip. */
	UPROPERTY(BlueprintReadOnly, Category = "Surfer|Pump")
	float PumpReleaseFromCharge = 0.0f;

	/** Request a scripted animation state. This sets a TARGET; `AnimState` then walks toward it one
	 *  link per update (see NativeUpdateAnimation).
	 *
	 *  It is not a direct write, and must not become one. The AnimBP's transitions are chained
	 *  Paddle -> Cobra -> PopUp, each rule keyed on AnimState being exactly the next state, so a
	 *  value that SKIPS a link matches no rule and the machine stops for good — stranded in the
	 *  paddle clip while this variable carries on to Surf.
	 *
	 *  That is not hypothetical. AStateTriggerAutoPilot pushed per step, and its "Cobra position" and
	 *  "Pop up" steps can activate on the SAME frame, so AnimState went 0 -> 2 with the machine still
	 *  in SK_Paddle. Measured 2026-08-31 on Boards_on_flat_water: the rider stayed prone for the
	 *  whole ride in 2 of 4 runs, and the logged sequence never contained a 1. Whether the two steps
	 *  share a frame is timing-dependent, which is why it presented as "sometimes".
	 *
	 *  SurfboardPawn's rails driver already had this protection locally; it belongs here instead, so
	 *  that no caller — autopilot, rails, or anything added later — can skip a link. */
	UFUNCTION(BlueprintCallable, Category = "Surfer|State")
	void SetAnimState(ESurferAnimState NewState) { this->DesiredAnimState = NewState; }

	/** Full lateral weight shift (amountToTheRight 0 or 1) ⇒ this much body lean (deg).
	 *  Negate to flip the direction instead of editing the AnimBP graph. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surfer|Tuning")
	float maxLeanRightDeg = 18.0f;

	/** Full fore-aft weight shift (amountInFront 0 or 1) ⇒ this much fore-aft lean (deg). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surfer|Tuning")
	float maxLeanForwardDeg = 12.0f;

	/** Full lateral weight shift ⇒ this much upper-body/head twist toward the lean side (deg).
	 *  Shares amountToTheRight with the lean by design: weight left = lean left AND look left. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surfer|Tuning")
	float maxTwistYawDeg = 25.0f;

	/** FInterpTo speed (1/s) for all three outputs — the body's own inertia on top of the
	 *  weight input's smoothing. Higher = snappier rider. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surfer|Tuning", meta = (ClampMin = "0.1"))
	float leanInterpSpeed = 6.0f;

	/** How much of the board's WORLD roll is subtracted from the commanded lean. The mesh is
	 *  ATTACHED to the board, so the body inherits deck roll and then adds the lean on top —
	 *  double-leaning (observed: a small weight shift read as about-to-fall-off). A real surfer
	 *  absorbs roll in the ankles/knees and keeps the torso balanced over the feet: 1 = fully
	 *  world-stabilized torso + commanded lean, 0 = legacy deck-relative. NEGATIVE allowed — if
	 *  the board-roll sign convention opposes the calibrated lean axis, flip it HERE, not in
	 *  the graph. Board roll is computed geometrically from the owning actor's transform (works
	 *  live and during kinematic replay alike; no SharedCalculations dependency). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surfer|Tuning")
	float rollCompensation = 0.8f;

	/** Fore-aft sibling of rollCompensation (board world pitch vs LeanForward). 0 = off: a
	 *  surfer's fore-aft posture mostly follows the deck, so compensation is opt-in here. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surfer|Tuning")
	float pitchCompensation = 0.0f;

	/** Neutral down-the-line gaze (deg) folded into TwistYawDeg — a surfer looks along the wave,
	 *  not over the nose (player-calibrated -30 on Manny). Default 0 for compatibility: the
	 *  first AnimBP carries the -30 as a graph literal — EITHER keep that literal (leave this 0)
	 *  OR set -30 here and delete the graph add-node. Never both. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surfer|Tuning")
	float neutralTwistYawDeg = 0.0f;

	/** Outputs for the AnimBP's Transform (Modify) Bone nodes. Smoothed, degrees. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Surfer")
	float LeanRightDeg = 0.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Surfer")
	float LeanForwardDeg = 0.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Surfer")
	float TwistYawDeg = 0.0f;

	// ========== Hip stabilization + foot IK ==========
	// The hip holds a fixed transform in the board's location+yaw-only "stable frame" while the
	// deck pitches/rolls beneath it; the feet keep their flat-water component-space transforms
	// (component space IS deck space — the mesh is attached to the board) and two-bone leg IK
	// absorbs the difference. Supersedes rollCompensation/pitchCompensation (set those to 0 in
	// the AnimBP when wiring this up). See specs/surfer-rider-hip-ik.md.

	/** Master switch. False = outputs stay pass-through (bHipStabReady false) — legacy behavior. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surfer|HipStab")
	bool bHipStabEnabled = true;

	/** 0..1: how LEVEL the hip is held against board tilt. Cheap — never hits a leg reach limit. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surfer|HipStab")
	float hipStabRotationAlpha = 1.0f;

	/** 0..1: how firmly the hip holds its stable-frame POSITION. Deliberately < 1 by default:
	 *  a full lock at +-45deg board pitch exceeds leg length (knee-pop); 0.7 keeps the hip
	 *  visually planted while leaving the legs ~30% slack. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surfer|HipStab")
	float hipStabTranslationAlpha = 0.7f;

	/** Board |pitch| and |roll| (deg) must both be below this for the one-time calibration
	 *  (pelvis offset + foot targets captured from the unmodified stance pose). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surfer|HipStab")
	float hipStabCalibrationMaxTiltDeg = 3.0f;

	/** Evaluated frames the rider must have spent in the Surf state before the runtime
	 *  calibration may capture — long enough for the pop-up->stance transition blend to finish
	 *  (30 frames ~ 0.5 s at 60 fps covers the default 0.2 s blend with margin). The counter
	 *  resets whenever the state leaves Surf, so intros that paddle/pop-up first are safe. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surfer|HipStab")
	int32 hipStabCalibrationFramesInStance = 30;

	/** Optional smoothing (1/s) of the pelvis target; 0 = direct (board motion is already
	 *  smooth, so this is normally unnecessary). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surfer|HipStab")
	float hipStabInterpSpeed = 0.0f;

	/** Skeleton bone names (Manny defaults; adjust for a purchased model). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surfer|HipStab")
	FName pelvisBoneName = TEXT("pelvis");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surfer|HipStab")
	FName footBoneNameLeft = TEXT("foot_l");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surfer|HipStab")
	FName footBoneNameRight = TEXT("foot_r");

	// ---- Stored calibration ----
	// The runtime capture waits for the Surf anim state (plus a blend-settling delay) and for a
	// near-flat board. Shipped levels use these stored values instead so the pose does not depend
	// on the ride happening to level out: run ONE session with the switch below false, copy the
	// paste-ready "hip-stab calibrated" log block into these defaults, set it back true.
	// (Surfing_infinite_wave does reach the gate — measured ~120 frames within 3 deg per ride,
	// flattest around 5-6.5 s after handoff — so the recapture no longer needs a flat-water map;
	// Boards_on_flat_water is gone.)

	/** Use the stored values below instead of the runtime capture. Required once a
	 *  paddling/pop-up animation plays before the stance. Defaults below were captured
	 *  2026-08-23 from the Ash stance (SK_Surf_stance) via surf.hipstab.recapture on
	 *  Surfing_infinite_wave. NOTE storedFootRotationRight is bit-identical to the previous
	 *  Manny capture, which is suspicious - if the right foot sits wrong, check that the
	 *  right-foot IK node in the Surf state is gated on bHipStabReady like the pelvis is —
	 *  recapture (one run with this false, paste the log block) if the stance pose asset or the
	 *  rider's placement on the board changes. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surfer|HipStab|Stored Calibration")
	bool bHipStabUseStoredCalibration = true;

	/** The AnimGraph state machine and the stance state inside it. The capture waits for the
	 *  machine to actually BE in this state — see the gate in UpdateHipStabilization for why the
	 *  pushed AnimState is not sufficient. Update these if the graph is renamed. */
	UPROPERTY(EditAnywhere, Category = "Surfer|HipStab")
	FName riderStateMachineName = TEXT("RiderSM");

	UPROPERTY(EditAnywhere, Category = "Surfer|HipStab")
	FName surfStateName = TEXT("Surf");

	/** Pelvis offset in the board's stable frame ("P0"). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surfer|HipStab|Stored Calibration")
	FVector storedPelvisOffsetStableLocation = FVector(18.546, 25.330, 8.757);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surfer|HipStab|Stored Calibration")
	FRotator storedPelvisOffsetStableRotation = FRotator(64.881, -178.851, 98.710);

	/** The stance pose's pelvis in component space (blend origin for the alphas). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surfer|HipStab|Stored Calibration")
	FVector storedPelvisSourceLocation = FVector(6.923, -20.034, 25.598);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surfer|HipStab|Stored Calibration")
	FRotator storedPelvisSourceRotation = FRotator(64.183, 139.727, 108.099);

	/** Stance foot transforms in component (= deck) space. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surfer|HipStab|Stored Calibration")
	FVector storedFootTargetLeft = FVector(7.825, 3.974, -9.248);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surfer|HipStab|Stored Calibration")
	FVector storedFootTargetRight = FVector(7.023, -43.767, -9.717);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surfer|HipStab|Stored Calibration")
	FRotator storedFootRotationLeft = FRotator(86.845, 160.181, 93.674);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surfer|HipStab|Stored Calibration")
	FRotator storedFootRotationRight = FRotator(-85.919, 62.546, 153.721);

	/** True once calibrated and enabled. Wire to the pelvis Transform (Modify) Bone node's
	 *  alpha (and the IK nodes') so the whole block is pass-through until calibration — which
	 *  is also what makes the calibration capture feedback-free. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Surfer|HipStab")
	bool bHipStabReady = false;

	/** Stabilized pelvis transform, COMPONENT space — for a Transform (Modify) Bone on the
	 *  pelvis with Translation and Rotation = Replace Existing, Component Space. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Surfer|HipStab")
	FVector PelvisStabLocation = FVector::ZeroVector;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Surfer|HipStab")
	FRotator PelvisStabRotation = FRotator::ZeroRotator;

	/** Foot effector locations (COMPONENT space, captured at calibration = glued to the deck)
	 *  for the two-bone IK nodes. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Surfer|HipStab")
	FVector FootTargetLeft = FVector::ZeroVector;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Surfer|HipStab")
	FVector FootTargetRight = FVector::ZeroVector;

	/** Captured component-space foot rotations — for the OPTIONAL per-foot Transform (Modify)
	 *  Bone restore if the soles drift off the deck plane at high tilt (Two Bone IK preserves
	 *  the foot's LOCAL rotation, so its component-space sole direction can change as the leg
	 *  chain bends). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Surfer|HipStab")
	FRotator FootRotationLeft = FRotator::ZeroRotator;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Surfer|HipStab")
	FRotator FootRotationRight = FRotator::ZeroRotator;

	virtual void NativeUpdateAnimation(float DeltaSeconds) override;

	/** Kinematic-replay override: substitute recorded weight amounts (0..1, 0.5 = centered) for
	 *  the live AWeightDistribution reads — during replay the force pipeline is tick-disabled and
	 *  the weight actor is frozen at its pre-replay values, which held the rider in one constant
	 *  lean for the whole replay (observed). Terminal like the rest of replay mode (Restart
	 *  reloads the level). Board-attitude compensation needs no override: it reads the board
	 *  transform geometrically, and the replayed transform is the real recorded one. */
	void SetReplayWeights(float AmountInFront, float AmountToTheRight);

	/** Recorded pump stroke for kinematic replay, where the live WeightDistribution is tick-frozen. */
	void SetReplayPump(bool bPumpingNow, float Phase);

private:
	/** The board's weight-distribution actor. An AnimBP cannot reference level actors, so this is
	 *  resolved at runtime: prefer the AWeightDistribution whose Surfboard is the mesh's owning
	 *  actor (the board the mannequin is attached to), else the nearest one. Cached on success. */
	AWeightDistribution* ResolveWeightDistribution();

	UPROPERTY(Transient)
	TObjectPtr<AWeightDistribution> cachedWeightDistribution;

	/** Replay override state (see SetReplayWeights). */
	bool bReplayWeightOverride = false;
	/** Board roll the lean compensation used last update, for the surf.debug.flags rider log. */
	float LastBoardRollDeg = 0.0f;

	/** Pump settle state (see bPumpSettling). Seconds left, and the release depth that keeps
	 *  feeding the rise clip so its explicit time does not jump as the settle starts. */
	float PumpSettleRemaining = 0.0f;
	float PumpSettleFromCharge = 0.0f;

	/** Raw (pre-settle) releasing flag from the previous update, for spotting the tick the
	 *  extension completes. */
	bool bPumpWasReleasing = false;

	/** Tired clip phase (see the Tired block). Time is into the CURRENT clip. */
	enum class ETiredClip : uint8 { None, FadeIn, Breathing, FadeOut };
	ETiredClip TiredClip = ETiredClip::None;
	float TiredClipTime = 0.0f;
	/** Advance the tired clip machine one update and publish TiredSequence / Time / Alpha. */
	void UpdateTiredClips(float DeltaSeconds);

	/** Advance the fall clip (once, then hold) and publish FallSequence / FallSequenceTime. */
	void UpdateFallClip(float DeltaSeconds);

	bool ReplayPumping = false;
	float ReplayPumpPhase = 0.0f;
	float ReplayAmountInFront = 0.5f;
	float ReplayAmountToTheRight = 0.5f;

	// ========== Hip stabilization state ==========

	/** Compute the stable frame, run the one-time calibration, and update the pelvis/foot
	 *  outputs. Called at the end of NativeUpdateAnimation. */
	void UpdateHipStabilization(float DeltaSeconds);

	/** True once the calibration has run (it is one-time per level). */
	bool bHipStabCalibrated = false;

	/** True when bHipStabCalibrated was satisfied from the stored values rather than a runtime
	 *  capture. Lets surf.hipstab.recapture undo it: -ExecCmds CVars are applied a few frames
	 *  after BeginPlay, by which point the stored branch has already run on frame 0. */
	bool bHipStabCalibratedFromStored = false;

	/** Pelvis transform relative to the stable frame, captured on flat water ("P0"):
	 *  the transform the pelvis must hold as the deck tilts. */
	FTransform PelvisOffsetInStableFrame;

	/** The unmodified stance pose's pelvis in component space (blend origin for the alphas).
	 *  Feedback-free: captured while bHipStabReady was still false, i.e. before the AnimBP's
	 *  Replace node started rewriting the pelvis. */
	FTransform PelvisSourceCS;

	/** Last valid stable-frame yaw (deg) — held when the board points too vertical for the
	 *  horizontal-forward yaw derivation (unreachable while riding, but guarded anyway). */
	float StableFrameYawDeg = 0.0f;

	/** One-shot missing-bone warning guard. */
	bool bLoggedMissingHipStabBones = false;

	/** One-shot missing-state-machine warning guard. */
	bool bLoggedMissingRiderStateMachine = false;

	/** Where SetAnimState wants the rider to get to. AnimState walks toward this one link per
	 *  update; they differ only while the chain is being walked. */
	ESurferAnimState DesiredAnimState = ESurferAnimState::Paddle;

	/** Last values reported by the pushed-vs-machine diagnostic in NativeUpdateAnimation, so it
	 *  logs on change rather than every frame. */
	FName LastLoggedMachineState = NAME_None;
	ESurferAnimState LastLoggedAnimState = ESurferAnimState::Surf;

	/** Consecutive updates seen in the Surf state (reset on any other state). Calibration waits
	 *  hipStabCalibrationFramesInStance of these so the pop-up->stance blend has settled and the
	 *  evaluated pose is the pure stance — the early frames after spawn additionally return the
	 *  REFERENCE pose, which once folded the rider onto the deck (observed). */
	int32 HipStabUpdatesSeen = 0;
};
