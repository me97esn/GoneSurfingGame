# Spec: Rail Lift = Weight-Shift Steering (gate on commanded lean, not board roll)

Status: **IMPLEMENTED (steering-only, first pass) — 2026-07-20. Behavioral validation partial (1 replay).**

## Overview

Rail Bernoulli lift ("rail lift") is the **weight-shift steering** force — it pulls the board
toward the rail the surfer engages by leaning ([bottom-yaw-hydrofoil.md:294](bottom-yaw-hydrofoil.md)).
Its direction is `±board.left` = "toward the engaged rail". The engaged rail used to be picked from
the board's measured wave-relative roll (`waveRelativeRollSin`), which carries a ~4 Hz **unintentional
wobble** — so the force dithered into/out of the wave, tracking the wobble as if it were steering input.

This spec gates the engaged rail on the surfer's **commanded lateral weight shift**
(`amountToTheRight`) instead. Commanded lean is the surfer's *intent*; it is smooth and deliberate,
so the steering force stops oscillating while remaining fully bidirectional (you can still carve either
way — the property that made the roll-slaved design correct in the first place, see
[rail-lift-into-wave-direction.md](rail-lift-into-wave-direction.md) for why "steady into the wave"
was the wrong fix).

This mirrors the **bottom-hydrofoil turn-gate** ([FluidDynamics.cpp:1228-1246](../Source/GoneSurfing/FluidDynamics.cpp#L1228)),
which already gates carve thrust on `lateralShift = |amountToTheRight − 0.5|` to distinguish
"wave is pushing me sideways" from "I'm actively carving". Same signal, same rationale, applied to
the rail-lift engaged-rail gate.

## Objective

Eliminate the rail-lift direction oscillation without losing bidirectional steering, by driving the
engaged rail from commanded lean rather than measured roll.

## Background & evidence

Measured on the `phone-2026-07-17-14-51-09` replay (front SC, `surf.debug.flags 'crossing,torque'`
+ `surf.debug.actors 'Calculations'`; `Fin > 0` = into the wave):

- **The bug (pre-fix):** direction is 100% roll-slaved. Rail-lift `Fin` flipped sign **~35×** over the
  ride; 57% of firing ticks pushed OUT of the wave; net −84k. The board's wave-relative roll is a
  symmetric ~4 Hz wobble (34% pos / 34% neg / 32% deadband), and the roll-slaved gate tracked it.
- **Commanded lean is smooth.** Extracted from the trace (`tilt_roll_deg` → deadzone/scale →
  `amountToTheRight`): the commanded lateral shift flips sign only **2×** over the same ride (43%
  leaning right, 23% left, 34% centered; mean |shift| 0.22). ~17× fewer sign changes than the wobble.
- **After the fix (same replay):** rail-lift `Fin` sign flips **1×**; net **+21.8k INTO** the wave;
  the into/out split now reflects the two deliberate lean phases, not dither. The board still rides
  the FACE ~5 s at planing 0.80 and glides THROUGH — comparable to pre-fix (crossing 11.98 s vs
  ~10.1 s pre-fix; PC-replay timing is chaotic, not a strict comparison).

## Design (implemented)

Single site: `AFluidDynamics::calcLiftForce` gate block. `s = amountToTheRight − 0.5`:

- **Engaged rail (gate):** `VE_Left` fires when `s < −0.05`; `VE_Right` fires when `s > +0.05`.
  (Below ±0.05 = trimming, not steering → gate closed.)
- **Direction:** unchanged per side — `VE_Left → −board.left`, `VE_Right → +board.left` ("toward the
  engaged rail"). Only the gate signal changed, so **no new sign mapping is introduced**. Leaning
  right dips the right rail and pulls toward it; empirically that is into-wave (net +Fin with the
  dominant right-lean confirms the mapping is not inverted).
- **Magnitude ramp:** `× SmoothStep(0.05, 0.20, |s|)` (same shape as the bottom-hydrofoil intentGate)
  so force scales in with commitment instead of snapping on.

Consequence — **steering-only:** with weight centered the gate is closed, so rail lift gives no
passive into-wave grip. Grip during trim is expected to be carried by the engine velocity-redirect
([carve-grip-via-redirect.md](carve-grip-via-redirect.md), `CarveGripRate = 4`, default-on). The
first replay supports this (board held the face with rail lift weak), but see Open Questions.

## Acceptance criteria

1. **Oscillation gone:** rail-lift `Fin` sign flips ≤ ~3 over the `14-51-09` ride (was ~35). ✅ (1).
2. **Follows intent:** net `Fin` positive (into wave) with the dominant right-lean; direction holds
   through each commanded lean phase. ✅ (net +21.8k).
3. **Board still rides the face:** planing sustained on the face for a comparable window; no early
   wash-out vs pre-fix. ✅ on 1 replay (~5 s at 0.80) — **needs 3× repeats** (bistable replay).
4. **Defaults / other maps unaffected** where no weight-shift is commanded (gate simply closed).

## Open questions / next steps

- **Behavioral confidence:** re-run the replay 3× (autopilots are bistable — see planing-redirect.md)
  to confirm the board reliably holds the face with steering-only rail lift.
- **Grip baseline (the "decide after measuring" fork):** if repeats or on-device show the board
  loses the face when not actively leaning, add a small wave-referenced passive baseline on top of
  the weight-shift steering. Current evidence says not needed.
- **Magnitude (deferred).** Rail lift is now weak (peak ~4k vs ~20k pre-fix) and fires far fewer
  ticks — a light steering nudge. If it lacks steering authority on-device, revisit: the
  un-normalized `board.left` (~0.2 scale) still under-scales it (see suspect #2 in
  [rail-lift-into-wave-direction.md](rail-lift-into-wave-direction.md)); normalizing + re-tuning
  `railLiftMagnitude` would give a cleaner, stronger term.
- **On-device feel:** does leaning produce a responsive, non-oscillating carve? CSV can't answer this.
- **Threshold tuning:** `0.05` engage / `SmoothStep(0.05, 0.20)` ramp borrowed from the
  bottom-hydrofoil gate; may want independent tuning for rail feel.
- **Re-evaluate the three carve terms together** (lateral turn, rail Bernoulli lift, rail yaw
  hydrofoil), as flagged at [bottom-yaw-hydrofoil.md:296](bottom-yaw-hydrofoil.md).

## Related

- [rail-lift-into-wave-direction.md](rail-lift-into-wave-direction.md) — the rejected "steady into
  the wave" approach and *why* it was wrong (would kill bidirectional steering). This spec is the
  chosen alternative.
- [rail-lift-roll-gate.md](rail-lift-roll-gate.md) — the prior gate (submersion via
  `waveRelativeRollSin`) this supersedes for the engaged-rail decision.
- [carve-grip-via-redirect.md](carve-grip-via-redirect.md) — owns grip/anti-slip now; the reason
  steering-only rail lift can hold the face.
- Memory: `rail-lift-oscillates-not-steady-into-wave`.
