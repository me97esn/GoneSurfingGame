# Spec: Surfer rider — weight-shift lean and upper-body twist

## Status
- [x] Spec drafted (2026-07-29)
- [x] C++ implemented (2026-07-29): `USurferAnimInstance`
  (`Source/GoneSurfing/SurferAnimInstance.h/.cpp`). Compiles clean.
- [ ] Editor: mannequin imported, surf-stance pose, AnimBP, mesh attached to board
- [ ] Player-validated lean/twist angles baked as defaults

## Motivation

The board surfs riderless. A visible surfer that leans with the player's weight input makes the
control loop legible (you see the input) and the board feel ridden. Iteration 1 is deliberately
minimal: ONE fixed surf-stance pose with procedural lean/twist on top, driven by the same
`AWeightDistribution` values that drive the physics — so the rider and the board always agree.

Out of scope for iteration 1 (the architecture leaves seams): pop-up animation (a montage
triggered by the autopilot's existing pop-up step), wipeout ragdoll (blend to simulated when the
board ejects the rider), foot IK (unnecessary while the stance is fixed and the mesh is attached
to the deck).

> **2026-08-06 update:** wipeout ragdoll landed (specs/surfer-fall-ragdoll.md), and the
> board-attitude compensation below is being SUPERSEDED by hip stabilization + foot IK
> (specs/surfer-rider-hip-ik.md) — counter-rotating the lean can't fix the hip's positional
> sway when the board pitches ±45° during pumping. Once that ships, `rollCompensation` /
> `pitchCompensation` default to 0.

## Design

### Inputs (all already computed per tick)

`AWeightDistribution::amountToTheRight` and `amountInFront` (0..1, 0.5 = centered) — the player's
weight-shift intent, already smoothed by the input pipeline (auto-centering etc.). The rider reads
the SAME values the physics consumes, so visual lean and board response cannot desynchronize.

### C++ — `USurferAnimInstance` (UAnimInstance subclass)

`NativeUpdateAnimation` computes BlueprintReadOnly floats the AnimBP consumes:

- `LeanRightDeg`  = (amountToTheRight − 0.5) × 2 × `maxLeanRightDeg`
- `LeanForwardDeg` = (amountInFront − 0.5) × 2 × `maxLeanForwardDeg`
- `TwistYawDeg`   = (amountToTheRight − 0.5) × 2 × `maxTwistYawDeg`

Twist shares the lateral source deliberately: weight left ⇒ lean left AND look left, one input.
Each output is `FInterpTo`-smoothed by `leanInterpSpeed` so the rider flows instead of snapping
(the weight values are input-smoothed already; this adds the body's own inertia on top).

WeightDistribution resolution (no AnimBP-to-level-actor reference exists in UE): auto-resolved at
runtime — prefer the `AWeightDistribution` whose `Surfboard` equals the mesh's owning actor (the
board the mannequin is attached to), else the nearest one. Cached after first success.

Sign conventions follow AWeightDistribution: amountToTheRight = 1 ⇒ positive LeanRight/Twist.
If a rotation reads backwards in the AnimBP, negate the corresponding max-angle knob rather than
editing the graph.

### Editor — manual setup steps (cannot be authored from C++)

**Step 1 — Get the mannequin into the project**
1. Content Browser → **+ Add → Add Feature or Content Pack → Third Person** (brings Manny/Quinn
   with their skeleton). Any other skeletal mesh works too — adjust bone names below.

**Step 2 — One surf-stance pose** (no animation playback needed)
Any ONE of:
- Open a crouched-idle animation → scrub to a good frame → right-click the asset →
  **Create → Create PoseAsset** (or just use the clip and let the AnimBP play it frozen at that
  time).
- Pose the skeleton with **Control Rig** and bake a single-frame animation.
- Download Mixamo's "surfing" idle, import FBX, retarget to the UE5 skeleton (IK Retargeter).

**Step 3 — Animation Blueprint**
1. Right-click the mannequin **Skeleton** asset → **Create → Anim Blueprint**, name e.g.
   `ABP_Surfer`.
2. Open it → toolbar **Class Settings** → Details → **Parent Class = SurferAnimInstance**.
   After this, the C++ variables (`LeanRightDeg`, `LeanForwardDeg`, `TwistYawDeg`) and the
   tuning knobs appear; knobs are edited in the AnimBP's **Class Defaults**.
3. In the **AnimGraph**: drop the stance pose/animation asset as the base node.
4. Add **Transform (Modify) Bone** nodes between the base and Output Pose (they auto-insert
   Local↔Component space conversions). For each node: set **Bone to Modify**, set **Rotation
   Mode = Add to Existing**, **Rotation Space = Component Space**, and expose the Rotation pin
   to wire a variable in:
   - `spine_01`: lateral lean from `LeanRightDeg` (optionally a second node on `spine_02` with
     half the value for a softer curve)
   - `spine_01`: fore-aft lean from `LeanForwardDeg` (can share one node with the lateral —
     both feed different axes of the same Rotation pin via a Make Rotator)
   - Twist: **single-bone, on `spine_01`'s Z alongside the leans** (player decision — a surfer
     rotates the torso as a unit to face a direction rather than turning the head; supersedes
     the originally-planned spine_03/05/neck 40/40/20 distribution). A surfer's NEUTRAL gaze is
     down the line, not over the nose, so the Z input is `TwistYawDeg + neutralTwist`
     (player-calibrated **−30°** on Manny; either a graph literal OR the `neutralTwistYawDeg`
     C++ knob — never both). Weight shifts swing around that baseline — the left/right
     asymmetry is real surfing's frontside/backside asymmetry. Fallback if the single hinge
     ever reads rigid at extremes: split the same sum 60/40 across `spine_01`/`spine_02`.
5. **Axis calibration** (one-time): which Make Rotator input (roll/pitch/yaw) produces which
   visual rotation depends on mesh import orientation AND the stance yaw (a surfer stands
   sideways). The C++ variables can NOT be typed into for this — they're VisibleAnywhere
   computed outputs, and the preview scene has no WeightDistribution so they sit at 0 (the C++
   would overwrite typed values anyway). Instead: leave the Rotation pin's variable disconnected
   and type a LITERAL value (e.g. 30) into the Transform (Modify) Bone node's exposed rotation
   field; the preview updates live. Permute the rotator axis until the rotation reads right,
   then wire the variable into that input and move to the next node. The mesh sits at a FIXED
   relative rotation on the board, so component-space axes are constant deck-relative —
   calibrate once. If a direction is mirrored, negate the corresponding max-angle knob in Class
   Defaults, don't rewire the graph. (The preview mannequin always shows the pose at variables
   = 0 — the full loop only runs in PIE with the mannequin attached to the board.)
6. Compile + Save.

**Step 4 — Attach the rider to the board**
1. Open the surfboard Blueprint → **+ Add → Skeletal Mesh Component**, parent it to the board
   mesh.
2. Set its Skeletal Mesh = the mannequin, **Anim Class = ABP_Surfer**.
3. Position it standing on the deck, yawed to surf stance (sideways, lead foot toward the nose).
   Attachment means board roll/pitch carries the rider; the lean angles ride on top.

**Step 5 — Verify**
1. PIE → shift weight left: rider leans left AND twists/looks left; board rolls left in unison.
   Right/front/back likewise. Release → smooth return to neutral.
2. No manual WeightDistribution wiring is needed — the AnimInstance resolves it at runtime
   (Surfboard-match, else nearest). If the rider stays frozen, check the Output Log and that the
   board BP's WeightDistribution has its `Surfboard` property set.
3. Knob tuning (lean/twist maxima, interp speed) lives in ABP_Surfer → Class Defaults, editable
   without touching C++.

## Knobs (UPROPERTYs on the AnimInstance — edit in the AnimBP class defaults)

| knob | default | meaning |
|---|---|---|
| `maxLeanRightDeg` | 18 | full lateral weight shift ⇒ this much body lean (negate to flip) |
| `maxLeanForwardDeg` | 12 | full fore-aft shift ⇒ this much fore-aft lean |
| `maxTwistYawDeg` | 25 | full lateral shift ⇒ this much upper-body twist toward the lean |
| `leanInterpSpeed` | 6 /s | smoothing of all three outputs (body inertia on top of input smoothing) |
| `rollCompensation` | 0.8 | fraction of the board's WORLD roll subtracted from the lean. The attached mesh inherits deck roll AND adds the lean — double-leaning (observed: small shifts read as falling off). A real surfer absorbs roll in ankles/knees; 1 = torso fully world-stabilized, 0 = legacy deck-relative. Negative = sign flip if board-roll convention opposes the calibrated lean axis. Reads `SC::worldRelativeRollSin` (wave-tilt-excluded by design) |
| `pitchCompensation` | 0 | fore-aft sibling (board world pitch vs LeanForward); off by default — fore-aft posture mostly follows the deck |
| `neutralTwistYawDeg` | 0 | down-the-line gaze baseline; −30 calibrated on Manny. Default 0 because the first AnimBP carries −30 as a graph literal — use one or the other, never both |

## Replay

During on-device kinematic replay the WeightDistribution is tick-frozen, so the rider held one
constant stale lean (observed). Fixed 2026-08-05 following the spray doctrine (record outputs,
play back verbatim): trace columns 28-29 record `amountInFront`/`amountToTheRight`, and
`ApplyReplayAtTime` pushes them via `USurferAnimInstance::SetReplayWeights`. Board-attitude
compensation needs no override — it is computed geometrically from the owning board actor's
transform (board.forwards = local +Y, board.left = local -X), which is equally correct live and
under kinematic playback. Pre-rider traces replay a neutral rider. See
specs/on-device-ride-replay.md ("Spray in replays").

## Acceptance criteria

- **Given** the player shifts weight fully left, **then** the rider leans left ~`maxLeanRightDeg`°
  and twists/looks left ~`maxTwistYawDeg`°, smoothly (no snap), while the board rolls left.
- **Given** weight returns to center (auto-center on release), **then** the rider returns to the
  neutral stance at the same smoothed rate.
- **Given** an autopilot drives the weight (cinematic intro / tests), **then** the rider animates
  identically — the AnimInstance reads AWeightDistribution, not player input.
- **Given** the board rolls/pitches with the wave, **then** the rider stays planted on the deck
  (attachment), leaning relative to the deck.
- Zero physics impact: the AnimInstance only reads; snapshot trajectories unchanged.

## Test cases

1. PIE: hold weight left/right/front/back — verify lean + twist direction and magnitude; verify
   smooth return on release.
2. Cinematic intro autopilot: rider leans through the scripted sequence.
3. Headless snapshot suite: Compare.ps1 unchanged (rider is read-only + cosmetic).
