# Spec: Surfer rider — hip stabilization + foot IK (board tilt absorbed by the legs)

## Status
- [x] Spec drafted (2026-08-06)
- [x] C++ implemented (2026-08-06): `USurferAnimInstance::UpdateHipStabilization` — stable-frame
      pelvis target, one-time flat-water calibration, foot targets/rotations, knobs incl. bone
      names (Manny defaults). Bone-name misses log once and leave the feature pass-through.
- [x] Editor: AnimBP graph changes done + player-validated (2026-08-06): pelvis Transform Modify
      Bone, 2x Two Bone IK, per-foot rotation restore (the "optional" polish turned out
      necessary — IK preserves foot rotation relative to the calf, so knee bend lifted
      toes/heels), lean nodes after, `rollCompensation` zeroed
- [ ] Player-validated stabilization alphas baked as defaults (running on C++ defaults 1.0/0.7 —
      revisit after pumping playtests at full pitch range)

## Motivation

The rider mesh is rigidly attached to the deck, so the whole body inherits every degree of board
tilt. During pumping the board pitches roughly +45..-45 deg and the mannequin see-saws with it —
far beyond anything a real surfer's torso does. The existing `rollCompensation` /
`pitchCompensation` (specs/surfer-rider-lean.md) only counter-ROTATE the lean bones: the body
still pivots as a unit around the deck, so the hip sweeps a large arc (with the pelvis ~90 cm
above the deck, 45 deg of pitch moves it ~60 cm horizontally). Counter-rotation alone can never
fix that, because the problem is positional.

What a real surfer does: the hip stays roughly fixed in space and level, the FEET follow the
deck, and the LEGS bend/straighten to absorb the difference. That is the target behavior, and it
is a classic pelvis-stabilization + two-bone leg IK setup. No pumping animation clip is needed.

This supersedes the lean spec's board-attitude compensation (which becomes 0 / retired) and
removes its "foot IK unnecessary" scoping note — the stance is still fixed, but the deck now
tilts UNDER the stance.

## Design

### The stable frame

Define the board's **stable frame** S = board actor location + board YAW only (pitch and roll
stripped). The rider's pelvis must hold a constant transform relative to S while the deck tilts.
S follows the board's travel and heading, so the rider still goes where the board goes and turns
with it — only tilt is filtered out.

### C++ — `USurferAnimInstance` additions

All computation stays in the AnimInstance (same geometric-from-actor-transform doctrine as the
existing compensation, so it works identically live and in kinematic replays).

1. **Calibration** (once, at the first update where the board is near-flat, |pitch| and |roll|
   < `hipStabCalibrationMaxTiltDeg`): capture from the evaluated pose
   - `P0` = pelvis transform expressed in S (its "flat-board" offset), and
   - the component-space transforms of both feet (`FootTargetL/R`). Component space is rigidly
     attached to the board, so these ARE deck positions — feet targets never need updating for a
     fixed stance.
   Playable levels start on flat water, so calibration completes in the first ticks; until then
   all outputs are pass-through (alpha 0).

2. **Per-tick pelvis target**: with A = the mesh component's world transform (includes board
   tilt), the stabilized pelvis in component space is `Pelvis_cs = A^-1 * S * P0`. Blend from the
   source pose's pelvis toward this target by two knobs — `hipStabRotationAlpha` (keeps the hip
   LEVEL) and `hipStabTranslationAlpha` (keeps the hip IN PLACE) — exposed to the AnimBP as a
   location + rotation pair.

3. **Outputs** (BlueprintReadOnly, consumed by the graph): `PelvisStabLocation`,
   `PelvisStabRotation` (component space, Replace mode), `FootTargetLeft`, `FootTargetRight`
   (component-space effector locations), plus a bool `bHipStabReady` gating the whole block
   until calibration.

### AnimBP graph (manual, cannot be authored from C++)

Node order in the AnimGraph matters:

1. Stance pose (existing base) →
2. **Transform (Modify) Bone `pelvis`** — Translation + Rotation = Replace Existing, Component
   Space, wired from `PelvisStabLocation` / `PelvisStabRotation`, node alpha = `bHipStabReady`.
   Children (legs, slogpine) follow, which lifts the feet off the deck →
3. **Two Bone IK x2** (`foot_l`, `foot_r`; Manny chain thigh/calf/foot) — effector = the
   captured `FootTarget*` in Component Space; joint (pole) targets set so knees bend forward in
   the stance direction. If the soles come off-plane at high tilt (Two Bone IK preserves the
   foot's LOCAL rotation, so the component-space sole direction can drift as the chain bends),
   add a Transform Modify Bone per foot restoring the captured component-space foot rotation →
4. The existing lean/twist Transform Modify Bone nodes (`spine_01` etc.) — UNCHANGED, but they
   must sit AFTER the pelvis stabilization so the commanded lean rides on a level hip →
5. Output Pose.

`rollCompensation` and `pitchCompensation` are set to **0** in the AnimBP class defaults —
stabilization replaces them; leaving them non-zero double-counters the roll. The knobs and code
path stay for A/B fallback (set alphas 0, compensation 0.8 to get today's behavior back).

### Why not Control Rig / Full-Body IK

Two-bone IK on two static-target legs is the entire requirement; FBIK/Control Rig adds authoring
and runtime cost (this runs on Android) for no benefit at this scope. Revisit only if arms/board-
grab poses ever need IK.

### Leg reach limit (the one real constraint)

With the hip fully pinned, 45 deg of pitch displaces the deck under the front/back foot by
~±35 cm vertically (feet ~50 cm from board center along its length): the downhill leg must
straighten and can run out of length — a visible knee-pop/stretch. Mitigations, in order:
- `hipStabTranslationAlpha` default **0.7** (not 1.0): the hip keeps ~30% of the deck's motion,
  which preserves most of the visual fix while giving the legs slack. Rotation alpha defaults
  to **1.0** — keeping the hip level is cheap and never hits a reach limit.
- The surf stance is already crouched; deepening the base pose buys more travel (asset-side).
- Two Bone IK "Allow Stretching" stays OFF (rubber legs read worse than a slight hip dip).

### Interactions

- **Fall/ragdoll** (specs/surfer-fall-ragdoll.md): once `TriggerFall` sets SimulatePhysics the
  anim pose is irrelevant; on `RestoreRiderFromRagdoll` (replay path) the anim resumes and the
  stored calibration is still valid (component-space deck geometry is unchanged). No coupling.
- **Replay**: board transform is the recorded one and A/S are derived from it geometrically —
  stabilization behaves identically. Recorded weight playback (SetReplayWeights) is untouched.
- **Physics/tests**: read-only and cosmetic, like the whole rider layer. Headless suite must be
  byte-identical in behavior (rider isn't even rendered there meaningfully; CSVs unaffected).

## Knobs (UPROPERTYs on `USurferAnimInstance`, Category "Surfer|HipStab")

| knob | default | meaning |
|---|---|---|
| `bHipStabEnabled` | true | master switch; false = legacy behavior (with compensation knobs) |
| `hipStabRotationAlpha` | 1.0 | 0..1, how level the hip is held against board tilt |
| `hipStabTranslationAlpha` | 0.7 | 0..1, how firmly the hip holds its stable-frame position (< 1 leaves leg slack, see reach limit) |
| `hipStabCalibrationMaxTiltDeg` | 3 | board tilt below which the one-time calibration may run |
| `hipStabInterpSpeed` | 0 | optional smoothing (1/s) of the pelvis target; 0 = direct (board motion is already smooth) |
| `bHipStabUseStoredCalibration` | true | use the stored values below instead of the runtime flat-water capture |
| `storedPelvis*/storedFoot*` | baked | calibration snapshot (pelvis stable-frame offset + source CS, foot targets/rotations); captured 2026-08-06 from the Manny stance + current board placement and baked as C++ defaults |

### Stored calibration (pre-stance animations)

The runtime capture assumes the STANCE pose is playing during the level's first flat seconds and
recalibrates every play/Restart. Once a paddling/pop-up animation precedes the stance, that
assumption breaks — and there is no later flat window (after pop-up the board is on a wave).
Procedure: run ONE stance-only session, copy the paste-ready "hip-stab calibrated" log block
into the AnimBP Class Defaults' Stored Calibration section, set
`bHipStabUseStoredCalibration = true`. The values are pose+attachment geometry, so they only
change when the stance pose asset or the mesh's placement on the board changes — recapture then
(and after AnimBP graph changes that affect the stance). NOTE: with a paddling state in the
AnimBP, the stab/IK nodes must live in (or be gated to) the stance branch — with stored
calibration `bHipStabReady` goes true immediately, and the pelvis Replace node would fight the
paddling pose.

## Acceptance criteria

- **Given** pumping pitches the board +45..-45 deg, **then** the hip stays visually planted
  (small residual motion per translation alpha) and level, the torso does NOT see-saw, both feet
  remain on the deck, and the legs visibly bend/straighten to absorb the tilt.
- **Given** a carving roll, **then** the same holds laterally (this replaces — and improves on —
  the old rollCompensation counter-lean, including the "leans as a whole body" complaint: the
  body now articulates at the hip).
- **Given** weight-shift input, **then** lean/twist still work as before, applied on top of the
  level hip.
- **Given** a kinematic replay of a pumping ride, **then** the rider behaves identically to live.
- **Given** a fall + Replay + Restart cycle, **then** ragdoll and restore work unchanged.
- Snapshot suite: Compare.ps1 results unchanged (cosmetic-only feature).

## Test cases

1. PIE: pump hard on a wave — hip steady, knees pumping, no torso see-saw; A/B with
   `bHipStabEnabled` false to confirm the delta.
2. PIE: hard carve both directions — hip level, feet planted, no double-counter lean (verify
   rollCompensation is 0 in class defaults).
3. PIE: `SurfFall` then Replay — ragdoll, restore, replayed rider stabilized normally.
4. On-device: pumping ride end-to-end (perf sanity: 2 IK nodes + 1 modify-bone).
5. Headless suite: `RunGameAndCollectLogs.bat ::surfing-down-the-line` — OK, unchanged rows.
