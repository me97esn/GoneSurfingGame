# Spec: Wave-face pitch alignment

> **STATUS (2026-06-24): proposed.** Follow-on to [wave-crossing-deceleration.md](wave-crossing-deceleration.md)
> and [barrel-glide-through-bug.md](barrel-glide-through-bug.md). The slope-thrust front-face gate (shipped)
> stops the board being *powered* over the crest, but does nothing while it climbs the front face — the
> board drives in at a flat/nose-down attitude and punches through instead of riding up the face. This spec
> adds a force that pitches the nose up to **follow the wave face** during the climb.

## Problem

When the board heads into a steepening face, nothing rotates its nose up to stay parallel to that face.
Measured on `turn-hard-into-the-wave` (nose elevation `board.forwards.Z / |f|` vs face angle `slopeSin`):

| faceSin | noseZ | |
|---|---|---|
| 0.449 | +0.235 | partial (+52% of face) |
| 0.291 | +0.420 | over-rotated |
| **0.516** | **−0.135** | **nose DOWN on a steep face** |
| **0.460** | **−0.255** | **nose DOWN** |

No consistent tracking — at the *steepest* parts the nose is often pitched **down**, driving the flat hull
into the wall. That attitude is what lets the whole board translate through (the crossing is a translation,
not a carve — see barrel-glide-through-bug.md).

### Why the existing forces don't fix it

- **No wave-relative *pitch* term exists.** The code has `waveRelativeRollSin/Cos` (roll relative to the
  wave surface, drives carving/rail gating — [SharedCalculations.cpp:285-286](../Source/GoneSurfing/SharedCalculations.cpp#L285))
  but **no pitch equivalent**. Nothing asks "is my bottom parallel to the face?"
- **There IS an emergent nose-up moment, but it's incidental and insufficient.** During the climb the front
  bottom actors carry far more upforce than the back (measured 15–35 vs 1–5; the nose digs deeper into the
  rising water, `amountUnderWater` 0.45–0.52 front vs 0.33–0.38 back). Front-lift > back-lift = a nose-up
  torque — but it's emergent from submersion asymmetry, competes with pitch angular damping
  (`AngularDampingY` + the nose-down extra), and is not enough to track the face.
- **The flow-driven pitch-up is transient.** Debug-drawing the absolute water velocity (author, 2026-06-24)
  shows it points slightly *up the face* as the board reaches the steepening base — then becomes horizontal
  a few ticks later. So any pitch-up that relies on the flow direction is a brief impulse, not a sustained
  alignment. **The fix must be geometry-based (board attitude vs the wave-surface normal), not flow-based**,
  so it holds as long as the board is on the face regardless of the momentary flow.

## Objective

Add a passive **pitch-alignment torque** that rotates the board so its bottom follows the local wave face
(drives the wave-relative pitch error to ~0) while it's on the front face. This keeps the hull presented to
the face so upthrust/buoyancy redirect the board *up and along* the face instead of letting it drive
through. Complementary to — not a replacement for — the slope-thrust gate and any cross-flow/wall drag.

## The signal: `waveRelativePitch`

Mirror the existing `waveRelativeRoll`, but in the pitch plane (rotation about `board.left`). The wave
normal is already sampled from the wave data per SharedCalculations actor
([SharedCalculations.cpp:277-278](../Source/GoneSurfing/SharedCalculations.cpp#L277):
`calculateWaveLocationAndNormal` → `waveNormal`), so no new sampling is needed.

```cpp
// Wave-tangent "forward": perpendicular to wave normal AND board.left, lying in the wave plane.
// For a wave-aligned board this equals board.forwards.
const FVector waveTangentForward = FVector::CrossProduct(this->left, this->waveNormal).GetSafeNormal();
const FVector leftN = this->left.GetSafeNormal();   // this->left carries the SC actor's ~0.2 scale; normalize
this->waveRelativePitchCos = FVector::VectorPlaneProject(this->forwards, leftN).GetSafeNormal() | waveTangentForward;
this->waveRelativePitchSin = FVector::VectorPlaneProject(-this->up, leftN) | waveTangentForward;
```

- `waveRelativePitchSin ≈ 0` when the board's bottom is parallel to the face (following it).
- Nonzero = pitch misalignment; sign distinguishes nose-too-high vs nose-too-low relative to the face.
- **Watch the un-normalized basis bug** (the recurring `this->left/forwards/up` ~0.2-scale issue from
  barrel-glide-through-bug.md): normalize every basis vector used as a unit (`leftN`, and the
  `VectorPlaneProject` plane normal), and verify the result is a *true* cosine/sine, not 0.2×.

Computed board-wide (average the two SC actors' `waveNormal`, or compute per-SC and average the result),
exposed like the roll fields.

## The force: restoring pitch torque

```
pitchAlignTorque = -k_pitchAlign * waveRelativePitchSin   (about board.left)
```
gated so it only acts on the rideable face:
- `boardWideSlopeSin > slopeThrustMinSlopeSin`-style deadzone (off on flat water between waves),
- `amountWetted` (hull engaged),
- optionally the **front-face gate** already added for slope thrust (`slopeThrustFrontFaceDir`) so it aligns
  to the front face and doesn't fight the board on the back/shoulder.

### Application — two options

- **Option A (recommended first cut): direct torque.** `AddTorqueInRadians` about `board.left`, computed
  board-wide and applied **once** by a single owner (SharedCalculations, or one designated bottom actor) to
  avoid double-applying across the ~10 FluidDynamics actors. Simplest, directly testable, no per-actor
  bookkeeping.
- **Option B (architecture-aligned refinement): fore/aft force couple.** Each bottom FluidDynamics actor
  applies a small vertical impulse ∝ (its fore-aft offset from COM) × `waveRelativePitchSin` via the
  existing `applyForceAsImpulse` path — front actors up / back down (or vice versa) for a net pitch torque
  and ~zero net force. Fits the per-actor-impulse model and auto-distributes, at the cost of needing each
  actor's offset. Move here if the direct torque feels artificial or fights the per-actor lift model.

### Interaction with existing damping

Pitch is already damped by the custom engine (`AngularDampingY` + `AngularDampingYNoseDownExtra`, applied in
`PBDRigidsEvolutionGBF.cpp`). The alignment torque must be strong enough to rotate the board on a real face
against that damping, but not so strong it oscillates. Expect to co-tune `k_pitchAlign` against the pitch
damping. The damping is a feature here — it's the derivative term that keeps the alignment critically damped
rather than springy.

## Per-actor vs shared SharedCalculations (author's note)

Currently 2 SC actors (front/back) are shared across ~10 FluidDynamics actors for performance. For pitch
alignment this is **sufficient and preferred**:

- The two SC actors already sample `waveNormal` at two fore-aft points, which directly brackets the board
  and defines the wave's fore-aft slope under it — exactly what a board-wide pitch-alignment torque needs.
- `AWaveHeight::waveHeightAndNormal(location, frame)` is available per location, so per-FluidDynamics
  sampling (≈10 lookups/tick vs 2) is *possible* if finer per-actor lift/normal modelling is wanted later —
  but it is **not required** for this feature, and the author's performance concern argues against it.
  Recommendation: keep the 2-SC board-wide normal for the alignment torque; only move to per-FD sampling if
  a separate need arises, and measure the per-tick cost (10× wave lookups) before committing.

## Tunables (on `SurfTuningSubsystem`, mirrored on `FluidDynamics`)

- `pitchAlignCoefficient` (k) — strength of the restoring torque. Default 0 = disabled (legacy), set to a
  working value once tuned.
- `pitchAlignMinSlopeSin` — face deadzone (reuse ~0.12 like slope thrust).
- (reuse `slopeThrustFrontFaceDir` for the front-face gate, or a dedicated flag.)
- Optional `pitchAlignMaxTorque` cap.
- Add `waveRelativePitchSin/Cos` + the applied torque/forces to a debug log (gate on a `pitch` flag).

## Acceptance criteria

- **AC1:** `waveRelativePitchSin` reads ~0 when the board is riding parallel to the face and grows with
  misalignment; it is a *true* sine (not deflated by the 0.2 basis scale) — verified against a hand
  computation from `board.forwards`, `board.up`, and `waveNormal` at a logged tick.
- **AC2:** With the torque enabled, nose elevation (`board.forwards.Z`) **tracks `slopeSin`** through the
  climb on `turn-hard-into-the-wave` (the table above flips from "nose down on a steep face" to "nose up,
  ≈ following") — within a tolerance band, not the current noise.
- **AC3:** `turn-hard-into-the-wave` outcome improves — the board rides up the face / is deflected back
  rather than driving its flat hull through. (Combined with the slope-thrust gate.)
- **AC4 (regression):** `surfing-down-the-line` is unharmed — speed and planing comparable to today; the
  alignment doesn't induce pitch oscillation or kill propulsion.
- **AC5:** Pop-up is not disrupted — the intro autopilot still pops the board up cleanly (gate the torque
  off until handoff, or confirm it doesn't fight the pop-up pitch command).
- **AC6:** `pitchAlignCoefficient = 0` reproduces today's behaviour exactly (clean opt-out).
- **AC7:** Flat-water autopilots (`surf-straight`, `hang-ten`, `top-turn`) unaffected (`slopeSin` below the
  deadzone there).

## Test plan

1. Add `waveRelativePitch` + the torque, default `pitchAlignCoefficient = 0`; confirm no behaviour change.
2. Enable with a trial coefficient; run `turn-hard-into-the-wave` with the `pitch`/`state` flags. Verify
   `waveRelativePitchSin` drives toward 0 and `board.forwards.Z` tracks `slopeSin` (AC1, AC2).
3. Re-check the crossing outcome (AC3) and the stall force balance from wave-crossing-deceleration.md.
4. Run `surfing-down-the-line` (AC4) and watch for pitch oscillation; tune `k` against the pitch damping.
5. Interactive pop-up check / device replay (AC5).

## Implementation status (2026-06-24): implemented (first cut)

Shipped as Option A (direct torque). [SharedCalculations.cpp](../Source/GoneSurfing/SharedCalculations.cpp)
`calculateAll` computes `waveRelativePitchSin/Cos` (true sine — basis vectors normalized, avoiding the
0.2× deflation the spec warned about) and applies a restoring angular-acceleration torque about
`board.left`, split 0.5× across the two SC actors:

```cpp
pitchAlignTorque = leftUnit * (-pitchAlignCoefficient * waveRelativePitchSin * slopeGate * share);
BasePrimComp->AddTorqueInRadians(pitchAlignTorque, NAME_None, /*bAccelChange=*/true);
```

- New tunables on [SharedCalculations.h](../Source/GoneSurfing/SharedCalculations.h) (per-instance, like the
  planing params): `pitchAlignCoefficient = 10` (0 disables), `pitchAlignMinSlopeSin = 0.12` (face deadzone).
  `waveRelativePitchSin/Cos` exposed as read-only outputs. Debug log gated on a `pitch` flag.

**Verification (headless):**
- Compiles; torque fires; `waveRelativePitchSin` is a true sine (−0.46 … +0.76).
- **`turn-hard-into-the-wave`:** nose elevation flips from nose-*down* on the steep face to nose-*up*
  (noseZ +0.18…+0.40 at faceSin 0.45–0.56) — it now pitches up to follow the face, tracking ~40–60% of
  the face angle (partial, no violent spin). The board climbs and stalls rather than driving through flat.
- **`surfing-down-the-line` (regression):** unharmed — 669 cm/s peak, planing 0.8 sustained, pitch smooth
  (±4°, no oscillation).
- Flat-water autopilots unaffected (`slopeSin` below the 0.12 deadzone).

**Caveats / tuning:** tracking is partial (~40–60%) at `coef=10`; raising `pitchAlignCoefficient` tightens
it but risks over-rotation/oscillation — left conservative for the first cut. `bAccelChange` makes the
coefficient inertia-independent. The torque sign was confirmed correct (nose pitches up). Default is
*enabled* (coef=10); set 0 to disable.

## Open questions

- Sign/return convention of `waveRelativePitchSin` (which sign is nose-up-needed) — settle empirically when
  wiring it, like the front-face gate direction.
- Torque (Option A) vs force couple (Option B) — start with A; revisit if it fights the per-actor lift.
- Does alignment alone meaningfully reduce the *translation* through the face, or only the attitude? The
  crossing is a translation; alignment changes the force geometry (hull presented to the face → redirect
  rather than penetrate) but may still need the cross-flow/wall drag (#2 in wave-crossing-deceleration.md)
  for the full fix. Evaluate after AC3.

## File references

- [SharedCalculations.cpp](../Source/GoneSurfing/SharedCalculations.cpp) — `waveNormal` (from wave data),
  `waveRelativeRoll` block to mirror (~277-296); add `waveRelativePitch` + apply the torque (Option A).
- [SharedCalculations.h](../Source/GoneSurfing/SharedCalculations.h) — new `waveRelativePitchSin/Cos`,
  `pitchAlignCoefficient` mirror.
- [FluidDynamics.cpp](../Source/GoneSurfing/FluidDynamics.cpp) / `.h` — if Option B (per-actor couple) and
  for the tuning-subsystem copy at init.
- [SurfTuningSubsystem.h](../Source/GoneSurfing/SurfTuningSubsystem.h) — new tunables.
- [WaveHeight.h](../Source/GoneSurfing/WaveHeight.h) — `waveHeightAndNormal` / `calculateWaveLocationAndNormal`
  (height + normal data), if per-FD sampling is ever pursued.
- Related: [wave-crossing-deceleration.md](wave-crossing-deceleration.md),
  [barrel-glide-through-bug.md](barrel-glide-through-bug.md) (un-normalized basis caveat).
