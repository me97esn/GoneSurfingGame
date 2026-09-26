// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include <utility>
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Buoyancy.h"
#include "SharedCalculations.h"
#include "ForceQueueManager.h"
#include <deque>
#include "FluidDynamics.generated.h"

class USurfTuningSubsystem;

UENUM(BlueprintType)
enum class ESide : uint8
{
	VE_Unset UMETA(DisplayName = "Set in BeginPlay"),
	VE_Left UMETA(DisplayName = "Left"),
	VE_Right UMETA(DisplayName = "Right"),
	VE_Down UMETA(DisplayName = "Bottom"),
	VE_Tail UMETA(DisplayName = "Tail"),
	VE_Fin UMETA(DisplayName = "Fin"),
	VE_Nose UMETA(DisplayName = "Nose")
};


UCLASS()
class GONESURFING_API AFluidDynamics : public AActor
{
	GENERATED_BODY()
	
public:	
	// Sets default values for this actor's properties
	AFluidDynamics();

	FVector left; 
	FVector forwards;
	FVector up;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FVector gravity = FVector(0.0, 0.0, -9.81);

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	bool debugAngleOfAttack = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	bool debugWaterVelocity = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	bool debugLift = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	bool debugThrust = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	bool debugDrag = false;

	/** Draw debug arrows/cones for lift, thrust, and drag forces. Independent from the per-category log flags above. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	bool debugDrawForces = false;

	/** Scale factor for force-debug drawings (arrows and cones). Lower = shorter visualization. Forces are in N which can be 10000+, so a small scale (0.01 default) keeps arrows readable. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(ClampMin = "0.0"))
	float debugForceDrawScale = 0.01f;

	/** Throttle for the debug UE_LOG calls — only log on every Nth tick. 1 = log every tick, 30 ≈ 2 Hz, 60 = 1 Hz. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(ClampMin = "1"))
	int32 debugLogEveryNthTick = 1;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float DebugDrawDurationTime;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	AActor* Surfboard;

	/** The simulating physics body of the surfboard. Forces from this FluidDynamics actor
	 *  are applied as impulses (force × DeltaTime) at this actor's world location to that body
	 *  via AddImpulseAtLocation. Set in BP BeginPlay (cast from the Surfboard actor's mesh). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	UPrimitiveComponent* surfboardMesh;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	ASharedCalculations* sharedCalculations;

	UPROPERTY(EditAnywhere, BlueprintReadOnly)
	FColor debugLiftColor = FColor::Green;

 	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float thrustMagnitude = 1.0;

	/** Coefficient for the lateral carving force (force toward the rail the rider leans onto).
	 *  Magnitude scales with worldRelativeRollSin (lean vs horizontal) × |sinPitch| × relWaterVelMag
	 *  × AmountPlaning × lateralTurnMultiplier × this coefficient. Keyed on WORLD roll (not wave-relative)
	 *  so a level board on a steep face doesn't carve at neutral — see specs/lateral-turn-world-relative-roll.md.
	 *  Replaces the old alongThrust mechanism. Set on bottom-side FluidDynamics actors only.
	 *  Overwritten at runtime by Tuning->lateralTurnCoefficient; this is only the fallback when no
	 *  tuning subsystem is present, kept in sync with the tuning default (4000). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float lateralTurnCoefficient = 4000.0f;

	/** Saturating speed cap (cm/s) for the lateral carve term. relWaterVelMag is ~150 in normal trim
	 *  but spikes to ~1200+ during a hard turn, so the linear speed factor turns a few-degree residual
	 *  roll into a runaway carve. Replaces the raw speed with vEff = v / sqrt(1 + (v/cap)^2): ~linear
	 *  below the cap, asymptotes to the cap above it. <= 0 disables (raw speed). Overwritten at runtime
	 *  by Tuning->lateralTurnSpeedCap. See specs/carve-grip-via-redirect.md. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float lateralTurnSpeedCap = 300.0f;

	/** Small-roll deadzone (in sin units, i.e. ~radians for small angles) gating the carve term via a
	 *  smoothstep: zero carve below this |worldRelativeRollSin|, ramping to FULL by 2x it. The
	 *  down-the-line over-carve is seeded by only ~2-6deg of residual roll ("no visible roll") that
	 *  runs away at planing speed; the gate stops sub-threshold roll from seeding it while a deliberate
	 *  lean keeps full carve above the band. 0 disables. e.g. 0.04 ~= 2.3deg. Overwritten at runtime
	 *  by Tuning->lateralTurnRollDeadzone. See specs/carve-grip-via-redirect.md. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float lateralTurnRollDeadzone = 0.04f;

	/** Additive hard-carve, keyed on the surfer's COMMANDED lean (amountToTheRight), NOT the board's
	 *  achieved roll. The board is roll-stiff (banks only ~2-3deg on a hard lean, worldRollSin at the
	 *  deadzone), so the roll-gated carve is ~0 in a hard turn. This ADDS a carve of
	 *  boost * smoothstep(start, full, |amountToTheRight-0.5|) * (rocker/speed/planing/coef terms),
	 *  toward the commanded rail, so a hard commit turns even when the board won't bank. Zero below
	 *  start, so trim is untouched. 0 = off. Overwritten at runtime by Tuning->lateralTurnHardCarveBoost
	 *  (default 0.5). See specs/hard-carve-progressive-boost.md. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float lateralTurnHardCarveBoost = 0.5f;

	/** Commanded-lean band (|amountToTheRight-0.5|, 0..0.5) over which the hard-carve boost ramps in.
	 *  Start sits above the gentle trim band so down-the-line trim is untouched. Overwritten at runtime
	 *  by Tuning->lateralTurnHardCarveStart / lateralTurnHardCarveFull. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float lateralTurnHardCarveStart = 0.30f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float lateralTurnHardCarveFull = 0.45f;

	/** Per-actor scalar on the lateral carving force, applied AFTER the shared coefficient and
	 *  rocker/velocity/planing factors. Lets the level designer ramp the force from full at the
	 *  nose (1.0) to zero at the tail (0.0). The carving force at an actor *behind* CoM yaws the
	 *  nose AWAY from the dipped rail (verified by isolation-test on 2026-06: front+back firing
	 *  together produced ~0° net yaw change vs the no-lateralTurn baseline because the back's
	 *  CCW contribution cancelled the front's CW one). Default 1.0 preserves legacy behavior on
	 *  any actor that hasn't been re-tuned; set to 0.0 (or low) on tail-side bottom actors to
	 *  recover the carving torque. Set on bottom-side actors only. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float lateralTurnMultiplier = 1.0f;

	/** Wave-slope gravity supplement. Adds a per-bottom-actor force along the true downhill
	 *  vector on the wave surface (waveSlopeDownVec, magnitude = sin(slope)), on top of natural
	 *  UE gravity. With UE gravity restored to stock (~-980 cm/s²) the natural gravity-along-
	 *  slope component does most of the work; this coefficient supplements it for cases where
	 *  more aggressive down-the-face acceleration is wanted (e.g. fast-take-off tuning).
	 *
	 *  Direction = waveSlopeDownVec (unit downhill on the wave-tangent plane × sin(slope)),
	 *  NOT board.forwards — so the force always points down the wave regardless of board heading.
	 *  Trade-off: on a board angled across the wave face, this introduces a sideways-relative-
	 *  to-heading component that can skid the tail. The original waveSlopeSupplement (pre-
	 *  2026-05-19) projected along board.forwards instead, which avoided the skid but was
	 *  physically wrong (could "climb up" the wave face). Pick your trade-off.
	 *
	 *  Two gates restrict when the supplement fires (see specs/wave-slope-gravity-supplement.md):
	 *    1. AmountPlaning > 0 — only while the board is actively surfing. Cuts trough-side drift:
	 *       waveSlopeDownVec on the back side of an incoming wave points along +X (back toward
	 *       the previous trough), so without this gate a stationary board gets pulled backwards
	 *       as the next wave approaches.
	 *    2. smoothstep(0.10, 0.20, slopeSin) — only on real wave faces, not residual ripple
	 *       noise. Without this gate, a planing board after the wave passes keeps getting kicks
	 *       on small slopes (0.02–0.09) and never decelerates, because planing-attenuated drag
	 *       isn't enough to balance even a small supplement multiplied by full AmountPlaning.
	 *
	 *  Bottom side only (avoid double-counting from rails/fins/tail). Header default 0.0f —
	 *  set per-instance in the umap on each VE_Down FluidDynamics actor. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float waveSlopeGravityCoefficient = 0.0f;

	/** Per-actor cap on the wave-slope-gravity supplement force magnitude. Mirrors the
	 *  `maxHydrofoilForceAmount` pattern. 0 or negative disables the cap.
	 *
	 *  Why it exists: the supplement scales as `coef × slopeSin × planing` and all bottom
	 *  actors read the same SC-shared `waveSlopeDownVec`, so 10× actors push in the same
	 *  direction. At high `waveSlopeGravityCoefficient` (which is the dominant lever for
	 *  forward speed), peak per-actor force can spike to 60-70 kN during the catch — 600-700
	 *  kN board-total — which, when redirected through transient board tilts, has huge Z and
	 *  Y components that overwhelm sideways/vertical damping and launch the board. The cap
	 *  bounds the spike while preserving the cruise-phase force at typical magnitudes.
	 *
	 *  Default 5000 N/actor matches `maxHydrofoilForceAmount`. With 10 bottom actors, that's
	 *  50 kN total board-cap on the supplement direction. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float maxSupplementForce = 5000.0f;

/** Bottom hydrofoil thrust — *forward* component coefficient. Decoupled from
	 *  upwardsThrustCoefficient so the forward share of the perpendicular-to-flow reaction can be
	 *  tuned independently from upthrust. Use case: on flat water during a sharp turn, the bottom
	 *  hydrofoil produces a forward push that keeps the board from bleeding speed — this lets that
	 *  push be dialed up without also amplifying the AOA-coupled upthrust. At equal coefficients
	 *  the formula reduces to the unified perpendicular-to-flow force (the spec's original
	 *  single-coefficient design); cranking this higher than upwardsThrustCoefficient gives more
	 *  forward thrust per unit AOA without raising the upthrust. Set on bottom-side actors only. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float forwardsThrustCoefficient = 0.005f;

	/** Bottom hydrofoil thrust — *actor-local* forward component coefficient. Independent from
	 *  forwardsThrustCoefficient (which projects onto board-shared forwards). This term projects
	 *  onto each actor's OWN forward axis, which on rocker-rotated actors tilts up at the nose
	 *  and down at the tail. The two coefficients combine additively per-actor: the board-shared
	 *  term gives uniform straight-line propulsion; the actor-local term adds rocker coupling
	 *  (front-loaded thrust on the nose, tail-loaded on the tail) that can influence pitch and
	 *  carve geometry. Default 0 = no rocker coupling (board-shared term alone). Going too high
	 *  causes pitch runaway because the BP rocker angles (~20°/actor) translate ~34% of this
	 *  term into world Z; bisection on 2026-05-19 found stability up to ~0.001 (scale relative
	 *  to forwardsThrustCoefficient). Set on bottom-side actors only. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float actorForwardsThrustCoefficient = 0.0f;

	/** Pitch-AOA amplifier for the bottom hydrofoil upthrust. The upForce magnitude is
	 *  v² × sinAOA × amountWetted × (upwardsThrustCoefficient + upwardsThrustPitchSensitivity × sinAOA) × upDot,
	 *  which adds a quadratic-in-sinAOA term on top of the linear upthrust. At cruise AOA
	 *  (rocker ≈ 5°, sinAOA ≈ 0.087) the boost is small; at a forward-weight nose-dive AOA
	 *  (~15-20°, sinAOA ≈ 0.3) the boost is several × larger relative to the linear term —
	 *  giving the board a stiffer counter-torque against weight-shift-induced pitch without
	 *  inflating cruise force. Default 0 = pure linear upthrust (legacy). Only affects the
	 *  up-axis component; boardFwd/actorFwd thrust components stay linear. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float upwardsThrustPitchSensitivity = 0.0f;

	/** Yaw-AOA hydrofoil thrust on the bottom. Force is perpendicular to in-plane flow in the
	 *  board's bottom plane (forwards × left), with anti-slip and forward components proportional
	 *  to cos(slip) and |sin(slip)| respectively. Magnitude scales with
	 *  v²InBottomPlane × |sinSlip| × amountUnderWater × effectiveWaterHeight × this coefficient.
	 *  Mirrors the fin's carve-coupling shape but applied to the much larger bottom surface, so
	 *  this coefficient is typically smaller than finLiftMagnitude despite the same force shape.
	 *  Decoupled from the pitch hydrofoil (upwards/forwardsThrustCoefficient): pitch projects flow
	 *  onto the forwards × up plane, yaw onto the forwards × left plane — they sum without overlap.
	 *  Default 0 = disabled. Set on bottom-side actors only. See specs/bottom-yaw-hydrofoil.md. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float yawHydrofoilCoefficient = 0.0f;

	/** Speed-attenuation band (cm/s, on relWaterVelMag) for the yaw-hydrofoil FORWARD (carve-coupling)
	 *  thrust. That thrust is coef·v²·sin²(slip)·... and v² grows with board speed → unbounded speed
	 *  positive-feedback that surges the board then lets the v²-drags slam it back. Taper the forward
	 *  component to zero as relWaterVelMag goes start→end (mirrors the pump speed attenuation) so it drives
	 *  at low speed but self-limits to a cruise. start >= end disables it (default). Overwritten by
	 *  Tuning->yawThrustAttenStart/End. See specs/yaw-thrust-speed-attenuation.md. */
	float yawThrustAttenStart = 0.0f;

	/** Yaw-hydrofoil FORWARD-drive gating (FR1 of specs/flat-water-propulsion-audit.md). Mirrored from
	 *  the subsystem each tick; see there for why the forward half is gated and the side half is not. */
	/** Bottom forward-drag shaping, mirrored from the subsystem each tick.
	 *  See SurfTuningSubsystem for why these two exist. */
	UPROPERTY()
	float bottomForwardDragMultiplier = 10.0f;

	UPROPERTY()
	float forwardDragWettingGate = 1.0f;

	/** Contact gate, mirrored from the subsystem each tick. 0 = the fix (the default, device-verified
	 *  2026-09-23); 1 reproduces the old ungated behaviour bit for bit, for A/B.
	 *  See SurfTuningSubsystem and specs/airborne-force-gating.md. */
	UPROPERTY()
	float airborneForceScale = 0.0f;

	UPROPERTY()
	float airborneFadeDistance = 10.0f;

	UPROPERTY()
	float yawFwdSlopeGateMin = 0.10f;

	UPROPERTY()
	float yawFwdSlopeGateWidth = 0.06f;

	UPROPERTY()
	float yawFwdOffFaceFloor = 0.0f;
	// Nose-with-the-wave gate thresholds (see USurfTuningSubsystem::yawFwdWithWaveGateOff/On).
	float yawFwdWithWaveGateOff = -1.0f;
	float yawFwdWithWaveGateOn = -0.90f;
	float yawFwdWithWaveGateSpeedMax = 150.0f;
	// A2: face-keyed drives x (1 - brokenGeo * BrokenDriveCut). Mirrored from the subsystem.
	float brokenDriveCut = 2.0f;
	/** 1 on a clean face, (1 - BrokenDriveCut) in the whitewater (specs/broken-wave-no-consequences.md A2). */
	float calcBrokenDriveScale() const;
	float yawThrustAttenEnd   = 0.0f;

	/** Scales the force-based anti-slip *side* component (yaw-hydrofoil + fin-lift `antiSlipDir·cosSlip`)
	 *  so it can be dialed down as the carve-grip velocity redirect takes over slip-resistance. 1 =
	 *  unchanged (Phase 1 default); 0 = removed. Overwritten by Tuning->AntiSlipForceScale.
	 *  See specs/carve-grip-via-redirect.md. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float antiSlipForceScale = 0.0f;

	// bottomDragSidewaysCoefficient and railDragSidewaysCoefficient were removed in
	// specs/redundant-sideways-drag-removal.md — they were per-actor variants of the same
	// physical effect that waveMassFlowDragCoefficient now handles board-wide.

	/** Wave-mass thrust coefficient. Converts the sideways component of wave-driven water flow
	 *  into forward propulsion along board.forwards via the hydrofoil-redirect mechanism.
	 *  Distinct from waveMassFlowDragCoefficient (which carries the board in the water's actual
	 *  flow direction). Magnitude scales with
	 *  effectiveWaterHeight × slopeSin × sidewaysVel² × this coefficient. Gated to steep wave
	 *  faces by slopeSin so flat-water carving is untouched. Default 0 = disabled. Set on
	 *  bottom and rail actors. See specs/wave-mass-thrust.md. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(ClampMin = "0.0"))
	float waveMassThrustCoefficient = 1e-4f;

	/** Wave-mass flow drag coefficient. Models the wave's water mass pushing the board in the
	 *  direction the water is actually flowing. Force direction = boardWideAbsoluteWaterVelocity.unit
	 *  (averaged across both SCs to eliminate per-actor sampling asymmetry). Replaces the
	 *  previous waveMassDrag (forward-only, scale-leaked) and waveMassSidewaysDrag (perpendicular,
	 *  board-frame). Magnitude scales with
	 *  boardWideEffectiveWaterHeight × boardWideSlopeSin × |boardWideAbsWaterVel|² × this coefficient.
	 *  Gated to wave faces by boardWideSlopeSin > 0. Default 0 = disabled. Applied on bottom and
	 *  on the engaged rail. See specs/wave-mass-flow-drag.md. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(ClampMin = "0.0"))
	float waveMassFlowDragCoefficient = 0.0f;

	/** Min slopeSin before any wave-mass force (flow drag bottom/rail/tail, wave-mass thrust)
	 *  engages; SmoothStep(min, min + 0.06, slopeSin) scales the magnitude. Delays takeoff until
	 *  the face is steep. 0 = legacy (fires from slopeSin 0). Overwritten each tick from the
	 *  tuning subsystem. See specs/steep-face-takeoff.md. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(ClampMin = "0.0"))
	float waveMassMinSlopeSin = 0.35f;

	/** Decouples the wave-mass FLOW drag (`waveMassFlowDrag` on bottom, `flowDrag` on rail) from roll by
	 *  relocating its application point from the actor toward the board's forward axis through the CoM
	 *  (zeroing the lateral AND vertical moment arms → no roll, incl. the keel term — while the front/back
	 *  pitch survives). 0 = legacy (applied at the actor); 1 = fully on the CoM longitudinal axis (fix).
	 *  Overwritten at runtime by Tuning->waveMassRollDecouple. See specs/wave-mass-drag-torque-decoupling.md. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(ClampMin = "0.0", ClampMax = "1.0"))
	float waveMassRollDecouple = 1.0f;

	/** Decouples the wave-mass FLOW drag from YAW (on top of the roll decouple above). The flow drag's
	 *  yaw (`r_fwd·F_left`) survives the roll relocation and was measured as the DOMINANT into-the-wave
	 *  steering torque down the line (`dragBottom_waveMassFlow` + `dragRailFlow` ≈ -64k net yaw), turning
	 *  the nose up the face at centred weight. This strips the yaw component of the applied angular impulse
	 *  while keeping the wanted nose-up-into-the-face PITCH — spec option C (linear at CoM + add back only
	 *  the chosen torque axes). 0 = legacy option B (keep yaw); 1 = strip yaw (fix). Overwritten by
	 *  Tuning->waveMassFlowYawDecouple. See specs/wave-mass-drag-torque-decoupling.md. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(ClampMin = "0.0", ClampMax = "1.0"))
	float waveMassFlowYawDecouple = 1.0f;

	/** Same option-C yaw strip as waveMassFlowYawDecouple, but for the wave-PENETRATION "wall" drag
	 *  (`wavePenetrationDrag`, the glide-through resistance). Its yaw was measured as the #1 into-the-wave
	 *  steering torque remaining after the flow terms were decoupled. Stripping it (keep the linear wall +
	 *  pitch, drop roll+yaw) removes that residual weathervane while preserving the glide-through stop.
	 *  Separate knob so it can be A/B'd independently of the flow decouple. 0 = legacy (at-actor, keep yaw);
	 *  1 = strip. Overwritten by Tuning->wavePenetrationYawDecouple. See specs/wave-mass-drag-torque-decoupling.md. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(ClampMin = "0.0", ClampMax = "1.0"))
	float wavePenetrationYawDecouple = 1.0f;

	/** Low-pass time constant (s) for the wave-mass FLOW push — EMA (alpha = DeltaTime/tau) on the applied
	 *  force. Smooths the flow spikes on top of the yaw-thrust speed attenuation (both needed for the smooth
	 *  glide). 0 = off. Overwritten by Tuning->waveMassSmoothingTau. See specs/yaw-thrust-speed-attenuation.md. */
	float waveMassSmoothingTau = 0.0f;

	/** Scratch accumulator (per tick, not tuned): the wave-mass flow drag stashed by calcDragForce so
	 *  applyDragAsImpulse can apply it at getWaveMassApplyPoint() instead of the actor. */
	FVector PendingWaveMassForce = FVector::ZeroVector;

	/** Persistent EMA state (NOT reset per tick) for the wave-mass flow push low-pass (waveMassSmoothingTau). */
	FVector SmoothedWaveMassForce = FVector::ZeroVector;

	/** Scratch accumulator (per tick, not tuned): the wave-penetration "wall" drag, applied via the same
	 *  option-C path as PendingWaveMassForce but gated by wavePenetrationYawDecouple. */
	FVector PendingWavePenetrationForce = FVector::ZeroVector;

	/** Decouples railLift's keel-roll: railLift is a sideways force below the CoM, so applying it at the
	 *  actor makes a -r_up·F_left roll torque (the dominant down-the-line destabiliser). Apply it at CoM
	 *  HEIGHT instead (zero the vertical arm) → no roll, while keeping the sideways grip and the yaw.
	 *  0 = legacy at-actor; 1 = at CoM height (fix). Overwritten by Tuning->railLiftRollDecouple. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(ClampMin = "0.0", ClampMax = "1.0"))
	float railLiftRollDecouple = 1.0f;

	/** Scratch (per tick): railLift stashed by calcLiftForce so applyLiftAsImpulse applies it at CoM height. */
	FVector PendingRailLiftForce = FVector::ZeroVector;

	/** Wave penetration resistance — "wall" drag along the wave's HORIZONTAL normal that resists the
	 *  board crossing the face (in or out), ungated by planing. Keeps the board ON the face (surfing
	 *  down the line) instead of charging through to the back. Tangential motion is unaffected
	 *  (zero normal component). Magnitude = coef × effectiveWaterHeight × vAcrossFace². Bottom-side
	 *  actors only. Default 0 = disabled. See specs/wave-penetration-resistance.md. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(ClampMin = "0.0"))
	float wavePenetrationCoefficient = 0.0f;

	/** Deadzone (cm/s) for wave penetration resistance: only cross-face speed ABOVE this is resisted,
	 *  so normal riding/pumping and the wave's steady normal water flow aren't braked (NFR1). Lets the
	 *  coefficient be strong at a fast punch-through. See specs/wave-penetration-resistance.md. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(ClampMin = "0.0"))
	float wavePenetrationThreshold = 200.0f;

	/** Passive slope thrust — gravity-down-the-wave-face drive, redirected along board.forwards.
	 *  Restores the passive propulsion lost when waveSlopeGravity was replaced by the (intent-gated)
	 *  hydrofoil thrust: a neutrally-trimmed board on a wave face should hold speed down the line
	 *  instead of coasting to a stop and washing through. Force = board.forwards · this · amountWetted
	 *  · (boardWideWaveSlopeDown · board.forwards) — the projection of the down-slope gravity onto the
	 *  forward axis, applied along it (the fins/rails redirect the down-slope pull into forward motion).
	 *  Self-handles every case: flat water (slopeDown ≈ 0) → no drive → decelerates; nose pointed UP the
	 *  face (projection < 0) → decelerates, never pushes up the wave. Board-wide slope (averaged across
	 *  both SCs) avoids per-actor yaw noise; gated on amountWetted (hull engaged), NOT the squashed
	 *  amountUnderWater. Independent of turnGate — this is the always-on passive drive; the hydrofoil
	 *  forwardsThrust stays as the intent/pump-gated active booster. Magnitude is bounded by this
	 *  coefficient (|F| ≤ coef, since |slopeDown · forwards| ≤ sin(slope) ≤ 1). Default 0 = disabled.
	 *  Bottom-side actors only. See specs/passive-slope-thrust.md. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(ClampMin = "0.0"))
	float slopeThrustCoefficient = 0.0f;

	/** Minimum board-wide slopeSin before passive slope thrust engages. Deadzone that keeps the drive
	 *  OFF on near-flat water between waves / in the trough (slopeSin below this → no force; ramps to
	 *  full over a small band above it). Without it the strong coefficient flung a slow board around on
	 *  any residual slope. Raised 0.12 → 0.35 (~20° face) for the steep-face takeoff; keep equal to
	 *  waveMassMinSlopeSin. Bottom-side actors only. See specs/passive-slope-thrust.md and
	 *  specs/steep-face-takeoff.md. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(ClampMin = "0.0"))
	float slopeThrustMinSlopeSin = 0.35f;

	/** Fraction of the down-slope gravity's LATERAL (board-left) component that the fins/rails redirect
	 *  into forward drive. Lets the board gain speed traversing the face (rail ~parallel to the wave)
	 *  and removes the stall when the nose points up-face (where the forward-projection drive clamps to
	 *  0). 0 = pure forward-projection drive. Bottom-side actors only. See specs/passive-slope-thrust.md. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(ClampMin = "0.0"))
	float slopeThrustFinRedirect = 0.5f;

	/** Front-face gate direction for passive slope thrust (world space, normalized in use). The drive is
	 *  scaled by how well the board-wide down-slope aligns with this vector — full on the front face,
	 *  suppressed on the BACK of the crest (where down-slope reverses and the drive would otherwise power
	 *  the board over/through the wave). (0,0,0) disables the gate. Set from the tuning subsystem; the
	 *  per-actor default is the zero vector (gate off) so an unconfigured actor keeps legacy behaviour.
	 *  See specs/wave-crossing-deceleration.md. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FVector slopeThrustFrontFaceDir = FVector::ZeroVector;

	/** Non-zero baseline contribution (cm) to effectiveWaterHeight. Keeps per-surface
	 *  drag/lift/thrust forces non-zero in the trough where waterColumnAbove and slopeSin
	 *  would both be 0. Hardcoded value used during water-mass tuning: 10.0. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(ClampMin = "0.0"))
	float baseHeight = 10.0f;

	/** cm contribution to effectiveWaterHeight when the wave surface is fully vertical
	 *  (slopeSin = 1); scales linearly with slopeSin in between. Captures wave-mass signal
	 *  even when the actor isn't submerged. Hardcoded value used during water-mass tuning: 50.0. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(ClampMin = "0.0"))
	float slopeHeight = 50.0f;

	/** Upper bound (cm) on effectiveWaterHeight. Without this, waterColumnAbove can grow
	 *  arbitrarily large under a breaking wave wall — every per-surface force scales linearly
	 *  with effectiveWaterHeight, so the unbounded growth produced multi-meganewton thrust
	 *  spikes that launched the board off the wave. Typical steady-state values are 50-100;
	 *  the cap sits well above that so normal surfing is unaffected. Tune up if a future
	 *  scenario needs more water-mass headroom; tune down if even capped forces still spike. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(ClampMin = "0.0"))
	float maxEffectiveWaterHeight = 200.0f;

	/** Per-actor saturation (N) on the bottom hydrofoil's pitch and yaw force vectors. Clamps
	 *  each force vector's magnitude independently; preserves direction. Stand-in for the
	 *  cavitation/stall behavior real hydrofoils exhibit at extreme AOA or velocity. Without
	 *  this, asymmetric submersion across front/back (e.g., board pitched over a wave crest)
	 *  produces force differentials that create runaway yaw torque (~600°/s spin observed).
	 *  Normal aggressive surf sees per-actor forces in the 100-2000 N range; default 5000 N
	 *  leaves headroom for hard carving. Set to 0 to disable. See specs/hydrofoil-force-cap.md. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(ClampMin = "0.0"))
	float maxHydrofoilForceAmount = 5000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly)
	FVector componentVelocity;

	UPROPERTY(EditAnywhere, BlueprintReadOnly)
	ESide side = ESide::VE_Unset;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly)
	double cosPitchAngleOfAttack = 0.0;
	
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly)
	double sinPitchAngleOfAttack = 0.0;
	
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly)
	double cosRollAngleOfAttack;
	
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly)
	double cosYawAngleOfAttack;

	/** Vertical distance from this actor up to the wave surface at the same (X,Y), clamped to 0.
	 *  Refreshed each tick in setup(). Raw measurement only; force formulas now use
	 *  effectiveWaterHeight, which combines this with a baseline and a wave-slope contribution. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly)
	float waterColumnAbove = 0.0f;

	/** sin of the wave-surface slope angle at this actor's (X,Y): sqrt(1 - normal.Z²).
	 *  0 on flat water, approaches 1 on a vertical wave wall. Refreshed each tick in setup(). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly)
	float slopeSin = 0.0f;

	/** What every per-surface drag/lift/thrust formula multiplies by — the per-surface mass-of-water
	 *  proxy. Computed as baseHeight + waterColumnAbove + slopeSin × slopeHeight. The baseline term
	 *  keeps trough forces non-zero; the slope term lets a tilted wave face contribute even when
	 *  the actor isn't submerged. Scaled by waterContactGate, so it is 0 for an airborne surface. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly)
	float effectiveWaterHeight = 0.0f;

	/** SIGNED distance from this actor to the wave surface, cm: positive = the actor is ABOVE the
	 *  surface (clear of the water), negative = submerged. The information waterColumnAbove throws
	 *  away by clamping at 0, and the only signal that separates "just breaking the surface" from
	 *  "in the air". Refreshed each tick in setup(). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly)
	float surfaceClearance = 0.0f;

	/** Is this surface touching water at all: 1 while submerged, fading to airborneForceScale once
	 *  clear by airborneFadeDistance. Multiplies effectiveWaterHeight (and so every force that
	 *  scales with it) plus the fin lift, which has no other wetting gate.
	 *  See specs/airborne-force-gating.md. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly)
	float waterContactGate = 1.0f;

	/** PER-ACTOR bottom-skin wetting: smoothstep(waterColumnAbove / wettedTransitionDistance), 0..1.
	 *  Unlike SharedCalculations::amountWetted (per-SC, last-writer-wins among 4 buoyancy corners → can't
	 *  carry left/right info) and amountUnderWater (per-SC, ~1m gradual ramp for smooth buoyancy), this is
	 *  computed at THIS actor's own (X,Y,Z), so it differs between the submerged and raised rail when the
	 *  board rolls — giving the bottom lift/thrust a real submerged-side asymmetry (roll righting). This is
	 *  the signal FD force terms should use for "is my surface wetted". See specs/per-actor-wetting.md. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly)
	float actorWetted = 0.0f;

	/** Per-actor water velocity (world), sampled at THIS actor's position each tick — same call
	 *  pattern/transform as the SC sampling, so it differs from SC only by WHERE it samples. The
	 *  contact-like wave forces (flow drags, penetration wall) blend toward it via
	 *  PerActorWaterVelocityBlend, so e.g. the nose feels the lip's fast core instead of the
	 *  board-midline fringe. See specs/per-actor-water-velocity.md. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly)
	FVector actorWaterVelocity = FVector::ZeroVector;

	/** Previous-tick actorWaterVelocity for the per-actor temporal smoothing (same
	 *  velocitySmoothingFactor as the SC path — striping artifacts must not return at actor
	 *  granularity). */
	FVector previousActorWaterVelocity = FVector::ZeroVector;

	/** 0..1 blend of the contact forces' water velocity: 0 = board-wide SC average (legacy),
	 *  1 = this actor's own sample. Runtime scratch from Tuning->PerActorWaterVelocityBlend
	 *  (kept in sync with that default). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float perActorWaterVelocityBlend = 1.0f;

	/** Synthesized lip impact (nose actor only) — runtime scratch from the Tuning LipImpact* knobs;
	 *  computed in calcDragForce's VE_Nose case, applied pitch-kept in applyDragAsImpulse.
	 *  See specs/lip-impact.md. */
	float lipImpactCoefficient = 1.0f;
	float lipImpactBandOffset = 150.0f;
	float lipImpactMinJetSpeed = 350.0f;
	float lipImpactMinSlopeSin = 0.30f;
	float lipImpactCrestRange = 300.0f;
	float lipImpactMaxForce = 1000000.0f;

	/** The synthesized lip-impact force stashed by the nose actor's calcDragForce pass. */
	FVector PendingLipImpactForce = FVector::ZeroVector;

	/** Sum of every force (world, force units — pre-impulse) this actor applied to the board this
	 *  tick, accumulated in the apply*AsImpulse funnels and zeroed at the top of setup(). Read-only
	 *  consumer: ASprayController (spray is the Newton-3 reaction to this). Purely diagnostic —
	 *  nothing in the force pipeline reads it back. See specs/board-spray-particles.md. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Spray")
	FVector appliedForceThisTick = FVector::ZeroVector;

	/** The non-sheet-producing portion of appliedForceThisTick, subtracted out by spray consumers:
	 *  the waveSlopeGravity supplement (synthetic gravity) and the wave-mass family (flow drag,
	 *  penetration, lip impact) — the latter is the wave's bulk water CARRYING the board, momentum
	 *  exchange with slow-moving mass, not the board throwing a sheet off a rail. Keying spray on
	 *  those made the slow wave-catch out-spray a hard carve (observed); excluding them leaves the
	 *  v² contact forces (drag, lift, thrust, rail lift), so fast carves splash hardest. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Spray")
	FVector appliedNonSprayForceThisTick = FVector::ZeroVector;

	/** appliedForceThisTick minus the non-sheet-producing contributions (see above). */
	FVector getSprayDriveForce() const { return appliedForceThisTick - appliedNonSprayForceThisTick; }

	/** Transition band (cm) for the per-actor amountWetted smoothstep. Copied from
	 *  Tuning->wettedTransitionDistance. Must be wide enough that the bottom actors' depth doesn't peg it
	 *  at 1.0, but narrower than amountUnderWater's ~1m ramp so it stays sharp. Bottom actors measured
	 *  resting ~37-50cm deep, so 60cm un-pegs them; overwritten by Tuning->wettedTransitionDistance. */
	float wettedTransitionDistance = 60.0f;

	/** Compensates the force-magnitude weakening introduced by per-actor actorWetted. The old per-SC
	 *  amountWetted gate was pegged at 1.0, so every actorWetted-gated force (bottom Bernoulli lift, hydrofoil
	 *  up-thrust / forward thrust, yaw hydrofoil, slope thrust) effectively saw gate=1.0. actorWetted now sits
	 *  at ~0.82 mean at rest (measured), so those forces dropped ~18%. This multiplier is applied uniformly
	 *  at the actorWetted force-gate sites (NOT to the raw actorWetted used for L/R asymmetry logging), so it
	 *  restores the pre-per-actor-wetting magnitude while preserving the new submerged-side asymmetry (the L/R
	 *  ratio is unchanged; only the common scale is lifted). 1/0.82 ~= 1.22. Copied from
	 *  Tuning->wettedForceCompensation. See specs/per-actor-wetting.md. */
	float wettedForceCompensation = 1.0f;

protected:
	// Called when the game starts or when spawned
	virtual void BeginPlay() override;

public:	
	// Called every frame
	virtual void Tick(float DeltaTime) override;

	UFUNCTION(BlueprintCallable, Category="Surfing")
	void setup();

	/** Frame-throttle gate for debug logs. True only on every (debugLogEveryNthTick)th call to setup(). */
	bool shouldDebugLog() const { return debugLogEveryNthTick <= 1 || (debugFrameCounter % debugLogEveryNthTick == 0); }

private:
	int32 debugFrameCounter = 0;

	/** Cached pointer to USurfTuningSubsystem, resolved once in BeginPlay.
	 *  At the top of each setup() the shared-by-design coefficient UPROPERTYs
	 *  are overwritten from this pointer, and the four BP-callable functions
	 *  (calcDragForce/calcLiftForce/calcThrustForce + Buoyancy::calculateBuoyancyForce)
	 *  ignore their incoming arg values when this is non-null. See
	 *  specs/runtime-tuning.md Phase 2/3. */
	UPROPERTY(Transient)
	USurfTuningSubsystem* Tuning = nullptr;

	void RefreshFromTuningSubsystem();

public:

	bool waterIsFlowingForwards();
	bool waterIsFlowingTowardsSurface();
	double unwindRadians(double angle);
	float calcCosAngleOfAttack(FVector relativeWaterVelocity, FVector normal, FVector compareTo);
	bool roughlyEqual(float a, float b);


	// Coefficients for the calc*/apply* force functions live SOLELY in USurfTuningSubsystem
	// (single source of truth; falls back to the subsystem CDO defaults if no game instance).
	// The old BP-passed-argument path (FluidDynamicsConstants) is removed.
	UFUNCTION(BlueprintCallable, Category="Surfing")
	FVector calcDragForce();

	UFUNCTION(BlueprintCallable, Category="Surfing")
	FVector calcLiftForce();

	/** Apply a force as an impulse (force × DeltaTime) on the surfboard mesh, at this actor's
	 *  world location. NaN-guarded; no-ops if surfboardMesh is null or the force is near zero.
	 *  Async-physics safe: AddImpulseAtLocation takes an impulse (N·s), not a continuous force. */
	UFUNCTION(BlueprintCallable, Category="Surfing")
	void applyForceAsImpulse(FVector force, float DeltaTime);

	/** Apply the wave-mass flow drag at getWaveMassApplyPoint() (the CoM longitudinal axis) instead of the
	 *  actor, so it pushes/stops without rolling the board. See specs/wave-mass-drag-torque-decoupling.md. */
	void applyWaveMassForceAsImpulse(FVector force, float DeltaTime, float yawDecouple);

	/** World application point for the wave-mass flow drag: the actor position projected onto the board
	 *  forward axis through the CoM, lerped by waveMassRollDecouple. Removes the roll moment arm. */
	FVector getWaveMassApplyPoint() const;

	/** Torque the wave-mass flow `force` actually imparts after decoupling: (getWaveMassApplyPoint()-CoM)×force
	 *  (roll already removed by the point) with the board.up (yaw) component scaled down by
	 *  waveMassFlowYawDecouple. Used for BOTH the applied angular impulse and the torque-budget registration
	 *  so the diagnostic reflects what is really applied. */
	FVector getWaveMassKeptTorque(const FVector& force, float yawDecouple) const;

	/** Apply railLift at getComHeightApplyPoint() (CoM height) instead of the actor, so its sideways force
	 *  can't keel-roll the board. See specs/wave-mass-drag-torque-decoupling.md. */
	void applyRailLiftAsImpulse(FVector force, float DeltaTime);

	/** World point at this actor's (X,Y) with Z lerped toward the CoM by railLiftRollDecouple — zeroes the
	 *  vertical moment arm so a sideways force here makes no roll, keeping its lateral/forward arms. */
	FVector getComHeightApplyPoint() const;

	/** Compute drag force via calcDragForce and apply it as an impulse. Single C++ call from BP
	 *  Tick replaces the old ApplyDrag/AddForce/AllForces/ForceQueue chain. */
	UFUNCTION(BlueprintCallable, Category="Surfing")
	void applyDragAsImpulse(float DeltaTime);

	/** Compute lift force via calcLiftForce and apply it as an impulse. Single C++ call from BP
	 *  Tick replaces the old ApplyLift/AddForce/AllForces/ForceQueue chain. */
	UFUNCTION(BlueprintCallable, Category="Surfing")
	void applyLiftAsImpulse(float DeltaTime);

	/** Compute thrust force via calcThrustForce and apply it as an impulse. Models angle-of-attack
	 *  hydrodynamic lift on the board's bottom (perpendicular to the board, upward). */
	UFUNCTION(BlueprintCallable, Category="Surfing")
	void applyThrustAsImpulse(float DeltaTime, FColor debugColor);
	float calcRailLiftForceAmount(float liftMagnitude, float cosYawAngle);
	float apxLiftCoefficient(float cosAngleOfAttack);

	/** FR1 of specs/yaw-hydrofoil-flow-direction.md: 1 while the board MOVES with the wave's travel
	 *  or along the crest (a turn keeps it), fading to 0 as it moves seaward into the wave. Below
	 *  ~150 cm/s the nose direction stands in for the velocity. Multiplied into the yaw-hydrofoil
	 *  forward drive only. 1 when disabled or without an SC. */
	float calcWithWaveGate() const;

	UFUNCTION(BlueprintCallable, Category="Surfing")
	FVector calcThrustForce(FColor debugColor);

	UFUNCTION(BlueprintCallable, Category = "Surfing")
	TArray<FVector> getEnquedForces();

	UFUNCTION(BlueprintCallable, Category = "Surfing")
	void enqueForce(FVector forceToEnque, int number_of_chunks);

private:
	std::deque<std::deque<FVector>> ForceQueue;
};
