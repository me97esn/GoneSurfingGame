# Spec: Surfer fall — the rider goes off the board on roll / lost planing / off the wave

## Status
- [x] Spec drafted (2026-08-05)
- [x] C++ implemented (fall detection + ragdoll + input lockout on `ASurfboardPawn`) (2026-08-05)
- [ ] Editor: physics asset sanity check on the rider mesh; PIE + on-device validation
- [ ] Player-validated thresholds baked as defaults
- [x] **Lost planing is no longer a wipeout (2026-09-14).** Owner: a wipeout is something specific,
      "totally different from the board losing speed and stopping". The two triggers now end the
      ride through one shared `EndRide(ERideEndKind)`: roll = `Wipeout` (card WIPEOUT), lost
      planing = `LostWave` (card LOST THE WAVE). `bFallen` still means "ride over" for every
      reader; `RideEndKind` says which. **Both ragdoll.** The first cut left the rider standing
      on a stall; on PC the owner read that as "the ragdoll fell and got reset onto the board" -
      a standing rider is not an ending. Reverted the same day; only the title differs now.
- [x] **Off the wave (2026-09-21).** Owner, on the device rides of 2026-09-21: *"the board is moving in
      the white water very slowly for a number of seconds before the surfer falls off. I would like him
      to fall off sooner."* The planing stall needs the board under ~200 cm/s for planing's ~2 s decay,
      and the whitewater carries a board at 200–300, so those rides dragged on 6–10 s. Third trigger,
      on the wave-geometry service's zone (`ASharedCalculations::waveZone`, [wave-geometry.md](wave-geometry.md)):
      **off the clean wave — zone not Pocket/Shoulder (whitewater, behind, flat) — for `FallOffWaveSeconds`
      (3) and slower than `FallOffWaveSpeed` (300 cm/s) at that moment → `LostWave`.** The timer resets
      the moment the board is back in the pocket or on the shoulder; Unknown zone (no model) never
      counts; same arming as the other two. Applied offline to the phone's own data of the four rides:
      each slow foam wallow ends 3–3.5 s after leaving the wave (t=14.1 / 8.9 / 26.4 / 11.2 against the
      actual 21.3 / 15.0 / 32.3 / 18.2); a board riding the whitewater to the beach at ≥ 300 keeps going.
      The variant that only counted slow seconds ended them 4.5–6 s after, because the whitewater shove
      itself keeps the speed above 300 for the first seconds. Tunables in `USurfTuningSubsystem`
      (Tuning|Fall) so the device can move them; 0 s = off. **Device-validated by the owner 2026-09-22.**
- [x] **Authored fall replaces the ragdoll (2026-09-22).** Owner: *"I am not happy with the ragdoll fall
      when the surfer falls over"*, and authored two clips in `L_AnimScratch` — `Content/Surfer/SK_Falling_left`
      and `SK_Falling_right`, off the board's left / right side (board.left = local −X), ending lying in
      the water. Built the same day, see "Authored fall" below: `USurferAnimInstance` plays the clip as an
      overlay (explicit time, once, held on the last frame — the tired/pump pattern, not a state-machine
      link), and the pawn moves the detached rider kinematically: the board's momentum relaxing into the
      water's own velocity, levelled to upright, riding the water surface. The card waits for the clip to
      end. The ragdoll stays as the fallback when the clips are not assigned and behind `bFallUseRagdoll`
      for A/B. **Editor step owed** (T1 below); not eye-tested until it is done.
- [x] **One card title: RIDE OVER (owner, 2026-09-22).** WIPEOUT / LOST THE WAVE "describe too specific
      endings that might be inaccurate", and warmer titles "would require judgement if it actually was a
      good ride or not". `ERideEndKind` stays for the log and the trace note; only the card stopped
      naming it.
- [ ] **The roll trigger has never fired in play** (owner, 2026-09-14: "there has never been a
      wipeout in this game"). The board is roll-stiff - `worldRollSin` ~0.04 (~2.7 deg) even in a
      hard turn ([[small-weight-shift-sharp-turn]], [[surface-carve-roll-spring]]) - so 90 deg is
      unreachable, and every ending the player has seen was the stall. The ragdoll path is live only
      through `SurfFall` / Blueprint `TriggerFall`. A real wipeout needs a real condition (pearling
      the nose, going over the falls, lip impact); until one exists this feature is the stall plus
      an unreachable branch.

## Motivation

The rider currently stays glued to the deck through any wipeout-grade event — the board can roll
past vertical or wallow to a stop with the mannequin serenely in stance. A fall makes failure
legible and gives rides a proper ending: the surfer ragdolls off, and the ride is over until the
player reviews it (Replay) or starts fresh (Restart).

## Triggers (any one; the first two player-specified 2026-08-05, the third 2026-09-21)

1. **Roll**: |board world roll| > `fallRollThresholdDeg` (default 90°) — computed geometrically
   from the board actor transform (same convention as the rider's roll compensation:
   board.forwards = local +Y, board.left = local -X).
2. **Lost planing**: `AmountPlaning` < `fallPlaningStopThreshold` (default 0.05) — the board has
   wallowed to a stop. Planing already has hysteresis + ~2s decay (PlaningDecayTime), so this
   fires only after genuine speed loss, not transient dips.

3. **Off the wave**: the wave-geometry zone at the board not Pocket/Shoulder for `FallOffWaveSeconds`
   and speed < `FallOffWaveSpeed` at that moment (Status, 2026-09-21).

### Arming (prevents false falls)

Both triggers are DISARMED until the ride is actually underway: armed when player controls are
enabled (post-autopilot handoff) AND `AmountPlaning` first exceeds `fallPlaningArmThreshold`
(default 0.5). Without arming, "not planing" would trigger instantly while paddling, and the
pop-up autopilot's attitude excursions could trip the roll check.

## On fall (all in `ASurfboardPawn::EndRide`)

1. **Rider goes off the board**: the board's SkeletalMeshComponent detaches (keep-world) and then
   either plays the **authored fall** (below; the default whenever the AnimBP has both clips) or
   **ragdolls** (the original: `SetSimulatePhysics(true)`, Ragdoll collision profile, the board's
   velocity × `fallVelocityInherit` on every body plus a rigid topple about the feet — see the
   implementation notes; kept as the fallback and behind `bFallUseRagdoll`).
2. **Board stops reacting to input**: `bPlayerControlsEnabled = false` and the weight is
   re-centered (0.5/0.5) so no stale weight torque keeps steering. The board's PHYSICS stays
   live — it washes out / tumbles realistically; only control input is cut.
3. **Recording stops** (`StopInputTrace`): the ride ended at the fall, so "latest ride" ends
   there too — a subsequent Replay plays up to the fall and holds, per the existing
   hold-on-last-frame behavior.
4. **Terminal until Replay or Restart** (player-specified): fall state clears only via
   `ReplayLastRide()` (watch the ride) or `RestartLevel()` (fresh ride; level reload resets the
   rider). No mid-ride recovery in v1.

## Authored fall (2026-09-22)

The fall is a clip the owner animated, not a simulation. What the code does around it:

- **Which clip.** The side is the one the ragdoll's topple already chose: the low rail when the
  board is rolled past 2°, else the weighted rail (read before the weight is re-centred). Right
  (−board.left) → `FallRightSequence`, else `FallLeftSequence`.
- **The pose.** `USurferAnimInstance::StartFall(bToBoardRight)` sets `bFalling`, picks
  `FallSequence`, and `UpdateFallClip` advances `FallSequenceTime` once to the clip's length and
  holds it there — the rider stays lying in the water for as long as the ride-over state lasts. An
  overlay that replaces the whole pose (state machine, lean, hip-IK, pump, tired), deliberately not
  a link in `ESurferAnimState` (the one-step walk in `SetAnimState` must not gain a link it could
  stick on). `StopFall()` on the Replay path.
- **The body in the world** (`ASurfboardPawn::UpdateFallenRider`, every tick while
  `bRiderFallAnimated`). The mesh is detached keep-world and moved kinematically:
  - *momentum into drift* — horizontal velocity starts at the board's × `fallVelocityInherit` and
    relaxes toward the water's own velocity × `fallWaterDrift` with time constant
    `fallMomentumDecaySeconds` (0.6 s): a body in the water carries what it had for a moment, then
    goes where the water goes, so a whitewater fall is carried shoreward with the foam;
  - *on the surface* — Z is the water surface at the rider's XY (`calculateWaveLocationAndNormalAuto`)
    plus the height the rider had over it at the fall instant. Relative on purpose: the board rides
    submerged and the data's absolute height is not world Z ([[waveheight-return-not-world-z]]),
    and neither matters while the same offset is kept. Without this a fixed-Z rider is left in the
    air or under the wave within a couple of seconds — the card does not pause the world;
  - *levelled* — the deck's pitch and roll at detach are slerped out over `fallLevelSeconds` (0.3 s),
    keeping the rider's heading, so the clip plays in the upright frame it was authored in.
- **Rate and the sink (owner, first PIE look 2026-09-22).** *"The animations are playing too slowly"* →
  `FallPlayRate` on the anim instance (AnimBP Class Defaults, Surfer|Fall; 1.5 by default, 1 = as
  authored). *"If the surfer stays on the surface it looks like he is sitting on the water"* — the
  board rides ~50 cm above the rendered wave, so "on the surface" is above the mesh — *"the best
  solution is to remove the surfer downwards at the end of the falling animation"* → once the clip
  has ended (plus `fallSinkDelaySeconds`, 0) the rider sinks at `fallSinkRate` (80 cm/s) to
  `fallSinkDepth` (200 cm) under where the clip left him, and holds there, under the card.
- **The card** waits for the clip: `max(1.2 s, clip length / rate + 0.6 s)` after the fall
  (`kWipeoutCardBeatAfterClipSeconds`), so it never comes up over a rider still going in.
- **Fallback.** With either clip unassigned in the AnimBP the pawn logs a warning and ragdolls as
  before; `bFallUseRagdoll` (pawn, Fall) forces the ragdoll for comparison. `SurfFall` in the console
  is still the way to trigger a fall on demand.

### Editor step (owed — T1)

In `SK_Mannequin_AnimBlueprint`:
1. Class Defaults → Surfer|Fall: assign `FallLeftSequence` = `SK_Falling_left`,
   `FallRightSequence` = `SK_Falling_right`.
2. At the **top level of the AnimGraph, last before Output Pose** — after the `RiderSM` state
   machine and every procedural bone — a **Blend Poses by Bool**: Active Value = `bFalling`,
   True = a **Sequence Evaluator** (never a Player) with its Sequence pin exposed and bound to
   `FallSequence`, Explicit Time bound to `FallSequenceTime`; False = the pose as it was.
   True Blend Time **0.15 s** (the clips start from the authored stance, but the live rider carries
   lean + hip-IK on top, so this one — unlike tired — needs a short cross-fade); False Blend Time 0.
3. Compile, save. Then in PIE: ride, type `SurfFall` — the rider should go off to one side, level,
   end lying in the water, and the card should come up after the clip ends. Check both sides
   (weight left vs right at the moment of `SurfFall`).

## Explicitly out of scope (v1)

- **Ragdoll in replays**: replay drives the rider from recorded weight amounts; a ride that
  ended in a fall replays with the rider standing through to the recording's end (which IS the
  fall moment, since recording stops there). Reproducing the ragdoll kinematically would need
  recorded bone transforms — own spec if ever wanted.
- Blended (partial-physics) ragdoll, get-back-on animations, swimming, board-rider collision
  during the tumble.
- Fall SOUND/FX (splash burst at rider impact is a natural follow-on for the spray system).

## Knobs (UPROPERTYs on ASurfboardPawn, Category "Fall")

| knob | default | meaning |
|---|---|---|
| `bFallEnabled` | true | master switch |
| `fallRollThresholdDeg` | 90 | \|board world roll\| beyond this = fall |
| `fallPlaningStopThreshold` | 0.05 | AmountPlaning below this (while armed) = fall |
| `fallPlaningArmThreshold` | 0.5 | AmountPlaning must first exceed this (with controls live) to arm the triggers |
| `fallVelocityInherit` | 1.0 | fraction of board velocity the rider keeps at detach (both falls) |
| `bFallUseRagdoll` | false | force the physics ragdoll even with the clips assigned (A/B) |
| `fallMomentumDecaySeconds` | 0.6 | authored fall: time constant of the rider's velocity relaxing into the water's |
| `fallWaterDrift` | 1.0 | authored fall: fraction of the water's velocity the rider drifts with |
| `fallLevelSeconds` | 0.3 | authored fall: seconds to level from the deck's tilt to upright |
| `fallSinkRate` | 80 | authored fall: cm/s the rider sinks after the clip (0 = never) |
| `fallSinkDepth` | 200 | authored fall: cm sunk, then held |
| `fallSinkDelaySeconds` | 0 | authored fall: pause between clip end and the sink |
| `FallPlayRate` (anim instance) | 1.5 | speed the fall clips play at, 1 = as authored |
| `fallSidewaysKick` | 50 | ragdoll only: cm/s UNIFORM shove toward the fall side; keep small or it reads as a hop |
| `fallUpwardKick` | 0 | ragdoll only: cm/s UNIFORM upward kick; same hop caveat, default off |
| `fallToppleRate` | 150 | ragdoll only: deg/s rigid topple about the feet (per-body v = ω × (p − feet): head fastest, feet ~none) |

## Implementation notes (2026-08-05)

- All on `ASurfboardPawn`: `UpdateFallDetection` (tick), `TriggerFall` / `EndRide`, `UpdateFallenRider`
  (tick, authored fall), `RestoreRiderAfterFall` (was `RestoreRiderFromRagdoll`).
- `AmountPlaning` is read from the `ASharedCalculations` wired to the pawn's board (lazy-resolved;
  falls back to `SharedCalculationsForCamera`, then any SC). No SC in the level = triggers never arm.
- `UpdatePlayerControlState` early-outs while fallen — otherwise the finished-autopilot path
  (handoff delay already elapsed) would re-enable player controls one tick after the fall.
- `ReplayLastRide` re-attaches the rider (physics off, pre-fall collision profile + relative
  transform restored) and clears the fall state before entering replay — the recorded ride plays
  with a standing rider per out-of-scope. If no trace exists, the level is left untouched
  (rider stays ragdolled, still fallen).
- Console command `SurfFall` forces a wipeout in PIE / -game for testing.
- Playtest finding (2026-08-05): a lost-planing fall fires at near-zero board speed, so the
  inherited velocity is ~nothing and the ragdoll collapsed straight down THROUGH the deck
  (board-rider collision is out of scope). First fix (uniform sideways+up kick) read as a
  sideways HOP — uniform velocity translates the whole body. Second fix: a rigid topple about
  the feet, applied per body as v = ω × (p − feetPivot) with the same ω on every body — head
  fastest, feet ~stationary. Fall side = low rail when the board is rolled (roll falls), else
  the weighted rail (read before the weight recenter). Feet pivot = bottom-center of the mesh
  bounds.
- The spec's original arming assumption ("test runs never enable player controls") was WRONG:
  the pawn enables controls once the test autopilot finishes + handoff delay, so a headless
  `surfing-down-the-line` run armed and fired a lost-planing wipeout in its post-recording
  tail (verified in the log). Detection is therefore explicitly gated off in test runs —
  `surf.autopilots` filter non-empty (same convention as `bAutoQuitOnComplete`) — and while
  `bExternalWeightOverride` is set (trace-replay autopilots, which record after handoff and
  would otherwise be corrupted by a mid-replay fall).

## Acceptance criteria

- **Given** a hard carve that rolls the board past 45°, **then** the rider detaches and ragdolls
  with the board's momentum, and weight/turn input stops affecting the board.
- **Given** the board slowing until planing is lost, **then** same fall behavior.
- **Given** the paddling/pop-up phase (pre-arming), **then** no fall regardless of planing state
  or autopilot attitude.
- **Given** a fall, **when** Replay is pressed, **then** the recorded ride plays back up to the
  fall moment and holds (standing rider — see out-of-scope).
- **Given** a fall, **when** Restart is pressed, **then** a fresh ride with the rider back in
  stance (level reload).
- Snapshot tests unaffected: fall detection is disabled whenever the `surf.autopilots` test
  filter is set or a replay autopilot is driving weight (see implementation notes — controls
  DO come live after the test autopilot finishes, so this needs an explicit gate).

## Test cases

1. PIE: deliberately over-carve → ragdoll + input lockout; press Replay → playback to fall;
   Restart → clean ride.
2. PIE: ride onto flat water and stall → planing fall.
3. PIE: sit at spawn paddling without popup → no fall (arming).
4. Headless suite: Compare.ps1 unchanged.

## Tasks

1. [x] **T1 Editor step** — wired 2026-09-22 (first try had the evaluator's Explicit Time on
       `TiredSequenceTime`, which holds the clip on frame 0: a standing rider drifting off the board).
2. [ ] **T2 Eye-test the motion** — momentum into drift, the levelling, the rider on the surface as
       the wave passes under the card. The three knobs are on the pawn (Fall). If the rider visibly
       floats or sinks over the seconds the card is up, the relative-height hold is the suspect.
3. [ ] **T3 Device** — a real LOST THE WAVE in the whitewater: the rider should be carried with the
       foam, not left behind it.
