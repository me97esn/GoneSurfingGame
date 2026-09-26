// Phase 1 of specs/runtime-tuning.md: damping coefficients only.
// Single source of truth for tunable damping values; mutable at runtime by an
// in-game UMG panel and persisted to Saved/TuningOverrides.json.
//
// Read path on hot paths is a direct UPROPERTY access through a cached pointer
// (see ASurfboardUtils::Tuning), so no string ops / map lookups per tick.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Containers/Ticker.h"
#include "SurfTuningSubsystem.generated.h"

UCLASS()
class GONESURFING_API USurfTuningSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	// --- Phase 1: damping (migrated from ASurfboardUtils). Defaults captured
	// from the live BP-overridden values at 2026-06-07 13:06 (see
	// specs/runtime-tuning.md). ---

	// NOTE: damping defaults bumped ~2x-effective (D' = 1 - (1-D)^2) when the engine damping became
	// frame-rate-independent (see specs/framerate-independent-angular-damping.md). The old per-frame damping
	// over-damped whenever the phone ran above 60fps (it hits ~120-140fps in light moments like the pop-up),
	// which was artificially holding the board calm; the fix normalized that to 60fps, leaving the board too
	// loose (yawing to the far side during pop-up). These values restore ~the old high-fps feel and are now
	// consistent across devices. Starting point — fine-tune live in the HUD (filter "damp").
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Damping")
	float SurfboardSidewaysDamping = 0.0975f;

	// What fraction of SurfboardSidewaysDamping still applies with NO water contact.
	//
	// Sideways (board-local Y) was the ONLY local linear axis with no contact gate: Z is scaled by
	// avgAmountUnderWater and X by (1 - avgAmountPlaning), Y by nothing. So it went on braking a
	// board that had left the water. Measured on the rails fixture over
	// phone-2026-09-22-20-26-02: the board loses 266 cm/s of HORIZONTAL speed across 0.45 s during
	// which both contact gates read 0.000 - airborne, where a ballistic board's horizontal velocity
	// must be constant. Zeroing the damping makes it flat (850 -> 883), which is the proof.
	//
	// It matters more than it sounds because the axis is board-LOCAL: mid-slide "sideways" contains
	// most of the DOWN-THE-LINE velocity, so what it removed was exactly the speed the board should
	// have kept. On that ride min down-line speed went 4 -> 255 cm/s with it off.
	//
	// 0 = the fix (no sideways damping without water to push against). 1 = the old ungated
	// behaviour, for A/B. Intermediate values keep a token amount, e.g. for aerodynamic drag.
	// This is NOT the damping-vs-carve-grip trade - that one is a feel call on the owner's sliders;
	// see specs/carve-grip-via-redirect.md "Step 4 revisited".
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Damping", meta=(ClampMin="0.0", ClampMax="1.0"))
	float SidewaysDampingAirScale = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Damping")
	float SurfboardForwardsDamping = 0.03f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Damping")
	float SurfboardVerticalDamping = 0.04f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Damping")
	float WorldDownwardsDamping = 0.004f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Damping")
	float WorldUpwardsDamping = 0.078f;

	// X/Y at 0.4/0.4 paired with WeightTorque 4000 (in-game tuned 2026-07-27; supersedes the
	// 0.2/0.2 + WeightTorque-2000 pairing of 07-22 — more pitch/roll damping with proportionally
	// more weight-shift authority to punch through it).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Damping")
	float AngularDampingX = 0.4f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Damping")
	float AngularDampingY = 0.4f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Damping")
	// Bumped 0.04 -> 0.08 (2x) (2026-07-20). After the pop-up's SURFING-STRAIGHT step centers weight, the board
	// kept yawing ~31 deg into the wave: the pop-up turn builds a high yaw RATE (~110-130 deg/s), the driving
	// torque collapses to ~0 within ~0.15s of centering (bank rights), and the leftover angular momentum coasts
	// because yaw damping was too weak to arrest it (see specs/framerate-independent-angular-damping.md re: the
	// dt-aware decay this feeds). A/B (pop-up + turn-hard-into-the-wave autopilots) at the 0.10 upper test point
	// halved the un-driven coast (post-center swing 30.9 -> 15.6 deg) with ZERO cost to the DRIVEN hard turn
	// (90->-42 deg identical at every frame) -- the strong carve torque overwhelms the extra damping; only the
	// torque-free coast is killed. Shipping 0.08 as a slightly more conservative default; headroom to go higher.
	// Took that headroom: 0.08 -> 0.15 (2026-08-23), past the 0.10 A/B test point.
	float AngularDampingZ = 0.15f;

	// Surface-relative pitch damping (see specs/surface-relative-pitch-damping.md). Extra pitch-axis
	// damping when the board rotates AWAY from alignment with the local water surface; reduction when
	// rotating TOWARD it. Replaces the old AngularDampingYNoseDownExtra (which only covered nose-down).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Damping")
	float AngularDampingYAwayExtra = 0.8f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Damping")
	float AngularDampingYTowardReduction = 0.10f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Damping")
	float YawDampingTiltBackInfluence = 0.9f;

	// Phone tilt angle (deg) that produces full weight deflection. Lower = more responsive
	// (less physical tilt needed for a full weight shift). The on-device pawn instance had
	// this hand-set to 45, which averaged only ~39% commanded deflection for typical play and
	// made the board feel sluggish to turn; 18 is the tuned default. Read live via the HUD by
	// ASurfboardPawn::GetEffectiveTilt*ForFull(), so it overrides any stale per-instance value.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Tilt")
	float TiltPitchDegreesForFullDeflection = 25.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Tilt")
	float TiltRollDegreesForFullDeflection = 25.0f;

	// --- The start-screen tutorial's drawn board (specs/tutorial-live-feedback.md) ---
	// It previews the real board's response, so it has to move like the real board: the physical
	// one has mass and the fork's angular damping, tuned so it deliberately does not pitch or roll
	// quickly. Chasing the commanded lean instantly taught a snappier board than the one ridden.
	//
	// These are the drawn board's own dynamics, not the ride's — nothing here touches physics. Set
	// them by riding, then opening the instructions and matching what you see. Here rather than on
	// a CVar precisely so that can be done with a slider, live, until it looks right.

	// How quickly the drawn board swings to a commanded lean — the natural frequency of the spring
	// that pulls it there, in Hz. Higher = snappier, lower = heavier. This is the only knob that
	// makes the response quicker: TutorialBoardDamping above 1 (it is 1.5) removes the wobble but
	// also *slows* the approach, so if the board feels sluggish it is this that wants raising.
	//
	// Roughly, at damping 1.5: the board covers about two thirds of the distance to a new lean in
	// 2.6 / (2*pi*Hz) seconds — so ~0.42s at the tuned 1.0.
	//
	// 1.0 was dialled in on device (2026-08-21), against the real board.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Tutorial")
	float TutorialBoardHz = 1.0f;

	// Damping ratio of that swing. 1 = arrives without overshoot; below that it overshoots and
	// settles, above it approaches without ever crossing. 1.5 was dialled in on device
	// (2026-08-21): overdamped, so the drawn board leans over and stays there rather than wobbling
	// into place, which is how the real one behaves under its angular damping.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Tutorial")
	float TutorialBoardDamping = 1.5f;

	// How hard one pump launches the drawn board, in travel-units/s^2 at full pump input. Large
	// because a pump is a pulse, not a hold: only the loading phase registers, so one committed
	// pump is about a third of a second of input. 50 was dialled in on device (2026-08-21) — a
	// single pump then carries the board off the edge in about half a second.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Tutorial")
	float TutorialPumpLaunch = 50.0f;

	// How fast that launch bleeds off with no further pumping, in Hz. Lower = glides further, so
	// pumps accumulate; higher = each pump is its own short burst.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Tutorial")
	float TutorialPumpDrag = 0.7f;

	// --- Gyro yaw fusion for the sideways axis (specs/tilt-yaw-fusion.md) ---
	// Gravity cannot see rotation about the gravity axis, so the sideways gesture loses
	// sensitivity as cos(theta) as the phone approaches vertical (and inverts past it).
	// A gyro-integrated yaw term covers exactly that null space.

	// Degrees of commanded sideways lean per degree of phone yaw. Magnitude only — the
	// direction lives in TiltYawInvert. 0 disables fusion entirely (exact pre-fix
	// behaviour). Kept positive so SurfTuningHUD's slider, which spans 0..4x the captured
	// default, covers a useful range.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Tilt")
	float TiltYawGain = 1.0f;

	// Reverses the yaw direction when >= 0.5. A float rather than a bool, and defaulting to
	// zero, on purpose: SurfTuningHUD builds a row per FFloatProperty, and its zero-default
	// path gives a raw 0..1 slider — so this is flippable from the on-device gear panel
	// mid-session. A signed gain could not be: that slider spans 0..4x default and can
	// never cross zero. The correct value is a coin flip until checked on hardware; see
	// specs/tilt-yaw-fusion.md TC1.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Tilt")
	float TiltYawInvert = 0.0f;

	// Time constant for the yaw integrator's leak back to neutral. Bounds gyro drift and
	// discards slow whole-body rotation. Must outlast a carve (~1-2 s).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Tilt")
	float TiltYawLeakSeconds = 2.0f;

	// Window after tilt calibration over which the gyro's zero-rate bias is averaged and
	// subtracted. The yaw channel contributes nothing during this window.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Tilt")
	float TiltYawBiasSampleSeconds = 1.0f;

	// Angular-impulse magnitude behind the weight-shift roll/pitch (AWeightDistribution).
	// Higher = more turn authority per unit of commanded weight shift. Read live via
	// AWeightDistribution::calculateWeightTorque, so it overrides the actor UPROPERTY default.
	// 4000 (in-game tuned 2026-07-27, paired with AngularDampingX/Y 0.4).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Weight")
	float WeightTorqueMagnitude = 4000.0f;

	// Ceiling on the player's commanded forward weight (amountInFront: 0 = all on the tail,
	// 0.5 = centred, 1 = all on the nose). Player input only - autopilots and the pop-up intro
	// write amountInFront directly and are not capped.
	//
	// Why: it is easy to hold the phone nose-down, and a nose-heavy board stops answering the roll
	// axis at all, which reads to the player as the game ignoring input rather than as physics. A
	// ceiling below 0.5 keeps a little weight on the tail at all times, so the board always turns
	// *some* amount - slower than a properly tail-weighted one, but never nothing.
	//
	// The clamp is applied to the mapped value, so below the ceiling the response stays exactly
	// proportional to tilt; only the part of the forward travel that would push past it is lost.
	// Above 0.5 this behaves as a plain cap on forward travel. Shipped at 0.45 until 2026-09-14,
	// where the forward half of the tilt range was spent and level-phone already sat marginally
	// tail-biased; raised to 0.6 to hand some forward authority back, then settled at 0.5 on
	// 2026-09-15 (ridden as the shortboard overlay value): centred is as far forward as the player
	// can go, so the board never loses the roll axis. 1.0 = no limit.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Weight")
	float WeightMaxInFront = 0.5f;

	// Which scheme drives weight on the phone: 1 = the on-screen virtual joystick (default),
	// 0 = phone tilt. Both stay in the build so they can be A/B'd on device; tilt is kept as a
	// player option rather than deleted. See specs/pump-button-and-virtual-stick.md.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Weight")
	float WeightInputSource = 1.0f;

	// Thumb travel from the stick's floating origin that produces full deflection.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Weight")
	float StickFullDeflectionPx = 90.0f;

	// Fraction of full deflection ignored around the origin.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Weight")
	float StickDeadzoneFrac = 0.08f;

	// The fore/aft axis self-centres while the thumb is still down, over this time constant: a push
	// forward or back is a nudge that decays, not a trim that is held, so a thumb resting slightly
	// off-centre cannot silently hold a trim the player has forgotten about. Sideways deliberately
	// does not self-centre — holding a lean has to hold the turn. 0 = hold the trim on both axes.
	//
	// Default 0 since 2026-09-14: with the latch holding both axes, a fore/aft that crept back
	// while held snapped the nose back down the moment the thumb stopped moving, which read as
	// the stick ignoring the player. The forward cap (WeightMaxInFront) is what actually guards
	// against a forgotten nose-down trim now.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Weight")
	float StickForeAftReturnSeconds = 0.0f;

	// Joystick latch: 1 = the command stays where the thumb left it on BOTH axes, so a turn
	// carries on and a lifted nose stays lifted after release; a tap on the stick without a drag
	// re-centres it, and touching down onto a held command picks it up rather than dropping it.
	// 0 = everything auto-centres on release (the original behaviour, AC1 in the spec). Sideways-
	// only latching was the first cut (2026-09-14) and felt like two instruments in one ring; the
	// player asked for fore/aft to hold the same way. An A/B knob: if testers over-lean with it
	// on, the answer is a heading-command stick, not a tweak here - see the spec.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Weight")
	float StickLatch = 1.0f;

	// Swap the joystick and the pump button for left-handed players.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Weight")
	float MirrorTouchControls = 0.0f;

	// Drawn size of the two on-screen controls, as a fraction of SCREEN HEIGHT (the short edge in
	// landscape, so a control keeps its physical size across aspect ratios). These are radii, so the
	// visible circle is twice this: 0.16 is about a third of the screen's height across, which on a
	// typical phone held sideways is roughly 2 cm - a thumb target, not an icon.
	//
	// Tunable because thumb-size decisions cannot be made on a monitor: the first pass at 0.12
	// measured about 1 cm on device and was unusable.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Weight")
	float TouchStickRadiusFrac = 0.25f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Weight")
	float TouchPumpRadiusFrac = 0.25f;

	// Height of both controls' centres, as a fraction down the screen. 0.5 is the centreline, which
	// is where thumbs actually sit on a phone held in landscape - they wrap around the edges near
	// the middle, not down at the bottom corners a desktop layout would suggest.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Weight")
	float TouchControlsYFrac = 0.5f;

	// The touch controls' captions ("PRESS & RELEASE / TO SPEED UP" under the pump button,
	// "SIDEWAYS TO TURN / BACK TO LIFT THE NOSE" under the stick). 0 = none. For the pump button
	// the value also picks the placement: 1 = inside the disc under the glyph, 2 = below the disc,
	// 3 = centred over a large faded glyph; the stick's caption is always below its ring. A tunable
	// rather than a CVar so it can be flipped on the phone mid-ride, which is the only place the
	// choice can be made. 2 keeps the figure unobstructed - a surfer reads the pose, everyone else
	// reads the caption - and is the working default.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Weight")
	float TouchControlsHint = 2.0f;

	// Pump button look: 1 = ghost (the stick's translucent-white tier, glyph in white over a soft
	// shadow; both controls weigh the same and only the glyph tells them apart) - the default,
	// chosen 2026-09-14 for looking like one family with the stick. 0 = coral (SURF AGAIN's
	// colours), which made the pump look MORE important than the stick, the main control.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Weight")
	float TouchPumpStyle = 1.0f;

	// How the controls are drawn before the player has them: on the hub, and through the intro up
	// to the handoff (specs/two-screen-navigation.md FR1a). Two numbers, because one was not a
	// signal: fading the whole thing evenly to 0.5 left a ghost control looking like a ghost
	// control ("at least to me it isn't obvious", owner 2026-09-22). The INSTRUMENT (rings, knob,
	// glyph) drops to TouchControlsRestArt so it reads as a drawing of itself; the CAPTIONS hold at
	// TouchControlsRestText, because on the hub the words are the whole reason the controls are
	// shown. Coming up to 1.0 at the handoff is then a real change of state, with the words steady
	// across it. Tunables rather than constants for the same reason TouchControlsHint is one: how
	// obvious "not yet" reads is a judgement made on the glass, over moving water, not on a monitor.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Weight", meta=(ClampMin="0.0", ClampMax="1.0"))
	float TouchControlsRestArt = 0.22f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Weight", meta=(ClampMin="0.0", ClampMax="1.0"))
	float TouchControlsRestText = 0.85f;

	// ---- Hold-to-pump (the pump button; specs/pump-button-and-virtual-stick.md) ----

	// A pump is a crouch and a rise, not a rhythm. Holding the button crouches the surfer, storing
	// nothing by itself; RELEASING extends the legs and drives the board down. The skill is the
	// moment of release - into a turn, at the bottom of the face - which is a decision the player
	// makes rather than a cadence the game imposes.

	// Seconds of holding to reach a full crouch. Releasing earlier gives a proportionally weaker
	// pump, which is what makes a half-pump expressible at all.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Pump")
	float PumpChargeSeconds = 0.5f;

	// Duration of the extension after release. The downward impulse is shaped across exactly this
	// window, so it is also how long the pump's force lasts.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Pump")
	float PumpReleaseSeconds = 0.35f;

	// Seconds the rider takes to settle from the top of the extension back into the surf stance.
	// Purely visual - it starts AFTER the extension, so it neither lengthens the force window nor
	// delays the next pump. Without it the return is a snap: the rise clip and the stance blend
	// both stand down on the same tick the extension completes, so the pose the outer blend is
	// leaving has already collapsed to the stance. See USurferAnimInstance's pump settle.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Pump")
	float PumpRecoverySeconds = 0.45f;

	// Charge below this is treated as a twitch and produces nothing, so resting a thumb on the
	// button and lifting it does not fire a pump.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Pump")
	float PumpMinCharge = 0.15f;

	// Sideways steering authority retained while pumping. 1 = untouched, which is the default and
	// makes this knob inert: pumping while rolling the board onto a rail is not just possible, it is
	// a preferred way to pump - you turn onto the rail while driving your weight down through the
	// board. Steering and pumping are one movement in the water, so the game must not separate them.
	// Kept as a lever rather than deleted so the coupling can be A/B'd, but it should stay at 1.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Pump")
	float PumpLateralAttenuation = 1.0f;

	// Seconds over which the lateral attenuation ramps in and out, so entering and leaving a pump
	// is not a step.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Pump")
	float PumpLateralRampSeconds = 0.2f;

	// Planing redirect (rocker lift): rotation rate (rad/s) at which the board's world velocity is bent
	// toward the wave up-slope when the nose is submerged, so it rides up the face instead of plowing
	// through it. Gated by nose amountUnderWater; auto-disabled on flat water (no slope). 0 = off.
	// Sweet spot ~2.0: minimum that reliably stops the glide-through (3/3 ride in testing) while the board
	// still carves the sharp turn; at >=3 it over-grips and stops turning. See specs/planing-redirect.md.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Damping")
	float PlaningRedirectMaxAngle = 2.0f;

	// One-sided planing redirect: scale on the redirect rate when the board's velocity already points OUT
	// of the wave face. The symmetric redirect cancels buoyancy's outward escape velocity for a buried
	// nose (it drives the face-normal velocity component to 0 from BOTH sides), which holds the nose down
	// and escorts the board through the crest. 1 = legacy symmetric; 0 = never redirect outward motion.
	// See specs/pitch-righting-and-redirect-escape.md.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Damping")
	float PlaningRedirectOutwardScale = 1.0f;

	// Crest-proximity fade for the planing redirect (cm; 0 = off). The redirect's target ("up the face")
	// is valid on the face but becomes an escort over the lip near the crest — during the bottom-turn
	// punch-through the board rides up PARALLEL to the face, so only removing the up-face steering helps
	// (surface-alignment righting measured a negative result). Full strength when the nose is >= this many
	// cm on the front-face side of the crest (front SC signedDistanceToCrest), fading smoothly to 0 at the
	// crest and staying 0 behind it. 600 = player-validated ("much better", 2026-07-22): kills the
	// bottom-turn punch-through (maxUW 0.97→0.80, climb escort +65→+17cm) and the turn carves deeper.
	// NOTE: on flat water signedDistanceToCrest reads 0 -> gate 0 (grip/redirect off there; redirect was
	// already slope-disabled, grip no-ops below 1 cm/s). See specs/pitch-righting-and-redirect-escape.md.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Damping")
	float PlaningRedirectCrestFade = 600.0f;

	// Same crest-proximity fade for the carve grip (cm; 0 = off) — near the lip the grip steers momentum
	// toward the into-wave nose heading, the other half of the escort. 600 = player-validated 2026-07-22.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Damping")
	float CarveGripCrestFade = 600.0f;

	// Which contact signal gates the carve grip: 0 = the NOSE's amountUnderWater (front SC, the
	// original), 1 = the board-wide contact gate the pitch, wave-normal and sideways damping already
	// share (smoothstep over the mean of the front and back SC submersion). Blended, so this is also
	// the A/B knob.
	//
	// The nose gate was never physically motivated - this spec's only argument for it was
	// "reuse/generalize the same machinery ... instantiated twice" (owner, 2026-09-23: "the carving
	// redirect by design only works when the nose is wetted. This means that the board can't do a
	// top turn, since during a top turn the nose is lifted above the wave at a big part of the
	// manuever"). Grip comes from the fins and the loaded rail, both aft of the nose. Measured on
	// phone-2026-09-22-20-26-02 at trace t=3.91: nose 0.000, tail 0.700 - the nose gate says 0.000
	// where the board gate says 0.683, and both still say 0.000 through t=3.99-4.41 where the whole
	// board is clear of the water, which is the case that SHOULD kill the grip.
	//
	// Deliberately saturating rather than a flat mean: the 4-point buoyancy average reads 0.177 at
	// that same tick because it dilutes by the dry nose, and fins do not care about the nose.
	//
	// The PLANING redirect keeps the nose signal - "the nose climbing the face" is a real rationale
	// for that one. See specs/carve-grip-via-redirect.md ("Amendment: the gate").
	//
	// DEFAULT 1 since 2026-09-23. It briefly shipped at 0 on a whole-ride regression that turned out
	// to be measuring the wrong water: 49% of that ride is whitewater and 39% flat, only 13% green
	// face, and split by zone the knob costs nothing on the face (slip mean 12.0 -> 11.4) while all
	// the slip it "added" was off the wave. Owner picked BOTH knobs on from the videos.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Damping", meta=(ClampMin="0.0", ClampMax="1.0"))
	float CarveGripContactBlend = 1.0f;

	// Whether a loaded rail lifts the crest fade in BOTH yaw directions. 0 = only a yaw TOWARD the
	// wave un-fades (the original), 1 = the yaw RATE does, whichever way the nose is turning.
	//
	// The original is asymmetric in a way that structurally excludes the second half of a turn:
	// measured on the same ride, yawTowardWave is 0.561 during the snap up the face and then 0.000
	// for the entire rest of the manoeuvre, because a top turn yaws back AWAY from the wave by
	// definition. So gripCrestGate sat at 0.00-0.07 from t=3.66 to t=4.24 and the grip was off no
	// matter what the contact gate said.
	//
	// Keying on |yaw rate| keeps the fade's actual job intact: an unloaded board drifting over the
	// lip has a low yaw rate and is still not escorted (which is the player-validated bottom-turn
	// punch-through fix, 600/600, 2026-07-22), while a board committed to a turn holds its line.
	//
	// DEFAULT 1 since 2026-09-23, together with the blend above - the pair is what matters, since
	// they are two multiplicands of one gate and neither moves the green-face numbers alone
	// (11.4 / 11.9 vs 12.0 mean slip). Together: mean 12.0 -> 9.7, p90 31.7 -> 21.1 on the face, for
	// 2.4% less speed (625 -> 610). Player-validated on the top turn: "it redirected the nose most
	// realistically".
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Damping", meta=(ClampMin="0.0", ClampMax="1.0"))
	float CarveGripTurnUnfadeSymmetric = 1.0f;

	// Surface-relative pitch RIGHTING servo rate (1/s): actively drives the board's pitch rate toward
	// alignment with the local water surface (wTarget = -rate × waveRelativePitchSin) — the attitude
	// analog of the velocity redirects, and the only escape route they can't cancel (they never touch
	// angular velocity). The pitch DAMPING (AngularDampingY*) only resists rotation; this initiates the
	// nose-up. Contact-gated in the SurfboardUtils feed. 0 = off (inert until tuned).
	// See specs/pitch-righting-and-redirect-escape.md.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Damping")
	float PitchRightingRate = 0.0f;

	// Carve grip (rocker-grip via the engine velocity redirect): rotation rate (rad/s) at which the board's
	// HORIZONTAL velocity is bent toward its horizontal heading (nose FD forwards), gated by nose
	// amountUnderWater. Velocity-follows-heading anti-slip grip that also aids the carve when rolled. The
	// yaw-plane analog of PlaningRedirectMaxAngle. 0 = off (Phase 1 default). See specs/carve-grip-via-redirect.md.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Damping")
	float CarveGripRate = 4.0f;

	// Yaw-rate ceiling that falls with speed, on yaw TOWARD the wave only (the lip-snap spin-out fix,
	// specs/lip-snap-spinout-and-air.md T1). A full lean at 750 cm/s commands ~105 deg/s of yaw into the
	// face; no redirect can bend 750 cm/s of momentum through that against the water flowing the other
	// way, so slip runs to 55-99 deg, the wave-normal damping removes the into-wave momentum and the
	// sideways damping the rest - every force along the nose positive, speed 758 -> 200 in 0.6 s.
	// Lowering lateralTurnCoefficient also softens slow turns (M4), so instead the ENGINE clamps the local
	// yaw rate (p.Chaos.Solver.MaxAngularVelocityZPos/Neg, hard ceilings like the linear MaxVelocity* ones)
	// on the sign that turns the nose toward resolvedWaveBackDirection, at
	//     cap(v) = YawRateCapMax * min(1, YawRateCapSpeedKnee / v)        [rad/s]
	// i.e. constant below the knee, and above it bounded lateral acceleration v*w <= Max*Knee. Slow turns
	// (~20-40 deg/s) never touch the cap. Yaw AWAY from the wave is never capped: the hybrid's recorded
	// cutback (phone-2026-09-17-10-48-07, 180 deg/s at 750 cm/s) rides out clean because the momentum it
	// keeps moves WITH the water, and a symmetric cap measured there stalled the whole ride (490 -> 322).
	// The cap applies to yaw from every source (turn torque, yaw hydrofoil, wave torques).
	// Default 2.5 rad/s x knee 200 = 500 cm/s^2 of lateral acceleration at speed (0.5 g): measured on the
	// spin-out trace at t=5-8, knee 150/200/250/300/400 -> mean speed 456/431/400/374/321 against a
	// 34/43/53 deg turn delivered at the crest for 200/250/300; 400 still spins out. 200 chosen headless,
	// 250 is the "more turn" alternative for the device pass. 0 = off (no clamp; the 2026-09-18 behaviour).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Damping")
	float YawRateCapMax = 2.5f;

	// Speed (cm/s) above which the yaw-rate cap falls as Knee/v. Below it the cap is YawRateCapMax flat.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Damping")
	float YawRateCapSpeedKnee = 200.0f;

	// A3, control degrades in the whitewater (specs/broken-wave-no-consequences.md): while brokenGeo
	// > 0 the yaw-rate ceiling is cap(v) x lerp(1, this, brokenGeo) on BOTH signs - the away-from-wave
	// sign, unlimited on the face (the cutback, lip-snap M7), is fed the same scaled cap only there.
	// The redirect-family way to take authority away: the lateral-turn torque and the commanded-lean
	// carve are untouched. Sized on 14-38-37's own board_avz (M15): the foam run-up yaws 91-110 deg/s
	// at 540-715 cm/s, where the face cap is 0.83 rad/s = 48 deg/s; 0.5 here holds it to ~24 deg/s at
	// speed and 72 deg/s under the knee, so paddling-speed turns in the foam are barely touched.
	// 1 = off (no control loss in the foam).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Damping", meta=(ClampMin="0.0", ClampMax="1.0"))
	float BrokenYawCapScale = 0.5f;

	// Lift the carve-grip crest fade while the board is yawing TOWARD the wave (rad/s of toward-wave yaw
	// at which the fade is fully lifted; the lift ramps linearly from 0). The fade exists so a board
	// riding steady along the lip with the nose a few degrees over the back slides shoreward instead of
	// following its nose over (hybrid crest ride, phone-2026-09-17-10-48-07 t=1.5-2.3: grip un-faded
	// there stalls the ride 490 -> 215). A snap at the lip is the other case: the rail is loaded in a
	// turn and should grip, and with the fade the grip is 7-16 % where the snap happens, so the momentum
	// skids off instead of following the nose up the face (specs/lip-snap-spinout-and-air.md M2/T2).
	// The toward-wave yaw rate separates the two: ~0 riding steady, 1+ rad/s in a snap. 0 = off.
	// Default 1.0, measured headless 2026-09-18 with the yaw cap, takeoff untouched (device-like harness):
	// spin-out trace ride mean 453 -> 607 (snap window 247 -> 656, slip 175 -> 18 deg), hybrid 490 -> 588,
	// 4-snap shortboard 247 -> 305, hard_turn autopilot unchanged (maxUW 0.76). 0.5 measured the same.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Damping")
	float CarveGripTurnUnfadeRate = 1.0f;

	// Wave-carry redirect (horizontal keep-up): rotation rate (rad/s) at which the board's HORIZONTAL velocity
	// is bent toward the wave's shoreward travel direction (-resolvedWaveBackDirection), so it holds station on
	// the crest instead of the wave rolling over it. Speed-preserving keep-up drift, self-limited by the
	// velocity-deficit gate (fades as the board's shoreward speed reaches WaveCarryTargetCrossSpeed). 0 = off.
	// 1.0 = player-verified default ("feels good", 2026-07-19). See specs/wave-carry-redirect.md.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Damping")
	float WaveCarryRedirectRate = 1.0f;

	// Target shoreward (cross-shore) speed (cm/s) the wave-carry redirect drives the board toward — roughly the
	// crest's own cross-shore phase speed. Self-limiting gate: the redirect drives while the board's shoreward
	// speed is below this and fades to 0 as it matches, so it settles instead of over-rotating.
	// See specs/wave-carry-redirect.md.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Damping")
	float WaveCarryTargetCrossSpeed = 60.0f;

	// Scales the force-based anti-slip (yaw-hydrofoil + fin-lift side component) so it can be dialed down as
	// the carve-grip redirect takes over slip-resistance. 1 = full force anti-slip; 0 = removed (grip
	// redirect carries it — the tuned default; the forward/carve-coupling thrust component is unaffected).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Damping")
	float AntiSlipForceScale = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Damping")
	float ClampYVelocityAt = 5000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Damping")
	float MaxVelocityX = 3000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Damping")
	float MaxVelocityZUp = 500.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Damping")
	float MaxVelocityZDown = 10000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Damping")
	float VelocityDampingThreshold = 2000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Damping")
	float VelocityDampingScale = 0.0005f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Damping")
	float MaxVelocityDamping = 0.95f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Damping")
	float AmountUnderWaterEquilibrium = 0.5f;

	// --- Lift (formerly FluidDynamicsConstants BP, passed-as-args). All
	// defaults captured from the live BP-overridden values 2026-06-07. ---

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Lift")
	float bottomLiftMagnitude = 0.001f;

	// Lowered from 0.001 -> 0.0002 (2026-06-15): at 0.001 the rail lift fired at ~-4215 (board-space,
	// sideways into the wave) — ~2x its opposing bottomHydrofoilYaw (~+2051), tipping the sideways
	// balance into the wave and feeding the involuntary roll/carve. Dialed down further to keep the
	// rail from pulling the board into the wave. Tune live on-device (grip vs into-wave pull).
	// See specs/barrel-glide-through-bug.md.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Lift")
	float railLiftMagnitude = 0.0002f;

	// Decouples railLift's keel-roll: railLift is sideways and applied below the CoM, so it makes a
	// -r_up*F_left roll torque (the dominant down-the-line destabiliser). 1 = apply at CoM height (no roll,
	// keeps grip+yaw); 0 = legacy at-actor. See specs/wave-mass-drag-torque-decoupling.md.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Lift", meta=(ClampMin="0.0", ClampMax="1.0"))
	float railLiftRollDecouple = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Lift")
	float finLiftMagnitude = 0.01f;

	// --- Drag (formerly FluidDynamicsConstants BP, passed-as-args). ---

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Drag")
	float bottomDragCoefficient = 0.4f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Drag")
	float maxDragAmount = 1e+08f;

	// Multiplier on the bottom FORWARD drag. Was a hardcoded 10.0f in calcDragForce() labelled
	// "EXPERIMENTAL: 5x hardcoded multiplier while iterating on the right coefficient" -- the comment
	// said 5 while the value was 10, so it had been doubled without review. Exposed here because it is
	// the largest single brake in the game: measured at -131k board-wide during hard deceleration,
	// against a total of +57k for everything pushing. Default 10.0 preserves the old behaviour.
	// See specs/flat-water-propulsion-audit.md (pitch-driven surge/brake investigation).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Drag", meta=(ClampMin="0.0"))
	float bottomForwardDragMultiplier = 10.0f;

	// 0..1: scale the bottom forward drag by the actor's real wetting (actorWetted) instead of letting
	// it ride on effectiveWaterHeight alone. effectiveWaterHeight keeps a deliberate non-zero baseline
	// when the actor is NOT submerged (so trough forces stay alive), which means this v^2 drag went on
	// braking a board that had left the water: 37% of the hardest braking ticks measured underW = 0.00
	// at a mean 1074 cm/s. 0 = legacy (no wetting scaling), 1 = fully scaled by actorWetted.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Drag", meta=(ClampMin="0.0", ClampMax="1.0"))
	float forwardDragWettingGate = 1.0f;

	// --- CONTACT GATE: a board in the air is not in the water. specs/airborne-force-gating.md ---
	//
	// forwardDragWettingGate above fixed ONE term this way. The same hole is in every other term,
	// because they all multiply effectiveWaterHeight, whose baseHeight + slopeSin*slopeHeight part
	// stays non-zero with the hull clear of the water (deliberately, so the trough can't strand the
	// board). slopeSin is 0.55-0.59 at the moment the lip launches the board — its largest value
	// anywhere — so the water forces are at their STRONGEST exactly where there is no water.
	//
	// Measured (fish, phone-2026-09-22-20-26-02, -RailsUntil=3.00, carve-grip knobs at 0): over
	// t=3.99..4.34, 24 cm clear of the surface, horizontal speed went 524 -> 977 cm/s (+86%) and the
	// heading turned +11.8 deg. Gravity is vertical, so BOTH must be zero across that window.
	//
	// airborneForceScale = what fraction of the water forces survives with no contact at all.
	//   0 = the fix (the default). 1 = the old ungated behaviour, bit for bit (the A/B control).
	// Intermediate values keep a token amount, e.g. for aerodynamic drag.
	//
	// Shipped at 1 (inert) while it waited for a feel pass; **device-verified by the owner and baked
	// at 0 on 2026-09-23**, alongside the carve-grip gate knobs and the wave-mass lip term. Covers
	// every force that reads effectiveWaterHeight plus finLiftForce, waveSlopeGravityForce,
	// lateralTurnForce, lipImpact, and the three wave-mass sites that recompute boardWideEffectiveH.
	// A submerged actor gates to exactly 1, so the trough baseline is untouched (AC2).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Drag", meta=(ClampMin="0.0", ClampMax="1.0"))
	float airborneForceScale = 0.0f;

	// Clearance (cm, actor above the wave surface) over which the contact gate fades 1 -> scale.
	// Measured from the SIGNED surface distance, not waterColumnAbove, which clamps the above-water
	// side to 0 and so cannot tell "just breaking the surface" from "a metre in the air". A submerged
	// actor is therefore gate = 1 exactly, which is what keeps the trough baseline intact (AC2).
	// Wide enough to cover hull thickness and spray rather than snapping at the waterline.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Drag", meta=(ClampMin="0.0"))
	float airborneFadeDistance = 10.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Drag")
	// 0.0002: the true-cosine fin fix made cos⁴ ~1400× larger at near-broadside, so the old 0.01
	// (itself down from 0.1) spiked fin drag to 48k–313k N at the high-speed pop-up transition and
	// braked/spun the board. 0.0002 tames the spike with no observed loss of carve grip.
	float finDragCoefficient = 0.0002f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Drag")
	float railDragCoefficient = 1e-05f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Drag")
	// Lowered 15 → 3 (2026-07-21) for the steep-face takeoff: at 15 the tail skin drag (v²-relative
	// shove from water hitting the tail from behind) measured ~40 kN at wave-catch and pushed the
	// board to ~190 cm/s on a gentle (slopeSin 0.13) face even with the wave-mass/slope-thrust gates
	// closed. At 3 the glued phase stays ≤ ~90 cm/s and takeoff waits for the steep face.
	// Lowered further 3 → 1 (live-tuned 2026-07-21) with the takeoff thresholds at 0.35.
	// See specs/steep-face-takeoff.md AC3.
	// Lowered 1 → 0.1 (live-tuned on PC 2026-08-24, baked from Saved/TuningOverrides.json).
	float tailDragCoefficient = 0.1f;

	// --- Thrust (BP-passed alongThrust/upwardsThrust + AFluidDynamics
	// shared-by-design coefficients merged into one category). ---

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Thrust")
	float alongThrustCoefficient = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Thrust")
	float upwardsThrustCoefficient = 0.2f;

	// Was 11.0 on bottom actors and 0 on rails/fins/tail in the BP. Code path
	// is bottom-only (see calcThrustForce gates), so unifying to 11.0 here
	// is behavior-preserving.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Thrust")
	float thrustMagnitude = 11.0f;

	// Was 0.2 on bottom actors and 0.3 elsewhere. Bottom-only code path; pick
	// the bottom value as the live one.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Thrust")
	float forwardsThrustCoefficient = 4.0f;

	// Was 0.2 on bottom, 0.1 elsewhere. Bottom-only.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Thrust")
	float actorForwardsThrustCoefficient = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Thrust")
	float upwardsThrustPitchSensitivity = 0.0f;

	// Was 0.001 on bottom, 0 elsewhere. Bottom-only.
	// Raised to 0.02 on 2026-08-21 (20x) — a deliberate change to how much slip is redirected
	// rather than shed, not a nudge. Re-approve the snapshot baselines it moves.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Thrust")
	float yawHydrofoilCoefficient = 0.02f;

	// Speed-attenuation band (cm/s, on relWaterVelMag) for the yaw-hydrofoil FORWARD carve-coupling thrust.
	// That thrust ∝ v²·sin²(slip); v² grows with board speed → unbounded speed feedback that surges the
	// board then lets the v²-drags slam it back (the down-the-line jerk). Taper the forward component to
	// zero as relWaterVelMag goes start→end (mirrors the pump speed attenuation) so it drives at low speed
	// but self-limits to a cruise. start >= end disables (default 0/0 = current behaviour, A/B-able).
	// See specs/yaw-thrust-speed-attenuation.md.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Thrust", meta=(ClampMin="0.0"))
	float yawThrustAttenStart = 300.0f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Thrust", meta=(ClampMin="0.0"))
	float yawThrustAttenEnd = 700.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Thrust")
	float maxHydrofoilForceAmount = 20000.0f;

	// --- Yaw-hydrofoil FORWARD-drive gating. See specs/flat-water-propulsion-audit.md (FR1). ---
	//
	// The forward (carve-coupling) half of the yaw hydrofoil had no slope gate and no front-face gate,
	// unlike every other propulsion term. Measured on the shortboard it supplied ~99% of net forward
	// force on flat water and 100% of it behind the crest, which is what let the board hold speed off
	// the face, in the whitewash and while inverted. These gate it the way passive slope thrust is
	// gated. NOTE: the anti-slip SIDE component cannot be "preserved instead" — AntiSlipForceScale is 0
	// by tuned default, so the forward component is the entire output of this term, and the slip-into-turn
	// redirection the 0.001 -> 0.02 raise (commit 936a0bce9, 2026-08-21) was for is delivered THROUGH the
	// forward drive. That is why only the front-face half of the gating survived measurement.
	//
	// The gate is a FLOOR, not a switch:
	//     fwdGate = yawFwdOffFaceFloor + (1 - yawFwdOffFaceFloor) * slopeGate * frontGate
	// so 0 removes off-face drive entirely and 1 restores the pre-fix behaviour exactly.
	// Minimum-slope deadzone for the forward drive: zero below, full above (like every other slope gate
	// here — nothing in this file gates a force ABOVE a steepness, and nothing should). 0.10 ~ 6 deg,
	// below which there is no face to be driven down.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Thrust", meta=(ClampMin="0.0"))
	float yawFwdSlopeGateMin = 0.05f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Thrust", meta=(ClampMin="0.001"))
	float yawFwdSlopeGateWidth = 0.06f;

	// 0..1 — the fraction of forward drive that survives OFF the face and behind the crest. This is the
	// per-board "how much does leaving the pocket cost you" dial, set in Content/Boards/<id>.json:
	// the rideable core of the wave is only ~3 m wide, so a beginner board needs a floor to stay fun
	// while a shortboard gets none. 1.0 = pre-fix behaviour. See board-selection.md — the board is the
	// difficulty setting, and this is the knob that makes that true for wave position.
	// Per-board in Content/Boards/<id>.json — 0 removes off-face drive entirely (shortboard), higher
	// values keep some so a beginner board is not punished for leaving the ~3 m pocket (foamie 0.35).
	// 1.0 restores the pre-gate behaviour exactly, which is the control for any A/B.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Thrust", meta=(ClampMin="0.0", ClampMax="1.0"))
	float yawFwdOffFaceFloor = 0.0f;

	// With-the-wave gate on the yaw-hydrofoil FORWARD drive. The term is v²·sin²(slip)-shaped and
	// sign-blind: a board pointed up the face and moving into the flow makes its own relative speed
	// and drives itself over the crest (measured 2026-09-16: 6 -> 207 cm/s in 0.5 s, the yaw term at
	// 180 kN, board stationary in front of an arriving crest).
	//
	// Gate on the board's VELOCITY direction, not its nose: d = dot(velocity horizontal, shoreward),
	// shoreward = -resolvedWaveBackDirection (the wave's travel axis from the tile geometry - on the
	// infinite-wave level (0.891, 0.454), NOT -X). Below ~150 cm/s the velocity direction means
	// nothing and the nose direction is blended in instead. Why velocity and not nose (measured
	// 2026-09-17 on the player's "brakes when turning into the wave" trace): a top turn and punching
	// out the back are the SAME nose orientation (~75 deg, nose.shoreward -0.6..-0.7); a nose gate
	// cost a normal turn 370 cm/s. What differs is where the board is GOING - along the line in a
	// turn (velocity.shoreward -0.40..-0.18), seaward in the pushes (-0.91..-0.99). Physically: the
	// hydrofoil cannot extract energy from a wave the board is moving against.
	//     withWaveGate = SmoothStep(yawFwdWithWaveGateOff, yawFwdWithWaveGateOn, d)
	//     ... applied only below yawFwdWithWaveGateSpeedMax (blended out over the 100 cm/s above)
	// -1.0/-0.90 (2026-09-18, was -0.85/-0.55): a PC shortboard cutback ran the velocity up the
	// face (d -0.63 -> -0.96) for a second at 300 -> 200 cm/s and the gate stalled it 378 -> 143.
	// The case the gate exists for is a SLOW board being driven into an arriving crest (6 -> 207
	// cm/s), so it now also fades out above 150 cm/s: a board with speed keeps its drive whatever
	// its heading. Disable (A/B control): set On <= Off.
	// NOT applied to the slope thrust's fin-redirect: that term is the drive through a turn
	// (50-115 kN on the player's turn) and gating it was the braking the player reported.
	// See specs/yaw-hydrofoil-flow-direction.md FR1.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Thrust", meta=(ClampMin="-1.0", ClampMax="1.0"))
	float yawFwdWithWaveGateOff = -1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Thrust", meta=(ClampMin="-1.0", ClampMax="1.0"))
	float yawFwdWithWaveGateOn = -0.90f;

	// Board speed (cm/s) above which the gate stops applying (blended out over the next 100 cm/s).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Thrust", meta=(ClampMin="0.0"))
	float yawFwdWithWaveGateSpeedMax = 150.0f;

	// Passive slope thrust (gravity-down-the-face drive, redirected along board.forwards).
	// See specs/passive-slope-thrust.md. Validated 2026-06-12: speed surges track slopeSin (board
	// reaches ~530 cm/s on a face at coef 60000, decays on the gentle shoulder). 10000 is too weak
	// to matter; 60000 gives strong realistic face drive. 40000 = moderate working value; final
	// tuning needs a steered ride that keeps the board in the pocket.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Thrust")
	float slopeThrustCoefficient = 40000.0f;

	// Min board-wide slopeSin before slope thrust engages — deadzone so it stays OFF on near-flat
	// water between waves (the un-gated term flung a slow board around). Raised 0.12 → 0.25 → 0.35
	// (~20° face, live-tuned 2026-07-21) so the board stays glued until the face is genuinely steep
	// instead of taking off at ~7° the moment the wave arrives. Keep equal to waveMassMinSlopeSin —
	// the effective takeoff threshold is whichever of the two is lower. Practical ceiling on the
	// test wave is ~0.40 (max slope under a glued board measured 0.47). See specs/steep-face-takeoff.md.
	// Retuned 0.35 → 0.01 after the wave-velocity registration fix (2026-07-22): with corrected water
	// velocities the wave-carry/glue dynamics changed and the steep-face gate held the board glued far
	// too long; ~0 = drive available as soon as any face exists, takeoff timing now emerges from the
	// force balance instead of a slope gate. Keep equal to waveMassMinSlopeSin.
	// Re-raised 0.01 → 0.35 (2026-07-23, player-validated — back to the pre-registration-fix value):
	// at 0.01 slopeThrust became the dominant drive on near-flat faces (measured ~5-6k/actor at
	// slopeSin 0.18, ~63% from the finRedirect term while riding down the line) so the board held
	// 800+ cm/s on a ~4° face and never slowed between waves. The flat-face problem the 0.01 gate
	// caused is instead fixed by the lowered maxSupplementForce (see there); takeoff timing stays
	// acceptable because the corrected water velocities carry the board to the steep-face threshold.
	// See the flat-face speed-gain investigation on branch no-gain-speed-on-flat-waves.
	// 0.35 -> 0.10 (2026-09-05). Required by the yaw-hydrofoil face gate: with the off-face drive gated
	// away, the board needs the wave's own propulsion available ON the face, and at 0.35 slope thrust
	// fired on only 9.5%% of ticks while the board rides below slopeSin 0.10 for 58%% of the time. The
	// early-takeoff constraint that set 0.35 no longer binds — takeoff is kinematic since 2026-08-25
	// (specs/deterministic-ride-handoff.md), so physics resumes with the board already planing.
	// Keep equal to waveMassMinSlopeSin. See specs/flat-water-propulsion-audit.md.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Thrust")
	float slopeThrustMinSlopeSin = 0.05f;

	// Fraction of down-slope gravity's lateral (board-left) component the fins redirect into forward
	// drive. Lets the board gain speed traversing the face (rail ~parallel to wave) and removes the
	// nose-up-face stall. 0 = old pure forward-projection drive. See passive-slope-thrust.md.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Thrust")
	float slopeThrustFinRedirect = 0.5f;

	// Front-face gate for passive slope thrust. The wave always travels the same world direction, so the
	// rideable FRONT face has its fall-line (down-slope) pointing roughly this way. The drive is scaled by
	// how well the board-wide down-slope aligns with this vector: full on the front face, suppressed on the
	// BACK of the crest — where the down-slope reverses and the 40000-strong drive would otherwise power the
	// board over/through the wave (the glide-through bug). Set to (0,0,0) to disable the gate.
	// See specs/wave-crossing-deceleration.md. Default points -X (measured front-face fall-line direction).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Thrust")
	FVector slopeThrustFrontFaceDir = FVector(-1.0f, 0.0f, 0.0f);

	// Default validated on-device; was 0.0 (disabled), which reset the phone-tuned value on every deploy.
	// Lowered 20000 -> 4000 to tame the small-weight over-turn (carve gain was too hot; the turn force is
	// worldRollSin x sinPitch x v x planing x this, and a ~2.7deg roll x 20000 produced a ~12-15k carve).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Thrust")
	float lateralTurnCoefficient = 4000.0f;

	// Saturating speed cap (cm/s) for the lateral carve term. The carve force scales with
	// relativeWaterVelocityMagnitude, which is only ~150 in normal down-the-line trim but spikes to
	// ~1200+ during a hard turn — so a barely-visible few-degree residual roll produces a runaway
	// carve (~36k) that yaws the board into the wave with centered weight. We replace the raw speed
	// with vEff = v / sqrt(1 + (v/cap)^2): nearly linear below the cap (normal carving untouched),
	// asymptotes to the cap above it (spike tamed). <= 0 disables the cap (raw linear speed).
	// See specs/carve-grip-via-redirect.md and the small-weight-shift-sharp-turn over-carve note.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Thrust")
	float lateralTurnSpeedCap = 300.0f;

	// Small-roll deadzone (sin units, ~radians for small angles) gating the carve term via smoothstep:
	// zero carve below this |worldRelativeRollSin|, ramping to FULL by 2x it. The down-the-line
	// over-carve is seeded by only ~2-6deg of residual roll ("no visible roll") amplified by planing
	// speed; the gate stops sub-threshold roll from seeding the turn while a deliberate lean keeps full
	// carve above the band. 0 disables. 0.04 ~= 2.3deg. Pairs with lateralTurnSpeedCap (spike tamer).
	// See specs/carve-grip-via-redirect.md.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Thrust")
	float lateralTurnRollDeadzone = 0.04f;

	// Progressive hard-carve boost. The board is roll-stiff — it banks only ~11deg even when the
	// surfer commands a hard ~54deg lean — so the carve force (keyed on the board's ACHIEVED roll)
	// makes a hard turn barely faster than gentle down-the-line trim (measured ~20 vs ~17 deg/s on
	// phone-2026-07-24-22-27-17). This multiplies the carve by (1 + boost * ramp), where ramp is a
	// smoothstep over the COMMANDED lean (|amountToTheRight-0.5|). It fires only on a deliberate hard
	// commit and leaves the small-lean trim regime bit-for-bit unchanged (ramp = 0 below the band).
	// 0 = off. Default 0.5 = player-validated (~2.3x hard-turn rate, saturates by 0.5, trim untouched;
	// headless A/B + on-device feel). See specs/hard-carve-progressive-boost.md.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Thrust")
	float lateralTurnHardCarveBoost = 0.5f;

	// Commanded-lean band (|amountToTheRight-0.5|, range 0..0.5) over which the hard-carve boost ramps
	// in via smoothstep. Start is set well above the gentle down-the-line trim band (the rail-lift gate
	// ramps 0.05..0.20) so trim is untouched; full is the hard-commit end.
	// 0.30 chosen from phone-2026-07-24-22-27-17: gentle trim maxes at |commandedShift|~0.28,
	// hard turns peg at 0.50 (tilt saturates full deflection), so the ramp clears trim entirely.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Thrust")
	float lateralTurnHardCarveStart = 0.30f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Thrust")
	float lateralTurnHardCarveFull = 0.45f;

	// --- Buoyancy (BP-passed coefficients + ABuoyancy shared-by-design). ---

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Buoyancy")
	float basicFloatBuoyancyCoefficient = 60000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Buoyancy")
	float horizontalVelocityBuoyancyCoefficient = 20.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Buoyancy")
	float verticalVelocityBuoyancyCoefficient = 100.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Buoyancy")
	float amountUnderWaterPower = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Buoyancy")
	float weightForceMaxMultiplier = 1e+07f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Buoyancy")
	float distanceWhereMaxForceShouldBeApplied = 30.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Buoyancy")
	// Was 8cm (for the now-dead per-SC amountWetted). The bottom FluidDynamics actors rest ~37-50cm below
	// the surface (measured), so 8cm pegs the per-actor actorWetted at 1.0. 60cm un-pegs all of them and
	// still ramps sharper than amountUnderWater's ~1m. See specs/per-actor-wetting.md.
	float wettedTransitionDistance = 60.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Buoyancy")
	// Restores the actorWetted-gated force magnitudes (bottom Bernoulli lift, hydrofoil up-thrust / forward
	// thrust, yaw hydrofoil, slope thrust) that dropped when the always-1.0 per-SC amountWetted gate was
	// replaced by the per-actor actorWetted, which measures ~0.82 mean at rest. 1/0.82 ~= 1.22. Applied as a
	// uniform multiplier at the force-gate sites (not to the raw actorWetted used for the L/R righting
	// asymmetry / logs), so it lifts the common scale without touching the submerged-side ratio. 1.0 = legacy
	// (weakened) behaviour. See specs/per-actor-wetting.md.
	float wettedForceCompensation = 1.22f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Buoyancy")
	float waveFaceNormalInfluence = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Buoyancy")
	float horizontalVelocityBuoyancyFrontBias = 2.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Buoyancy")
	float boardHalfLength = 100.0f;

	// --- Wave-mass forces (AFluidDynamics shared-by-design). ---

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|WaveMass")
	float waveSlopeGravityCoefficient = 300000.0f;

	// Per-actor cap on the waveSlopeGravity supplement. At 100 it amputated the whole force (raw
	// per-actor ~84k on a steep face → the board had ~1/27th of physical downhill gravity and could
	// not accelerate down the face). 5000 (2026-07-22, tuned with the registration fix) ≈ energetic
	// but bounded; the per-axis velocity ceilings + sideways damping guard the old catastrophic-launch
	// failure the cap was added for.
	// Lowered 5000 → 1000 (2026-07-23, player-validated): at 5000 the supplement saturated the cap on
	// any slope past its own 0.10-0.20 smoothstep (~50k board-wide), a major contributor to the board
	// gaining speed on gentle faces. 1000 keeps the downhill assist without overpowering flat-glide drag.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|WaveMass")
	float maxSupplementForce = 1000.0f;

	// 0.0005 → 0.002 (2026-08-23).
	// 0.002 → 0.003 (live-tuned on PC 2026-08-26, baked from Saved/TuningOverrides.json).
	// → 0 (d4dbb60c1): the whole wave-mass force family was retired when the wave interaction moved
	// to damping + redirects, on the argument that a force cannot do a resisting or turning job
	// without overshooting.
	// → 0.0001 (owner, PC editor, 2026-09-23). NOT a reversal of that argument: at 1e-4, three and a
	// half decades below the 0.003 it was retired from, this is not the old drive term. It is the
	// lip's PUNCH — the owner tuned it up alongside the carve-grip gate and the airborne force
	// gating and found "big improvements ... gives the lip a bigger punch".
	// The two were kept equal every time they were tuned, up to and including 1e-4. That pairing
	// ENDED 2026-09-24: waveMassFlowDragCoefficient went on to 1e-5 on its own, so the thrust keeps
	// the punch and the drag does not. Tune them separately from here.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|WaveMass")
	float waveMassThrustCoefficient = 0.0001f;

	// Halved 0.002 → 0.001 (live-tuned 2026-07-21) alongside the steep-face takeoff thresholds —
	// softens the wave-mass shove overall, not just its onset. See specs/steep-face-takeoff.md.
	// Raised 0.001 → 0.004 (2026-08-23), alongside the waveMassThrustCoefficient bump above.
	// Halved back 0.004 → 0.002 (live-tuned on PC 2026-08-24, baked from Saved/TuningOverrides.json).
	// Nudged 0.002 → 0.003 (live-tuned on PC 2026-08-26, baked the same way — kept equal to
	// waveMassThrustCoefficient above, as both bumps were tuned together).
	// → 0 (d4dbb60c1), then → 0.0001 (owner, PC editor, 2026-09-23) — see the note on
	// waveMassThrustCoefficient above.
	// → 0.00001 (owner, 2026-09-24). **This breaks the keep-them-equal pairing**: the thrust
	// coefficient stays at 1e-4 for the lip's punch while the flow DRAG drops a further decade to
	// 1e-5. Do not "restore" the pairing by raising this back — the two are now deliberately
	// independent, and the drag half is the one the owner wanted quieter.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|WaveMass")
	float waveMassFlowDragCoefficient = 0.00001f;

	// Min slopeSin before ANY wave-mass force (flow drag on bottom/rail/tail, wave-mass thrust)
	// engages: SmoothStep(min, min + 0.06, slopeSin) multiplied into the magnitude. The flow push
	// previously scaled linearly from slopeSin 0 with no deadzone, shoving the board to planing
	// speed on a ~6° face the moment the wave arrived (measured ~35 kN — the #1 wave-catch pusher,
	// see specs/wave-catch-propulsion-budget.md). 0.35 ≈ 20° face = "steep enough to take off"
	// (live-tuned 2026-07-21; keep equal to slopeThrustMinSlopeSin). 0 = legacy behavior (gate
	// open at any slope > 0). See specs/steep-face-takeoff.md.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|WaveMass", meta=(ClampMin="0.0"))
	float waveMassMinSlopeSin = 0.05f; // keep equal to slopeThrustMinSlopeSin (see its comment)

	// === Broken-water signal (specs/broken-wave-no-consequences.md A4, M11-M12) ===
	// The height and velocity data cannot tell a bore from a face (a bore front reads slopeSin
	// 0.23-0.34, as steep as the ridden face; the water column is the hull's draft either way).
	// The whitewater point cloud CAN: it is the sim's own record of where the wave has broken.
	// brokenAmount = max(aheadTest, denseTest), smoothed over FoamBrokenSmoothSeconds, where
	//   aheadTest = SmoothStep(FoamAheadCountLow, FoamAheadCountHigh, foam points within
	//               FoamAheadRadius in the half-disc AHEAD of the board's motion (nose below 100 cm/s))
	//   denseTest = SmoothStep(FoamBrokenCountLow, FoamBrokenCountHigh, foam points within
	//               FoamBrokenRadius all round).
	// Why two (M12, 2026-09-17, 796 face ticks / 338 whitewater ticks / 67 wrong-way ticks): in the
	// pocket the board rides beside the lip's foam, so an all-round count on a face has a fat tail
	// (p90 284, max 445) that overlaps thin whitewater (a board turning back into the broken section
	// read 95-215). But on a face the foam is all BEHIND the board and the water ahead is clean
	// out to 6 m (ahead600 p95 34, max 104), while heading into the broken section reads 148-327.
	// The dense test covers sitting in the foam while moving out of it (whitewater min all-round
	// 409, above the face's 445 only with the knee at 420-520). RE-READ THE COUNTS ON DEVICE: the
	// cache holds the camera-culled particles and the Android particle budget is lower.
	// `surf.debug.flags 'foam'` on SharedCalculations logs every input and the result.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Waves", meta=(ClampMin="50.0"))
	float FoamAheadRadius = 600.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Waves", meta=(ClampMin="0.0"))
	float FoamAheadCountLow = 60.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Waves", meta=(ClampMin="1.0"))
	float FoamAheadCountHigh = 160.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Waves", meta=(ClampMin="50.0"))
	float FoamBrokenRadius = 300.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Waves", meta=(ClampMin="0.0"))
	float FoamBrokenCountLow = 420.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Waves", meta=(ClampMin="1.0"))
	float FoamBrokenCountHigh = 520.0f;

	// Seconds of first-order smoothing on brokenAmount so a flickering particle count does not
	// flicker the score rate or the drive. 0 = raw.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Waves", meta=(ClampMin="0.0"))
	float FoamBrokenSmoothSeconds = 0.25f;

	// A2 (specs/broken-wave-no-consequences.md): the face-keyed drives - yaw-hydrofoil forward and
	// passive slope thrust - scale by (1 - brokenGeo * this). A bore front is a 0.3 face to the data
	// and drove a board turned back into the whitewater from 119 to 663 cm/s (2026-09-17).
	// Keyed on brokenGeo (the wave-geometry service) since 2026-09-21. The foam reads it keyed on
	// before were wrong for the job: the ahead-inclusive read zeroed the drive for a whole hard turn
	// whenever the nose swung at the lip's foam on a clean face (the board bled 350 -> 79 cm/s and
	// skidded), and the surround read never fired (peak 0.12) - so it shipped OFF from 2026-09-18.
	// The service reads 0 through the whole slalom and every pocket ride (wave-geometry AC2), so
	// the cut cannot fire on the face; M16 measured 14-38-37's wrong-way run-up (mean speed
	// relative to the water along the nose in the whitewater 285 -> 36 cm/s, the run-up gone) with
	// the pocket and shoulder identical to the cm. 1 = no face drive at broken 1; 2 = none past
	// broken 0.5 (M20: with the along-nose damping relaxed to keep a board's forward momentum, a
	// half-broken board surged to 730 on the drives at 1). 0 = the A/B control.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Waves", meta=(ClampMin="0.0", ClampMax="4.0"))
	float BrokenDriveCut = 2.0f;

	// ===================================================================================
	// RESTORING THE PRE-DAMPING BUILD (the A/B control)
	//
	// The wave interaction moved from v² forces to damping + redirect, so "0 disables it" is no
	// longer the control — these defaults ARE the new behaviour. To measure against the old build,
	// put this whole set back at once (Saved/TuningOverrides.json, no recompile):
	//
	//   waveMassThrustCoefficient    0.0001  -> 0.003
	//   waveMassFlowDragCoefficient  0.00001 -> 0.003
	//   wavePenetrationCoefficient   0      -> 0.0003
	//   waveNormalDampingRate        0.06   -> 0
	//   waveNormalHeightDelta        60     -> 0
	//   slopeThrustMinSlopeSin       0.05   -> 0.10
	//   yawFwdSlopeGateMin           0.05   -> 0.10
	//   waveMassMinSlopeSin          0.05   -> 0.10
	//
	// Half a swap is worse than either side: the damping and the forces do the same job, and running
	// both measured RMS acceleration +80%. Change the set, not one line of it.
	//
	// 2026-09-23: the two wave-mass coefficients are no longer 0 — the owner baked them at 1e-4 for
	// the lip's punch. That does NOT weaken the warning above: 1e-4 is three and a half decades
	// below the 0.003 this control restores, so the family is still nowhere near doing the drive job
	// the damping took over. The set is what changes together; the from-values just moved.
	// 2026-09-24: waveMassFlowDragCoefficient moved on to 1e-5, so the two are no longer equal and
	// the from-values above differ. Same conclusion, more so — the flow drag is now four and a half
	// decades below the value this control restores.
	// See specs/wave-interaction-damping-and-redirect.md (M12).
	// ===================================================================================

	// === Height-derived wave normal (step 2, specs/wave-interaction-damping-and-redirect.md) ===
	//
	// The STORED normals do not describe the stored heights: measured over one grid row, the
	// gradient the normal implies matches the heights exactly in X (ratio 1.000) but is ~2.14x too
	// steep in Y at the median, and 10-12x on the flat shoulder (M6). Drawn on the wave, the
	// horizontal projections speckle red/green sample to sample — it is NOISE, not a rotation.
	//
	// Noise inflates `slopeSin` even though it does not bias a direction, because
	// slopeSin = sqrt(1 - nz^2) is a MAGNITUDE: a noisy vector's length is biased upward and the
	// error cannot cancel. Measured inflation 1.59x median, 3.35x on the shoulder.
	//
	// Every propulsion term gates at slopeSin 0.10 and scales with it, so that inflation is upstream
	// of the whole drive stack. Heights are clean (a continuous location.z; it is the normals that
	// came off ray_cast), so this derives the normal from the height field by central difference
	// instead: g = (dz/dx, dz/dy), n = normalize(-gx, -gy, 1).
	//
	// Value is the half-step in cm. Grid spacing is ~56 cm cross-shore and ~40 cm down the line, so
	// anything below that aliases. 0 falls back to the stored normals. Default 60: measured to cut
	// p99 acceleration 24%, RMS accel 15% and churn 13% in the swap — a cleaner signal, not a
	// different slope (in situ the two agree within ~15%; M6's 1.59x does not reproduce on a ride).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|WaveMass", meta=(ClampMin="0.0"))
	float waveNormalHeightDelta = 60.0f;

	// === Wave-normal damping (FR1, specs/wave-interaction-damping-and-redirect.md) ===
	//
	// Damps the wave-normal component of the board's velocity measured RELATIVE TO THE WATER, in the
	// Chaos fork, after the redirects. Replaces the v² forces that used to do this job and overshot:
	// wavePenetrationDrag was measured going from -2,177 (resisting) to +12,507 (driving).
	//
	// The target is the water's velocity, not zero. Damping toward zero is world-frame and would hold
	// a stationary board still while a wave arrives instead of letting it be carried. Toward the water
	// one formula gives the carry push, the penetration block, and no braking when board and water
	// move together.
	//
	// 0..1 per DampingRefDt, like the other damping coefficients. 0 disables it. Default 0.06 is the
	// measured knee: 0.12 buys almost nothing more and stalls the board more.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|WaveMass", meta=(ClampMin="0.0", ClampMax="1.0"))
	float waveNormalDampingRate = 0.06f;

	// Slope gate for the wave-normal damping — "is there actually a wave face here".
	//
	// Without this the damping is only CONTACT gated, so on flat water it stays fully on (the board
	// is still floating, underW ~0.3-0.4) and keeps hauling the cross-shore velocity component down
	// toward the water's, which out there is only ~53 cm/s. The board glides off the wave and stops
	// dead. Reported from play, 2026-09-08. The v² forces this damping replaced were all gated on
	// waveMassMinSlopeSin; the damping inherited the contact gate but not the slope one.
	//
	// Measured separation over a ride: flat water sits at slopeSin p50 0.037 / p90 0.071, the wave
	// face at p50 0.118 / p90 0.405. So a ramp from 0.05 to 0.10 is off on flat water and fully open
	// on the face. Water speed also separates them (53 vs 105-313 cm/s) but less cleanly, and slope
	// is the signal the retired forces used.
	//
	// 0 disables the gate (damping everywhere, the reported bug).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|WaveMass", meta=(ClampMin="0.0"))
	float waveNormalDampingMinSlopeSin = 0.05f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|WaveMass", meta=(ClampMin="0.001"))
	float waveNormalDampingSlopeWidth = 0.05f;

	// === The whitewater (specs/broken-wave-no-consequences.md A1, A3; signal = brokenGeo, A4) ===
	//
	// A1, the shove. Two damping terms in the fork, both toward the water's sampled velocity, both
	// keyed on brokenGeo (never a force - memory forces-are-not-the-default-tool):
	//  1. the wave-normal damping's gate opens on brokenGeo (gate = contact x max(slopeGate, broken))
	//     and its rate scales by lerp(1, this, broken) - the cross-shore component;
	//  2. p.Chaos.Solver.WhitewaterDampingRate = waveNormalDampingRate x this x broken x a soft
	//     contact gate - the WHOLE horizontal velocity. A board in a bore is a cork in aerated water:
	//     it keeps neither its along-line speed nor its heading's momentum.
	// Why both (M16/M17 on the T1 fixture, hybrid, nose up the line): the normal-only term at the
	// face rate 0.06 carried the board at 0.39x the water's speed; scaled x4 0.61x, x10 0.73x - but
	// the board kept threading the bore along its nose at -150 cm/s and moved 0.5 m. With the whole
	// velocity damped (M17) it is carried 5.4 m (x4) / 7.8 m (x10) shoreward over one wave against
	// the control's 0.5 m, at the water's own speed. 10 = the board tracks the water fully (0.6 per
	// tick while broken); back off toward 4 for more of the board's own inertia. The shove starts
	// 2 m behind the modelled impact (PocketBehind): a board riding tight under the lip reads the
	// same water (-262) as a lip landing, so the front itself cannot be claimed within the model's
	// +-2 m; the board catches the bore's wake (1.2-1.8 m/s), not the front (3 m/s). The A/B control
	// is BrokenDampingGate 0.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Waves", meta=(ClampMin="0.0"))
	float BrokenDampingRateScale = 10.0f;

	// M18: the whitewater damping is anisotropic in the board's frame - across the hull at the full
	// rate, along the nose at rate x this. The owner's device ride (phone-2026-09-21-17-54-27): with
	// the isotropic term the peel overtook a board riding down the line at 404 cm/s and took its
	// along-line speed to 139 in 0.75 s while shoving it to -394 sideways in 0.25 s - "the board goes
	// from moving only forwards to only sideways". A hull is a wall to water across it and
	// streamlined along it: 0.2 = a 0.3-0.4 s decay along the nose at x10 against 0.07 s across, so
	// the board keeps most of its forward speed for a second or two while being dragged sideways.
	// 1 = isotropic (the M17 behaviour).
	// 0.2 -> 0.1 (M20): at broken 1 the along-nose decay is 0.6 x this per tick - 0.14 s at 0.2, which
	// is still "the forwards velocity stops" (the owner's second ride: 514 -> 178 in 0.8 s); the
	// forward hold seen on the overtake fixture came from the broken ramp, not from the fraction.
	// 0.05 lets a partly-broken board surge to 730 on the drives, so the drive cut is sharpened to
	// match (BrokenDriveCut 2: no drive past broken 0.5). 0.1 = a 0.28 s decay at broken 1.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Waves", meta=(ClampMin="0.0", ClampMax="1.0"))
	float WhitewaterAlongNoseScale = 0.1f;

	// === The off-wave ending (specs/surfer-fall-ragdoll.md, "Off the wave", 2026-09-21) ===
	// The ride ends (LOST THE WAVE) once the board has been off the clean wave - the wave-geometry
	// service's zone not Pocket or Shoulder: whitewater, behind the wave, the flat - for
	// FallOffWaveSeconds and is doing less than FallOffWaveSpeed at that moment. The owner on the
	// device rides of 2026-09-21: "the board is moving in the white water very slowly for a number
	// of seconds before the surfer falls off; I would like him to fall off sooner." The planing
	// stall (fallPlaningStopThreshold) needs the board to wallow under ~200 cm/s for its ~2 s decay,
	// and the whitewater carries a board at 200-300 - so those rides dragged on 6-10 s. Measured on
	// the four rides with 3 s / 300: each ends 3-3.5 s after leaving the wave (t=14.1 / 8.9 / 26.4 /
	// 11.2 against 21.3 / 15.0 / 32.3 / 18.2), a board riding the whitewater to the beach at >= 300
	// keeps going. The timer resets the moment the board is back in the pocket or on the shoulder.
	// 0 seconds = off.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Fall", meta=(ClampMin="0.0"))
	float FallOffWaveSeconds = 3.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Fall", meta=(ClampMin="0.0"))
	float FallOffWaveSpeed = 300.0f;

	// 0/1: open the wave-normal damping gate on brokenGeo (A1). 0 = the pre-2026-09-21 behaviour,
	// slope gate only - the A/B control for the shove.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Waves", meta=(ClampMin="0.0", ClampMax="1.0"))
	float BrokenDampingGate = 1.0f;

	// === Propulsion budget (FR6, specs/wave-mass-thrust-closing-speed.md) ===
	//
	// The forward-drive terms compensate for each other: zeroing waveMassThrust — half the gross
	// drive on surge ticks — moved the NET forward force by only 9%, because waveMassFlow (+488%),
	// dragRailFlow (+326%) and wavePenetration (sign flip) grew to replace it. They are all functions
	// of where the board sits on the wave, and removing drive moves the board somewhere the others
	// are stronger. So the limit is applied to the SUM: hold the total and there is nothing to refill.
	//
	// Governs the along-board.forwards components of waveMassThrust, bottomSlopeThrust,
	// bottomHydrofoilYaw (forward half) and waveSlopeGravity. Braking is never touched, and the
	// forces carrying the sideways-glide-through block and the nose-up pitch are excluded.
	//
	// Units are BOARD WEIGHTS (1.0 = mass × |gravityZ|), so the numbers survive a mass change.
	// Measured today: 1.46 board weights net forward on the top decile of surge ticks.
	//
	// propulsionCeilingWeights <= 0 DISABLES the governor entirely — that is the A/B control.
	//
	// DEFAULT IS OFF. Measured 2026-09-07 at knee 0.6 / ceiling 1.0: the governor engages hard where
	// it should (scale 0.08 at p1, 0.21 at p5, raw demand p90 = 1.39 board weights), but the net
	// forward force on surge ticks fell only 12% — and the ride got SPIKIER, not calmer
	// (p99 accel +26%, time above 1000 cm/s 1.9% -> 3.2%). The excluded R3/R4 forces refilled the
	// budget: dragBottom_waveMassFlow +330%, dragRailFlow up with it. That is the FR6.6 leak,
	// predicted in the spec and now measured. AC1 and AC4 both fail, so this ships OFF until
	// FR6.7 (staggering the shared slopeSin gates) or a different approach closes the leak.
	//
	// The mechanism is kept because it works as designed and is a clean A/B vehicle for that work.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|WaveMass", meta=(ClampMin="0.0"))
	float propulsionCeilingWeights = 0.0f;

	// Drive passes through untouched below this; above it, compression ramps in smoothly toward the
	// ceiling (asymptotic — the total never actually reaches the ceiling, so there is no hard clip
	// for the board to hit). Must be < propulsionCeilingWeights or the governor disables itself.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|WaveMass", meta=(ClampMin="0.0"))
	float propulsionKneeWeights = 0.6f;

	// The ceiling scales with the local wave energy (|absoluteWaterVelocity|²) so a bigger, faster
	// wave still drives the board harder — without this the budget is a governor rather than physics.
	// This is the flow speed (cm/s) at which the ceiling equals propulsionCeilingWeights exactly.
	// Measured: ~233 cm/s at the board on surge ticks, 716 cm/s peak anywhere in the field.
	// 0 disables the scaling (fixed ceiling).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|WaveMass", meta=(ClampMin="0.0"))
	float propulsionCeilingRefFlow = 250.0f;

	// Clamps the wave-energy scaling so a near-still or freak-fast sample cannot collapse or explode
	// the ceiling. Applied symmetrically: scale is clamped to [1/x, x].
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|WaveMass", meta=(ClampMin="1.0"))
	float propulsionCeilingFlowClamp = 3.0f;

	// Decouples the wave-mass FLOW drag from roll by relocating its application point from the actor toward
	// the board forward axis through the CoM (removes the lateral+vertical moment arms → no roll/keel,
	// keeps the front/back pitch). 0 = legacy (at actor); 1 = on the CoM longitudinal axis (fix).
	// See specs/wave-mass-drag-torque-decoupling.md.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|WaveMass", meta=(ClampMin="0.0", ClampMax="1.0"))
	float waveMassRollDecouple = 1.0f;

	// Decouples the wave-mass FLOW drag from YAW (on top of the roll decouple). Its yaw (r_fwd·F_left)
	// survives the roll relocation and is the DOMINANT into-the-wave steering torque down the line
	// (dragBottom_waveMassFlow + dragRailFlow ~= -64k net yaw, measured), turning the nose up the face at
	// centred weight. Strips the board.up (yaw) component of the applied angular impulse while keeping the
	// nose-up-into-the-face PITCH (spec option C). 0 = legacy option B (keep yaw); 1 = strip yaw (fix).
	// See specs/wave-mass-drag-torque-decoupling.md.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|WaveMass", meta=(ClampMin="0.0", ClampMax="1.0"))
	float waveMassFlowYawDecouple = 1.0f;

	// Same option-C yaw strip for the wave-PENETRATION "wall" drag (glide-through resistance), whose yaw was
	// the #1 into-the-wave steering torque remaining after the flow decouple. Keeps the linear wall + pitch,
	// drops roll+yaw. Separate from the flow knob so it can be A/B'd independently. 0 = legacy; 1 = strip.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|WaveMass", meta=(ClampMin="0.0", ClampMax="1.0"))
	float wavePenetrationYawDecouple = 1.0f;

	// Low-pass time constant (s) for the wave-mass FLOW push (EMA, alpha = dt/tau on the applied force).
	// The raw push is a violent flickering impulse; alone this doesn't fix the down-the-line surge, but ON
	// TOP OF the yaw-thrust speed attenuation it further smooths the flow spikes (measured 3×: peak
	// ~1002→872, decel ~-1383→-800). 0 = off. See specs/yaw-thrust-speed-attenuation.md.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|WaveMass", meta=(ClampMin="0.0"))
	float waveMassSmoothingTau = 0.2f;

	// 0..1: blend the CONTACT wave forces (waveMassFlowDrag, rail flowDrag, wavePenetration) from the
	// board-wide SC water velocity (0, legacy) toward each FluidDynamics actor's OWN sample (1) — so
	// the nose feels the lip's 500-700 cm/s core instead of the board-midline fringe (~4-7x on v²).
	// Default 1 (player-validated 2026-07-22): full per-actor. waveMassThrust and the
	// lift/relative-velocity family stay board-wide by design. See specs/per-actor-water-velocity.md.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|WaveMass", meta=(ClampMin="0.0", ClampMax="1.0"))
	float PerActorWaterVelocityBlend = 1.0f;

	// Synthesized lip impact (nose actor): when the nose is under the curl (near the crest of a steep
	// breaking section) the airborne sheet's momentum isn't in the column data — sample the LANDING band
	// shoreward of the nose for the jet's true velocity and apply F = coef × jetSpeed² along the jet's
	// direction (shoreward+down), pitch-kept/roll-yaw-stripped at the nose arm. 0 = OFF.
	// 1.0 (player-tuned 2026-07-22, with the 1M cap): F = jetSpeed² — a 600 cm/s jet hits with 360k,
	// several times board weight. The v² model makes fast jets escalate hard. See specs/lip-impact.md.
	// Halved 1.0 → 0.5 (player-tuned 2026-07-23, alongside the flat-face gate revert): full-strength
	// hits landed too hard with the re-raised slope gates / 1000 supplement cap physics.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|WaveMass", meta=(ClampMin="0.0"))
	float LipImpactCoefficient = 0.5f;

	// Probe spacing anchor (cm): the landing band is sampled at {0.5, 1.0, 1.5}× this, shoreward of the nose.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|WaveMass", meta=(ClampMin="0.0"))
	float LipImpactBandOffset = 150.0f;

	// Band speed (cm/s) below which no curl is considered overhead (smoothstep onset to ×1.3).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|WaveMass", meta=(ClampMin="0.0"))
	float LipImpactMinJetSpeed = 350.0f;

	// Breaking-section gate: smoothstep(min, min+0.08, boardWideSlopeSin).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|WaveMass", meta=(ClampMin="0.0"))
	float LipImpactMinSlopeSin = 0.30f;

	// Crest-proximity gate width (cm): full at the crest, fading linearly to 0 at this |distance|.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|WaveMass", meta=(ClampMin="1.0"))
	float LipImpactCrestRange = 300.0f;

	// Hard cap on the synthesized impact force (player-tuned 2026-07-22: ~10× board weight — the
	// coefficient-1.0 v² model needs the headroom; the per-axis velocity ceilings backstop runaways).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|WaveMass", meta=(ClampMin="0.0"))
	float LipImpactMaxForce = 1000000.0f;

	// Wave penetration resistance — "wall" drag along the wave's horizontal normal that resists the
	// board crossing the face, keeping it ON the face instead of charging through to the back.
	// coef × effectiveWaterHeight × vAcrossFace². Starting guess; tune live. See wave-penetration-resistance.md.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|WaveMass")
	float wavePenetrationCoefficient = 0.0f;

	// Deadzone (cm/s): only cross-face speed above this is resisted, so normal riding isn't braked
	// (NFR1) and the coefficient can be strong at a fast punch-through. See wave-penetration-resistance.md.
	// Raised 150 → 400 (2026-07-22): the registration fix gave the flats real seaward return-flow, so
	// a planing glide reads ~450-500 cm/s of closing speed and the old deadzone let the wall brake
	// normal riding at ~-11k (measured: >half the flat-glide deceleration). 400 keeps the glide free
	// while a genuine fast punch-through (600+) still hits the v²-above-threshold wall.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|WaveMass")
	float wavePenetrationThreshold = 400.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|WaveMass")
	float baseHeight = 10.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|WaveMass")
	float slopeHeight = 1000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|WaveMass")
	float maxEffectiveWaterHeight = 200.0f;

	// --- Planing (ASharedCalculations state machine; see calculateAmountPlaning).
	// Planing keys on the board's absolute world-frame speed (cm/s) — NOT depth
	// or relative water velocity. Defaults lowered from the old per-actor values
	// (starts 400 / stops 300 / full 1000) so planing engages at realistic
	// ride speeds. ---

	// Board speed (cm/s) required to start planing.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Planing")
	float PlaningStartsVelocity = 300.0f;

	// Hysteresis: once planing, the board keeps planing until speed drops below this.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Planing")
	float PlaningStopsVelocity = 200.0f;

	// Board speed (cm/s) at which AmountPlaning reaches MaxPlaning.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Planing")
	float PlaningFullVelocity = 400.0f;

	// Ceiling for AmountPlaning (0..1). Lowered from the old per-actor 0.9.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Planing", meta=(ClampMin="0.0", ClampMax="1.0"))
	float MaxPlaning = 0.75f;

	// --- Pump (rider's downward impulse; see specs/pumping.md). Phase 0 uses
	// TestPumpInput as a constant override to validate the mechanism before
	// the accelerometer pipeline lands. ---

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Pump")
	float MaxPumpForce = 200000.0f;     // N; full-strength impulse magnitude (raised from 100000, 2026-06-13)

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Pump")
	float TestPumpInput = 0.0f;         // 0..1; non-zero overrides player input

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Pump")
	float PumpForwardOffset = 0.0f;     // cm along board.forwards from COM (negative = behind)

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Pump")
	float PumpLateralOffset = -1.5f;    // cm along board's local +X (lateral). Use to compensate for an asymmetric centerOfMassOffset that causes pumping to bias roll.

	// Drive the pump through the ENGAGED RAIL rather than the centreline: cm of lateral offset at
	// full bank, applied toward whichever rail is currently lower and scaled by how far the board is
	// banked, so a flat board pumps down its centreline and a committed one pumps through its rail.
	//
	// This is what makes a pump turn the board harder, and it does it the honest way. The off-centre
	// impulse is a roll torque as well as a downward push, so it banks the board further into the
	// turn; the carve reads world roll, so more bank is more carve. The rail also goes deeper, which
	// raises that side's wetting and effective water height, and the rail lift and yaw hydrofoil
	// both scale with those. Nothing here fakes a submersion value - the board really is driven
	// down on that side.
	//
	// 0 = pump straight down the centreline (the pre-2026-09-10 behaviour).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Pump")
	float PumpRailOffset = 25.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Pump")
	float PumpSlopeAttenuationStart = 0.05f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Pump")
	float PumpSlopeAttenuationEnd = 0.50f;

	// Speed-based attenuation. Pumping in real surfing saturates: at high board
	// speeds the rider's leg motion adds proportionally less acceleration
	// (drag scales with v², water moves past faster than the rider can react).
	// Below Start: full pump effectiveness. Above End: pump produces no force.
	// Linear smoothstep in between. Set End <= 0 to disable.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Pump")
	float PumpSpeedAttenuationStart = 300.0f;   // cm/s

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Pump")
	float PumpSpeedAttenuationEnd = 600.0f;     // cm/s

	// Speed-based RAMP-IN, the other side of the taper above. A pump converts speed the board
	// already has: the rider unweights and reloads against water that is flowing past the bottom,
	// and if it is not flowing there is nothing to push against. Below Start the pump does
	// nothing; full by End. Measured 2026-09-12 on PC: with no ramp-in, a board stalled at
	// ~120 cm/s by a full-rail carve got the pump at full strength (the taper above gives 1.0
	// below 300), launched to ~520 cm/s in 0.3 s (1.4 g), and the carve bled it straight back to
	// 120 for the next one - the board pirouetted in a 2 m circle for 30 s, 96 m of path for 8 m
	// of net travel. Same relative-water-velocity key as the taper. Set End <= 0 to disable.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Pump")
	float PumpSpeedRampInStart = 100.0f;        // cm/s

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Pump")
	float PumpSpeedRampInEnd = 250.0f;          // cm/s

	// Per-actor ceiling on the pump-driven forward thrust of the bottom pitch hydrofoil, at full
	// pump input; the ceiling scales down with the pump gate, so a half pump is a half launch.
	// Force units (1 g on the board is ~100 000).
	//
	// Why a separate ceiling: the raw v²·sinAOA forward term runs 5-300x over the general
	// maxHydrofoilForceAmount (20 000) on every pump, so every bottom actor pinned at that cap
	// and the launch was always cap x actor count, ~2 g, whatever the pump input. Combined with
	// the gate saturating at 2.5 % input (see FluidDynamics) the pump was a switch on a 2 g
	// thruster, not a graded push. 6 000 x ~10 bottom actors = ~0.6 g at a full pump.
	// 0 = no pump-specific ceiling (only maxHydrofoilForceAmount applies, the pre-2026-09-12 behaviour).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Pump")
	float PumpForwardForceCap = 6000.0f;

	// Phase 1 — accelerometer pipeline tuning (see ASurfboardPawn::UpdatePumpInput).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Pump")
	float PumpAccelForFull = 1.0f;       // m/s² along gravity that maps to PumpInput = 1.0 (lowered from 7.0, 2026-06-13)

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Pump")
	float PumpDeadzone = 0.1f;           // m/s²; below this, ignore as hand-jitter (dialed in on Android 2026-06-11)

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Pump")
	float PumpLowPassHz = 10.0f;         // low-pass cutoff applied to A_along_gravity

	// --- Wave crest scan (ASharedCalculations::signedDistanceToCrest) ---
	//
	// NOT assist-only: signedDistanceToCrest also gates PlaningRedirectCrestFade, CarveGripCrestFade
	// and lipImpact. Those were tuned against the LEGACY scan, which measurement showed sets off
	// shoreward on ~60% of ticks and latches onto leftover whitewater — reporting the board as
	// "behind the crest" while it rides the face, which slams those gates shut. Making the scan
	// correct therefore CHANGES RIDE FEEL, so it is switchable for a clean A/B.

	/** 1 = symmetric bounded sweep, nearest significant peak wins (default). 0 = the legacy
	 *  one-comparison hill-climb. Flip to 0 to get exactly the old ride back. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Assist")
	float CrestScanRobust = 1.0f;

	/** How far either way the robust scan looks, cm. A crest further off than this is not the wave
	 *  being ridden. The legacy scan ran to 3000. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Assist")
	float CrestScanRangeCm = 1200.0f;

	// --- Gradual control handoff (specs/gradual-control-handoff.md) ---
	//
	// Dev switches first. Credit is session-only, so every playtest otherwise opens at alpha 1 on an
	// assisted board — which quietly corrupts any OTHER thing being tuned or eyeballed in that
	// session. These two are the in-game way to get the assist out of the way; `surf.assist.enabled`
	// and `surf.assist.alpha` do the same from a console, which a phone does not have.
	//
	// Both default to 0 = "ship behaviour", so the HUD's Reset button on either row always returns
	// the assist to normal. (The HUD's slider spans 0..4x the default, which collapses to a plain
	// 0..1 slider when the default is 0 — hence the inverted sense of AssistDisable: a toggle needs
	// the usable range, and "0 = normal" is the property worth keeping.)

	/** 0 = assist active (ship behaviour). >= 0.5 = assist fully off, board behaves as it does for a
	 *  graduated player. Flip this before tuning anything else. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Assist")
	float AssistDisable = 0.0f;

	/** 0 = follow the ride-time fade schedule (ship behaviour). > 0 = pin the authority ceiling at
	 *  this value, so a specific assist strength can be felt without riding to it. Use AssistDisable
	 *  for the alpha = 0 case. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Assist")
	float AssistAlphaForce = 0.0f;

	/** 0 = off. >= 0.5 = draw the two edges of the no-assist band on the wave face, plus the board's
	 *  live distance-to-crest. Lets a playtest separate "the band is in the wrong place" from "the
	 *  controller is misbehaving" — indistinguishable from the deck otherwise. Drawn by
	 *  ASharedCalculations, coloured by the same SurfAssist::BandError the controller gates on. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Assist")
	float AssistDrawBand = 0.0f;

	// Skip the "A helping hand" first-play card in PIE. 0 = show it (default, and what a player gets).
	//
	// Purely a development convenience: the card pauses the world until it is dismissed, so with an
	// editor that gets restarted many times a day it is a click between every single test run. It is
	// deliberately NOT a general "never show the card" switch - the card is a real requirement (FR7
	// of specs/gradual-control-handoff.md) and it is the one surface that tells a first-time player
	// what the assist is.
	//
	// PIE only, checked against EWorldType::PIE. Standalone -game runs launched from the editor, which
	// is what Screenshot.ps1 and RunGameAndCollectLogs use, are unaffected and still see what a player
	// sees. Widening it is a one-line change if that turns out to be wanted.
	//
	// Lives here rather than as a CVar because Saved/TuningOverrides.json survives an editor restart:
	// set it once and it stays set, which is the entire point. A CVar would have to be retyped every
	// time the editor comes back, which is the problem, not the fix.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Assist")
	float AssistSkipFirstPlayCardInPIE = 0.0f;

	// --- Start screen ---

	/** Skip the pre-wave Start screen and drop straight into the ride. A development convenience:
	 *  verifying anything about the RIDE otherwise costs a click on every single launch, which is
	 *  enough friction to stop things being checked at all.
	 *
	 *  Unlike AssistSkipFirstPlayCardInPIE this is deliberately NOT PIE-only, because the runs that
	 *  most need it are exactly the ones PIE excludes — Screenshot.ps1 launches standalone -game, and
	 *  a menu it cannot dismiss makes the ride unphotographable.
	 *
	 *  Lives here rather than on a CVar for the same reason as the assist skip:
	 *  Saved/TuningOverrides.json survives an editor restart, so it is set once and stays set. That
	 *  file is not shipped, so a stray 1 cannot reach a player.
	 *
	 *  Does not touch the tutorial: "Back to instructions" still opens it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Start Screen")
	float StartScreenSkip = 0.0f;

	/** Dev switch: run the intro on PHYSICS (the live AStateTriggerAutoPilot) instead of replaying
	 *  the recorded pose track named by the pawn's RailsIntroTrace. >= 0.5 = rails off.
	 *
	 *  The rails intro is a POSE RECORDING, so it is frozen at the physics of the day it was made
	 *  (Content/InputTraces/intro-reference.csv: 2026-08-25, when the board rode 30-50 cm under).
	 *  Every ride-height improvement since is invisible in it, which is what makes the takeoff look
	 *  buried against wave meshes that also sit ~40 cm below the height data. This is the switch for
	 *  checking whether the CURRENT physics does the takeoff better before committing to re-record.
	 *
	 *  Same reasoning as StartScreenSkip for living here rather than on a CVar: the decision is made
	 *  in the pawn's BeginPlay, long before a console exists, and Saved/TuningOverrides.json survives
	 *  an editor restart so it is set once and stays set. That file is not shipped.
	 *
	 *  Needs an AStateTriggerAutoPilot in the level, enabled and wired to the pawn's
	 *  StateTriggerAutoPilot — with rails off and no autopilot there is no takeoff at all.
	 *  See specs/deterministic-ride-handoff.md. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Start Screen")
	float RailsDisable = 0.0f;

	/** Dev switch: capture this run's intro as the next rails source trace, exactly as -RecordIntro
	 *  does. >= 0.5 = record, writing Saved/InputTraces/<session>.csv from BeginPlay to handoff.
	 *
	 *  Implies RailsDisable — recording a rails run would only re-record the track it is already
	 *  following — so this one switch both shows the physics takeoff and captures it. To promote the
	 *  result, copy it to Content/InputTraces/ and point RailsIntroTrace at it; Content/ is the only
	 *  root that survives packaging, and InputTraces is already in DirectoriesToAlwaysStageAsUFS.
	 *
	 *  For a fixed window instead of BeginPlay-to-handoff there is still only the command line
	 *  (-RecordIntroSeconds=N); this switch takes the default, which is what re-recording an intro
	 *  wants. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Start Screen")
	float RecordIntro = 0.0f;

	/** How long each drawn band line lives, seconds. The overlay is redrawn every frame, so this is
	 *  really flicker insurance: long enough to outlive a frame hitch, short enough that the lines
	 *  do not smear as the wave carries the band along.
	 *
	 *  It is also the screenshot knob. Pausing stops ASharedCalculations ticking, so the redraw
	 *  stops and whatever is on screen expires — at the default you get about a third of a second
	 *  after pausing before the band vanishes. Wind this up to a few seconds to line a shot up, then
	 *  put it back: at 60fps a long lifetime means dozens of overlapping copies trailing behind the
	 *  moving band. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Assist")
	float AssistDrawBandSeconds = 0.3f;

	// Mirrors SurfAssist::FTuning field for field; ASurfboardPawn copies these across each tick, so
	// the whole feel of the assist is dialled in live through Saved/TuningOverrides.json. Defaults
	// here must stay in step with the struct's — that is the fallback when no subsystem exists.

	// FR2 — the guard band, in signedDistanceToCrest units (cm; negative = on the front face).
	// Narrowed 40/500 → 100/200 (live-tuned on PC 2026-08-26, baked from Saved/TuningOverrides.json):
	// a tighter band, held further off the crest. Keep in step with SurfAssist::FTuning.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Assist")
	float AssistBandNear = 100.0f;       // closest to the crest before the "out the back" edge bites

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Assist")
	float AssistBandFar = 200.0f;        // furthest down the face before the "into the flats" edge bites

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Assist")
	float AssistBandSoftness = 150.0f;   // cm over which the guard ramps to full — no step at the edge

	// FR3 — the position -> heading -> weight cascade.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Assist")
	float AssistMaxHeadingSin = 0.5f;    // largest heading offset the outer loop asks for (~30 deg)

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Assist")
	float AssistHeadingP = 0.6f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Assist")
	float AssistHeadingD = 0.03f;        // without this the loop weaves: weight commands a turn RATE

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Assist")
	float AssistHeadingRateSmoothingSeconds = 0.15f;  // low-pass before the D term; raw rate went bang-bang

	// FR1 — authority clamp, in weight units (player range is 0..1).
	//
	// 10 is player-tuned on device (2026-08-27) and is NOT ten units of authority: at this value the
	// clamp never binds, so the real ceiling becomes whatever the PD terms produce — about 0.3 for
	// steering (HeadingP 0.6 x MaxHeadingSin 0.5, plus the D term) and up to 0.35 for trim
	// (TrimP 0.5 x the widest target error). The first guess of 0.15 was cutting the controller off
	// well below its own output, which is why it felt weak.
	//
	// Consequence worth knowing: FR1's guarantee that a committed player input can never be
	// overpowered used to rest on THIS clamp. It now rests on the PD gains instead. Setting this to
	// ~0.4 would restore the clamp as a real backstop while binding rarely if ever — worth an A/B
	// against 10 to confirm they feel the same.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Assist")
	float AssistMaxAuthority = 10.0f;

	// Diagnosis amplifiers. 1.0 = the shipping controller, bit for bit. Leave them there.
	//
	// They exist for one question the ride could not answer: does the guard push the RIGHT WAY? Its
	// correction is small on purpose, it only speaks at the band edges, and from the deck "the assist
	// caught me" and "the wave caught me" look the same. So a sign error could live here forever.
	//
	// Wind one up and the channel goes bang-bang: the correction swamps the entire 0..1 weight range,
	// so the board is pinned to full lean for as long as the guard is speaking and player input
	// cannot fight it. Then ride out the back on purpose. The board either carves hard back down the
	// line — sign right — or drives itself off the wave within a second or two, in which case set
	// AssistSteerSign to -1 and repeat.
	//
	// The HUD slider spans 0..4x default, which for a default of 1.0 is already past saturation
	// (steering peaks near 0.3 of correction, and 0.5 is full deflection from centre), so the slider
	// alone reaches the extreme — no typing on a phone keyboard. The numeric box goes further if a
	// headless capture wants the correction unmistakably railed.
	//
	// These scale the AssistMaxAuthority clamp with them, so cranking one is not silently capped.
	// That deliberately suspends FR1's "assist can never overpower the player" guarantee: it is a
	// measuring instrument, not a feel knob.
	//
	// 0.0 mutes a channel, which is the other half of the tool. Both corrections drive one board, and
	// an amplified trim term slams the nose (specs/nose-dive-bug.md) before the steering question gets
	// an answer. Prove one channel at a time: AssistTrimGain 0 while testing steering, and the
	// reverse.
	//
	// Two more switches make the picture readable while these are up:
	//   AssistAlphaForce 1  — pin full authority instead of riding to it (credit is session-only)
	//   AssistDrawBand 1    — draw the band being enforced, so "wrong direction" and "wrong band"
	//                         cannot be confused for each other
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Assist")
	float AssistSteerGain = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Assist")
	float AssistTrimGain = 1.0f;

	// FR4 — trim.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Assist")
	float AssistTrimNeutral = 0.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Assist")
	float AssistTrimSlopeGain = 0.20f;   // steeper face -> more tail

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Assist")
	float AssistTrimSpeedRef = 1400.0f;  // cm/s

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Assist")
	float AssistTrimSpeedGain = 0.10f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Assist")
	float AssistTrimTargetMin = 0.30f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Assist")
	float AssistTrimTargetMax = 0.65f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Assist")
	float AssistTrimP = 0.50f;

	// Pumping is a DELIBERATE fore/aft input, and the trim term exists to correct neglect, not to
	// cancel intent. Left alone it read the pump swing as error and flattened it (device report
	// 2026-08-26: pitch stopped oscillating while pumping). Above the threshold the fore/aft assist
	// stands down, and stays down for the release window so it cannot snap back between strokes.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Assist")
	float AssistPumpInputThreshold = 0.15f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Assist")
	float AssistPumpReleaseSeconds = 0.7f;

	// FR5 — rate limit on the player's STEERING while assist is strong. A rate limit and NOT a gain
	// reduction: reduced gain makes the board feel dead and misrepresents how it will handle later.
	// Fore/aft is deliberately NOT limited: pumping lives there and is fast by definition, and at 1.5
	// this removed the pitch oscillation from pumping entirely (device, 2026-08-26).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Assist")
	float AssistRateLimitAtFullAssist = 1.5f;   // steering weight units per second at alpha 1

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Assist")
	float AssistRateLimitAtNoAssist = 1000.0f;  // effectively unlimited at alpha 0

	// FR6 — the fade schedule, in seconds of accumulated ride credit.
	//
	// Compressed ~6x from 30/90/180 to 10/20/30 (2026-08-30), promoted from the PC
	// TuningOverrides.json after playing against it. The original schedule was a guess; this one
	// hands the board over inside the first minute, which is where it was found to belong.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Assist")
	float AssistCreditFullSeconds = 10.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Assist")
	float AssistCreditMidSeconds = 20.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Assist")
	float AssistCreditZeroSeconds = 30.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Assist")
	float AssistAlphaAtMid = 0.4f;

	// Seconds spent being shepherded count for less: they were earned by the assist, not the player.
	// Not zero, so a player who never quite holds it alone still graduates — four times slower.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Assist")
	float AssistAssistedCreditRate = 0.25f;

	// RETIRED to 1.0 on 2026-09-18 (specs/trick-scoring.md FR12): the score no longer looks at
	// whitewater. The factor lerp(1, this, ASharedCalculations::brokenAmount) is still in the credit
	// product, so a future broken-water signal can price the foam again by lowering this - but
	// brokenAmount is not that signal. It reads the foam point cloud NEAR the board, never the board
	// IN broken water: bisected 2026-09-17, it sat at 1.0 for 1.5 s on a clean face because a turn
	// pointed the nose at the lip's foam 6 m ahead (memory a2-drive-cut-fires-on-clean-face, spec
	// M13). Turns are exactly when the trick window pays, so at 0.25 it docked the hard rides the
	// score exists to reward. The owner retired the same signal from the hydrofoil drive cut for the
	// same false positives, and ruled the score should ignore whitewater too.
	// 0.25 = the pre-2026-09-18 behaviour (whitewater at the assist's out-of-pocket price), for A/B.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Assist", meta=(ClampMin="0.0", ClampMax="1.0"))
	float ScoreWhitewaterCreditRate = 1.0f;

	// specs/ride-score-counter.md FR2 — speed multiplies the credit rate, so riding fast unassisted
	// earns faster than drifting unassisted. Chosen over trick scoring (D2 there) because it does not
	// change what graduation means: riding faster is still surviving, only harder.
	//
	// Keep the ceiling small. A large multiplier turns the fade into a speed-run, which reopens the
	// question D2 closed. Set AssistSpeedCreditMaxMult to 1 to disable the multiplier outright.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Assist")
	float AssistSpeedCreditRefLow = 1400.0f;    // cm/s at or below which the multiplier is 1

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Assist")
	float AssistSpeedCreditRefHigh = 2600.0f;   // cm/s at or above which it reaches the ceiling

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Assist")
	float AssistSpeedCreditMaxMult = 2.0f;

	// Escape hatch if the guard is found pushing the wrong way on device: -1 flips it. The sign is
	// derived at runtime from board and wave vectors, so this should never be needed.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Assist")
	float AssistSteerSign = 1.0f;

	// --- Stamina (specs/stamina.md) ---
	//
	// Every ride is a run with an energy budget. The infinite wave never ends and the foamie's
	// assist makes falling rare, so without this a ride could go on indefinitely - past the trace
	// recorder's 120 s cap, and long enough that "best score" measured patience. One pool per ride,
	// drained by riding at all (slowly), by pumping, and by hard turns; refilled by resting WHILE
	// tired. Empty = TIRED, a state rather than an ending: the assist goes off, the tired tuning layer
	// (Saved/TiredTuning.json, see SetTired) goes on, the rider's anim flag flips - and the ride
	// then ends the way rides end, by losing the wave or falling, or the rider rests and recovers.
	// This block writes nothing into the physics; what tired does to the board is entirely the
	// layer's contents, which nobody knows yet and which are meant to be found on device.
	//
	// The three drains are additive, so pumping THROUGH a hard turn is the most expensive thing in
	// the game - which is also the most speed-producing thing in it. With the defaults: cruising
	// alone lasts 120 s (90 until 2026-09-22; owner), continuous pumping ~25 s, and a full-size turn (120 deg at ~80 deg/s) costs
	// ~8 % of the pool; resting refills from empty to the exit fraction in ~10 s. All first-cut
	// guesses; tune on device from the end-of-ride log line, which breaks the spend down by source.

	/** Master switch. 0 turns the feature off entirely (no bar, no drain, no ending). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Stamina")
	float StaminaEnabled = 1.0f;

	/** A full pool lasts this long just riding, no pump, no hard turn. 0 = riding costs nothing and
	 *  only effort spends the pool (spec D1's alternative - a passive ride then never ends). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Stamina")
	float StaminaPassiveRideSeconds = 120.0f;

	/** Pool per second while a pump stroke is running - the gesture as the player makes it
	 *  (bPumpActive), NOT the attenuated force, which is zero most of the time and let a tired
	 *  rider pump while "resting". 0.04 = ~25 s of continuous pumping. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Stamina")
	float StaminaPumpCostPerSecond = 0.04f;

	/** Smoothed wave-relative heading rate (deg/s) below which turning is free. Same number as
	 *  TrickTurnEntryRateDeg on purpose: the score and the cost must agree about what "hard" means. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Stamina")
	float StaminaTurnFreeRateDeg = 35.0f;

	/** Pool per degree of heading change above the free rate. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Stamina")
	float StaminaTurnCostPerDegree = 0.0012f;

	/** Below this fraction the bar turns warm: "spend it or lose it". */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Stamina")
	float StaminaLowFraction = 0.2f;

	/** While TIRED and resting (no pump, no hard turn), the pool refills from empty in this long.
	 *  Only while tired: a refill during normal riding that outpaced the passive drain would net
	 *  positive for a cruiser, who would then never tire - the unbounded ride again. The passive
	 *  drain still runs underneath, so at 90 / 25 the 0.3 exit is ~10 s of rest. Must beat the
	 *  passive drain or a tired rider never recovers. 0 = no recovery: tired is for the rest of the
	 *  ride. The foamie profile sets 0 (owner, 2026-09-22): on the easiest board a tired rider who
	 *  just surfed straight waited the spell out and rode on for ever; see StaminaTiredGuardEverywhere. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Stamina")
	float StaminaRecoverySeconds = 25.0f;

	/** Tired clears once the pool is back to this fraction. Hysteresis: a real breath, not a
	 *  flicker at the zero line. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Stamina")
	float StaminaTiredExitFraction = 0.3f;

	// --- What tired does: REVERSED assist (2026-09-15) ---
	//
	// The first idea - a tired tuning layer that dulls the board - failed on device: every value
	// tried made the board EASIER. This game's difficulty is over-responsiveness (a small weight
	// shift over-turns), so a less responsive board is a more forgiving one, and "sloppy" cannot be
	// the tired feel. Owner: "the one thing that makes surfing easier is assist, the one thing that
	// can make surfing harder during out of stamina is REVERSED assist."
	//
	// Why it is genuinely hard and still not a kill: the steering guard is a PD controller on
	// heading error that engages only once the board leaves the pocket band. Reversed, that is
	// positive feedback - inside the band nothing happens, any excursion is amplified, the further
	// out the harder the push. Balancing a stick. But the correction is ADDITIVE and clamped by
	// AssistMaxAuthority, so the player's input always works; they are fighting a bounded push,
	// not losing the controls. The trim channel is switched OFF while tired rather than reversed:
	// reversed trim pushes toward nose-dive / stall, which is a hard kill.

	/** Assist strength while tired. Forced up from whatever the board rides at - most boards ride
	 *  at 0 - so the reversed guard bites on every board. 0 = no reversed assist (tired then only
	 *  removes the normal assist and applies the tuning layer). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Stamina")
	float StaminaTiredAssistAlpha = 1.0f;

	/** Multiplier on AssistSteerGain while tired - scales the reversed push AND its ceiling. 1 =
	 *  exactly as strong as the normal guard, pushed the other way. The difficulty knob. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Stamina")
	float StaminaTiredSteerGain = 1.0f;

	/** Multiplier on AssistTrimGain while tired. 0 = trim help off (the rider is on their own fore
	 *  and aft); never make it negative - reversed trim is a nose-dive. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Stamina")
	float StaminaTiredTrimGain = 0.0f;

	/** 1 = while tired the reversed guard works in EVERY part of the wave, not only outside the
	 *  pocket. The guard's band collapses onto the crest, so the whole face reads "too far down"
	 *  and the reversed push points down the face from everywhere on it, at full strength from
	 *  AssistBandSoftness below the crest: a tired rider is pulled off the wave unless they hold
	 *  against it the whole time. 0 = the band as authored: the reversed guard only speaks OUTSIDE
	 *  the pocket, so a rider who holds the pocket and makes no big movement is never pushed - and
	 *  on the foamie that rider surfed for ever, tired or not, until the pool refilled (owner,
	 *  2026-09-22: "too easy surfing forever"). The foamie profile sets 1, with
	 *  StaminaRecoverySeconds 0. The score's pocket is the authored band regardless
	 *  (SurfAssist::FOutput::bOutsideBand): tired changes what the board does, never what a second
	 *  in the pocket is worth. Strength is StaminaTiredSteerGain, as for the edge push. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Stamina")
	float StaminaTiredGuardEverywhere = 0.0f;

	/** Dev switch: 1 = the rider is tired NOW and stays tired regardless of the pool, 0 = normal.
	 *  Flip it in the TUNE panel mid-ride to sit in the tired state and find the tired values
	 *  without riding 90 s to earn each spell (owner: "it's gonna be tiresome waiting to become
	 *  tired to tune the values"). While it is on, slider edits go to Saved/TiredTuning.json like
	 *  in a real spell. Stamina's own knobs - this one included - are never written to that file,
	 *  or the switch would save itself as a tired value and every future spell would pin itself. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Stamina")
	float StaminaForceTired = 0.0f;

	// --- Trick scoring (specs/trick-scoring.md) ---
	//
	// Turns count, not just survival - and a turn is paid over the seconds AFTER it rather than at
	// its exit, because a hard turn followed by a fall is the easy half. Defaults are the first-cut
	// values measured 2026-09-11 from 89 s of real riding across eight device traces.
	//
	// The two headline numbers INTERACT and must be tuned together: how much of a ride the earning
	// window sits open is a function of how easily a turn qualifies, not of the window length alone.
	// At sweep 60 / window 4.0 the window is open ~18% of a ride. Drop sweep to 50 and it is ~47%.
	// Below sweep ~40 it is open two thirds of the ride and the multiplier has become the baseline.

	/** Smoothed wave-relative heading rate, deg/s, that opens a turn event. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Tricks")
	float TrickTurnEntryRateDeg = 35.0f;

	/** Rate the arc must fall below to close. Hysteresis: a held turn must not chatter into events. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Tricks")
	float TrickTurnExitRateDeg = 14.0f;

	/** Minimum arc duration to score at all. Measured median turn runs ~1.0 s. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Tricks")
	float TrickTurnMinSeconds = 0.35f;

	/** Minimum wave-relative heading change, degrees. THE knob for how often anything scores. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Tricks")
	float TrickTurnMinSweepDeg = 60.0f;

	/** Low-pass on heading rate. Matches the assist's, deliberately - one opinion about the motion. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Tricks")
	float TrickHeadingRateSmoothingSeconds = 0.15f;

	/** Discard an arc whose steering guard was active for more than this fraction of it (FR5). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Tricks")
	float TrickTurnGuardDiscardFraction = 0.5f;

	/** Sweep counting as a full-size turn; beyond this, size stops adding. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Tricks")
	float TrickGradeRefSweepDeg = 120.0f;

	/** Floors on the bite (rail engaged) and drive (speed kept) grade factors. At these values a
	 *  clean carve grades about twice a slide through the same heading change. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Tricks")
	float TrickGradeBiteFloor = 0.40f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Tricks")
	float TrickGradeDriveFloor = 0.40f;

	/** Grade at or above which the burst fires (the snap fires on every scored turn). The burst earns
	 *  its impact from rarity - a bar a competent rider clears twice a wave makes it an obstruction -
	 *  but a bar nobody clears is not rare, it is absent.
	 *
	 *  0.65, down from 0.80, after the first device session: the sixteen turns that scored graded
	 *  0.32-0.86 with a median near 0.55, and exactly one cleared 0.80. The rider's own pick for
	 *  "sharp enough to celebrate" was a 144-degree snap that would have graded around 0.78. At 0.65
	 *  roughly the top third of scored turns burst. Live-tunable through the tuning HUD and
	 *  Saved/TuningOverrides.json. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Tricks")
	float TrickBigTurnGrade = 0.65f;

	// The four window numbers below were re-sized together on 2026-09-18 (specs/trick-scoring.md D9)
	// from a device ride the owner rated "at least 500-600": a hard cutback, two linked turns, then
	// 24 s more surfing, which scored 156 against 300+ for riding straight. Replayed, the three turns
	// chained to the old 3.0x cap and the whole chain paid 2.1 credit-seconds (37 points): the window
	// ran at 4.0 s / peak 2.5 / cap 3.0, so even in band, at full grade, a chain topped out near
	// 4 credit-seconds - a tenth of a straight ride. Sized now so one isolated big turn is worth
	// roughly 15 s of straight riding and three linked turns roughly a whole ride's survival term.

	/** How long the earning window runs after a turn's exit. 6 s (was 4): the recovery from a big
	 *  turn has to fit inside it or the turn is unpayable (D6). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Tricks")
	float TrickWindowSeconds = 6.0f;

	/** Earn-rate multiplier at the exit, at full grade, decaying linearly to 1 across the window. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Tricks")
	float TrickWindowPeakMultiplier = 4.0f;

	/** Credit-seconds paid at the exit itself, scaled by grade, so the counter visibly moves on
	 *  every scored turn. With TrickWindowPeakMultiplier this sets the exit-vs-window split; at
	 *  2.0 / 4.0 a big turn pays ~20% at the exit and ~80% through its window (D5's start number). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Tricks")
	float TrickTurnExitLumpCredit = 2.0f;

	/** Ceiling on the window multiplier however long a chain runs. Not optional: a hard turn is
	 *  already paid in speed, and speed already multiplies the credit rate. Two big turns reach it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Tricks")
	float TrickChainMaxMultiplier = 6.0f;

	/** What the window's base rate does while the board is OUTSIDE the guard band. The survival
	 *  term pays AssistAssistedCreditRate (0.25) there and is untouched by this; only the base the
	 *  trick window multiplies uses this factor instead.
	 *
	 *  1.0 = the window ignores the band. Measured 2026-09-18: the band is 100-200 cm from the
	 *  crest, and any turn that sweeps 60+ degrees leaves it, so with the old shared 0.25 every real
	 *  turn's window ran on a quarter base and a 3x chain paid 0.75x the straight-line rate - the
	 *  player earned LESS per second during the celebration than riding straight. Whitewater still
	 *  docks the window (ScoreWhitewaterCreditRate); this is only the band factor.
	 *  0.25 = the pre-2026-09-18 behaviour (window and survival share one guard factor), for A/B. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Tricks", meta=(ClampMin="0.0", ClampMax="1.0"))
	float TrickWindowGuardRate = 1.0f;

	/** Per-board multiplier slope, 1 + K * (difficulty - 1). At 0.25 the foamie pays 1.00x and the
	 *  shortboard 2.00x. Never lets any board fall below 1.0. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Tricks")
	float TrickBoardDifficultyK = 0.25f;

	// --- Board shadow grounding (specs/board-shadow-grounding.md) ---
	//
	// Who casts the board's shadow, switchable live so it can be A/B'd on a phone through the tuning
	// HUD — there is no console on device, and the mobile renderer is where this needed testing.
	//   0 = the BOARD casts its own shadow, proxies cast nothing. No grounding correction, but it is
	//       an ordinary visible mesh casting an ordinary shadow — the fallback if a platform will not
	//       render hidden shadow casters.
	//   1 = the PROXY casts and the board does not (default). The grounding correction applies.
	//   2 = as 1, but the proxies are also DRAWN. If a shadow appears at 2 and not at 1, the platform
	//       is refusing to cast from hidden primitives and the answer is 0.
	// Anything else is treated as 1.
	//
	// DEFAULTS TO 0 (2026-08-31). At mode 1 the board threw no shadow at all while the rider did:
	// the board mesh is Nanite, static proxies get SetForceDisableNanite(true) to stop them drawing,
	// and something on that fallback path does not cast. The rider is skeletal, never Nanite, and was
	// fine throughout. Mode 0 costs nothing today because ShadowContactZBias is 90, which leaves the
	// grounding correction switched off anyway — so the proxy was contributing exactly nothing while
	// breaking the board's shadow. Switch back to 1 if the Nanite proxy path gets fixed AND a
	// negative contact bias makes grounding worth having.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Shadow")
	float ShadowMode = 0.0f;
	//
	// AShadowController casts the board's and rider's shadows from shadow-only proxy meshes pushed
	// DOWN by the board's measured clearance above the rendered water. A lower caster throws its
	// shadow closer to straight beneath the board, so more drop pulls the shadow IN.

	// THE knob for "the shadow sits too far from the board". NEGATIVE pulls it in; positive lets it
	// drift out. It nudges the contact plane down, which raises the measured clearance, which drops
	// the caster further. Also absorbs the constant offset between the hand-placed FluidDynamics
	// sampler positions and the visible hull they sit on.
	//
	// Applied BEFORE the clamp, so air still separates — but keep ShadowMaxGroundingOffset
	// comfortably above the drop this produces while riding, or riding saturates the clamp too and
	// air stops reading as air.
	//
	// 0 -> 90 (2026-08-30), promoted from the PC TuningOverrides.json after playing against it.
	// Note what 90 means: measured clearance runs about -10..+48 cm, so subtracting 90 puts it below
	// zero on effectively every tick, the clamp floors the drop at 0, and the caster sits exactly on
	// the board. The grounding correction is therefore OFF by default and the shadow is cast from the
	// board's true position. That is a defensible place to land — the board turned out not to be
	// levitating (see the spec's Status) — but it means the clamp machinery is dormant, and anyone
	// re-tuning this should start by going NEGATIVE, not by nudging 90.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Shadow")
	float ShadowContactZBias = 90.0f;

	// The declared line between rendering offset and real air (cm). Clearance below this is treated
	// as the rendering offset and fully corrected away (shadow plants); above it is treated as
	// genuine air and rendered honestly (shadow separates). Raise alongside a negative
	// ShadowContactZBias. Measured riding clearance never exceeded +48 cm.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Shadow")
	float ShadowMaxGroundingOffset = 60.0f;

	// Wave DATA surface -> RENDERED water mesh surface (cm). The display meshes sit ~40 cm below the
	// height data the physics samples. Verified by eye against the drawn contact plane; this is a
	// CALIBRATION, not a taste knob — tune ShadowContactZBias instead unless the waterline itself
	// has moved. Same quantity and value as ASprayController::sprayWaterlineZBias.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Shadow")
	float ShadowWaterlineZBias = -40.0f;

	// Exponential smoothing time constant (s) on the drop. Per-tick clearance is a min over ~20 hull
	// points against sampled wave data and is noisy; without this the clamp engaging at takeoff pops.
	// A shadow may lag a frame or two invisibly; it may not jitter. 0 disables smoothing.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Shadow")
	float ShadowOffsetSmoothingSeconds = 0.12f;

	// --- Lifecycle ---

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	// --- Reflection-based mutators for UMG row binding. Adding a new UPROPERTY
	// in category "Tuning|*" above automatically appears in GetAllPropertyNames
	// without touching this header. ---

	UFUNCTION(BlueprintCallable, Category="Tuning")
	float GetByName(FName PropertyName) const;

	UFUNCTION(BlueprintCallable, Category="Tuning")
	void SetByName(FName PropertyName, float NewValue);
	/** Same write as SetByName but NEVER saved: no dirty flag, no debounced save into
	 *  Saved/BoardTuning/<id>.json. For scripted, in-memory experiments (a replay flipping a
	 *  coefficient on one tick) that must not stick to the board. Broadcasts OnTuningChanged. */
	void SetTransient(FName PropertyName, float NewValue);

	UFUNCTION(BlueprintCallable, Category="Tuning")
	float GetDefault(FName PropertyName) const;

	UFUNCTION(BlueprintCallable, Category="Tuning")
	void ResetToDefault(FName PropertyName);

	UFUNCTION(BlueprintCallable, Category="Tuning")
	TArray<FName> GetAllPropertyNames() const;

	UFUNCTION(BlueprintCallable, Category="Tuning")
	FString GetCategoryForProperty(FName PropertyName) const;

	// --- Board profiles (specs/board-selection.md D2) ---

	/** Install a board's tuning block as the effective baseline, then re-apply the dev JSON on top.
	 *  Order is: compiled defaults -> this board -> Saved/TuningOverrides.json.
	 *
	 *  Idempotent by construction: every tunable is first restored to its compiled default, so
	 *  switching A -> B -> A lands back exactly on A rather than accumulating. That matters because
	 *  this subsystem lives on the GAME INSTANCE and survives the level reload a restart performs.
	 *
	 *  Unknown keys are logged and skipped, exactly as the JSON loader treats schema drift.
	 *
	 *  Pass an empty map to return to plain compiled-defaults-plus-JSON (what a test run wants). */
	void ApplyBoardBaseline(const TMap<FName, float>& BoardTuning, const FString& BoardId);

	/** Which board's overlay the HUD is currently writing to. Empty when no board is installed, in
	 *  which case edits fall back to the global file - the pre-boards behaviour. */
	UFUNCTION(BlueprintCallable, Category="Tuning")
	FString GetActiveBoardId() const { return ActiveBoardId; }

	/** The value a property returns to when "reset" — the board's value if this board sets it,
	 *  otherwise the compiled default. */
	float GetBaseline(FName PropertyName) const;

	// --- Tired layer (specs/stamina.md FR3) ---
	/** A fifth, TRANSIENT layer: Saved/TiredTuning.json, applied on top of the whole stack while
	 *  the rider is out of stamina and lifted again when they recover. It is how "tired" changes
	 *  the feel - slower weight response, weaker pumps, less turn authority, whatever the file says
	 *  - without a single physics branch anywhere: the coefficients simply read differently.
	 *
	 *  Never diffed into a board overlay: while it is on, HUD edits (SetByName) go INTO the tired
	 *  file instead of the board's, so the on-device loop is "get tired, open TUNE, move a slider,
	 *  it sticks as a tired value". That is the whole way these values are meant to be found -
	 *  nobody knows them yet. Re-read from disk on every activation, so a hand edit applies on the
	 *  next tired spell with no restart. */
	void SetTired(bool bOn);
	bool IsTired() const { return bTiredActive; }

	// Multicast delegate fired after any SetByName / ResetToDefault. Listeners
	// can drive per-system one-shot pushes (the SurfboardUtils damping CVar
	// path doesn't need this — it re-reads every Tick).
	DECLARE_MULTICAST_DELEGATE_TwoParams(FOnTuningChanged, FName /*PropertyName*/, float /*NewValue*/);
	FOnTuningChanged OnTuningChanged;

private:
	// FName -> compile-time default. Captured at the start of Initialize() before
	// any JSON overlay is applied. Used for slider scaling and reset-to-default.
	TMap<FName, float> Defaults;

	// The active board's tuning block, or empty when no board is applied. Overlaid on Defaults to
	// give the EFFECTIVE BASELINE that SaveToDisk diffs against.
	//
	// Without this, SaveToDisk's sparse "differs from compiled default" write would bake the whole
	// board profile into Saved/TuningOverrides.json the first time anyone nudged a slider - and
	// those values, now indistinguishable from a human's, would survive the next board switch and
	// silently pin every board to this one's numbers. See specs/board-selection.md D2.
	TMap<FName, float> BoardBaseline;

	// Layer 3: the GLOBAL overrides file, hand-edited and applied to every board. It is where the
	// dev switches live (StartScreenSkip, AssistSkipFirstPlayCardInPIE), not per-board feel.
	TMap<FName, float> GlobalBaseline;

	/** Id of the board whose overlay receives HUD edits. */
	FString ActiveBoardId;

	// FName -> "Tuning|Damping" / "Tuning|Lift" / ... Populated explicitly in
	// SnapshotDefaults because UPROPERTY metadata is editor-only and Android
	// non-editor builds strip FProperty::HasMetaData / GetMetaData.
	TMap<FName, FString> Categories;

	// Returns the FFloatProperty for a given name, or null if no such tunable
	// exists. Iterates GetClass()'s property list once; called rarely (UMG events,
	// JSON load). Hot-path code accesses fields directly through the actor's
	// cached USurfTuningSubsystem pointer.
	FFloatProperty* FindFloatProperty(FName PropertyName) const;

	void SnapshotDefaults();
	/** Layer 3. Returns what it applied, so it can serve as part of the save baseline. */
	void LoadGlobalOverrides(TMap<FName, float>& OutApplied);
	/** Layer 4: the active board's overlay - the only layer the HUD writes. */
	void LoadBoardOverlay();
	void SaveToDisk();

	/** Saved/TuningOverrides.json - global, hand-edited, applies to every board. */
	static FString GetJsonPath();
	/** Saved/BoardTuning/<id>.json - per board, written by the HUD. Empty id = no overlay.
	 *
	 *  It lives under Saved/ and not beside the profile because Content/Boards is staged INTO THE
	 *  PAK: on the phone a board profile is read-only, so per-board tuning could not live there even
	 *  if it wanted to. Saved/ is the only writable place on device. */
	static FString GetBoardOverlayPath(const FString& BoardId);

	bool bDirty = false;
	float SaveDebounceSeconds = 0.0f;
	FTSTicker::FDelegateHandle TickerHandle;
	bool TickDebounce(float DeltaTime);

	// Layer 5: the tired overlay. Contents of Saved/TiredTuning.json while active, plus what the
	// HUD moved during the spell; Restore holds what each key read before the overlay went on.
	bool bTiredActive = false;
	TMap<FName, float> TiredOverlay;
	TMap<FName, float> TiredRestore;
	/** Stamina's own knobs (Tuning|Stamina) describe the pool, not the tired feel; they never enter
	 *  the tired layer - StaminaForceTired saving itself there would make every spell permanent. */
	bool IsTiredLayerKey(FName PropertyName) const;
	void LoadTiredOverlay();
	void SaveTiredOverlay();
	/** Saved/TiredTuning.json - hand-edited or HUD-written while tired. */
	static FString GetTiredJsonPath();
};

namespace SurfTuning
{
	// One-line helper to fetch the subsystem from any actor's world context.
	// Returns nullptr if the game instance isn't running (e.g. PostLoad on an
	// editor-only utility actor).
	GONESURFING_API USurfTuningSubsystem* Get(const UObject* WorldContext);
}
