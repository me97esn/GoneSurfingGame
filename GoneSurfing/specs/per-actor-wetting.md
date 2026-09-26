# Spec: Per-actor wetting (replace the dead/per-SC submersion gates on FluidDynamics forces)

## Status
- [x] Root cause of the dead/blunt submersion gates identified (2026-06-30)
- [x] Per-actor `AFluidDynamics::actorWetted` implemented (smoothstep on `waterColumnAbove`)
- [x] Bottom lift + up-thrust / yaw-hydrofoil / slope-thrust routed to it
- [x] **Calibrated `wettedTransitionDistance`** — measured actors rest ~37–50 cm deep; default set to 60 cm
      (8 cm pegged at 1.0). See "Calibration".
- [x] Verify the submerged-side asymmetry produces a *righting* roll torque — **confirmed** via the torque
      budget: with the roll-decouple commits in, `dragRailFlow` / `dragBottom_waveMassFlow` / `railLift` all
      register **0** roll torque and TOTAL roll torque is ~961 (mean|roll|) vs the old 84k–245k. Bottom net
      roll opposes bank.
- [x] **Re-tune the gated force magnitudes up** (2026-07-01). Measured at-rest `actorWetted` on the bottom
      actors = **mean 0.82 / median 0.83** (n≈21k thrust-log samples on `pop-up-2`), so the always-1.0 per-SC
      gate → per-actor swap dropped every gated force ~18%. Added `wettedForceCompensation` (Tuning +
      FluidDynamics, default **1.22** = 1/0.82), applied as a uniform multiplier at the five force-gate sites
      (bottom Bernoulli lift, hydrofoil up-thrust / actor+board forward thrust, yaw hydrofoil, slope thrust)
      via a `wettedForce = actorWetted * wettedForceCompensation` local — **logs still report raw
      `actorWetted`** so the L/R asymmetry diagnostic is unaffected, and the L/R *ratio* (righting signal) is
      preserved (uniform scale). **Caveat:** default 1.22 strengthens flat-water forces too → re-run + re-Approve
      the `Boards_on_flat_water` snapshot baselines (surf-straight, hang-ten, …) or set it to 1.0 there.
      **Does NOT fix the `pop-up-2` stall** — see the note in [[downline-yaw-into-wave-is-overcarve]].
- [ ] Roll out to the remaining FD force terms (the intended end state — see "Recommendation")
- [ ] Remove the now-dead per-SC `amountWetted` (member + `Buoyancy.cpp` computation) once nothing reads it

## Problem

The bottom FluidDynamics forces (Bernoulli down-lift, hydrofoil up-thrust, yaw-hydrofoil, slope-thrust)
were gated by `SharedCalculations::amountWetted`. Two independent defects made that gate useless for any
per-side (roll) behaviour:

1. **Dead / pegged at 1.0.** `amountWetted` is `smoothstep(depthBelowSurface / wettedTransitionDistance)`
   over an **8 cm** band ([Buoyancy.cpp:192–197](../Source/GoneSurfing/Buoyancy.cpp#L192)). The actors rest
   **deeper than 8 cm**, so it clamps to 1.0 — it never varies. (See [[submersion-gates-comparison]].)
2. **Per-SharedCalculations, not per-actor.** It's written by the 4 corner buoyancy actors into the shared
   SC (last-writer-wins), and there are only **two SCs (front / back)**. The left and right bottom actors
   share an SC, so they read the **same** value — it can only carry **front/back (pitch)** information,
   **never left/right (roll)**.

`amountUnderWater` (the live depth signal) is **also per-SC**, and deliberately ramps 0→1 over **~1 m** to
keep buoyancy smooth — far too gradual to mark "this rail is in the water and that one isn't". So swapping
the gates to `amountUnderWater` gives a live but blunt, still-per-SC signal (measured: bottom-lift roll
torque ≈ 0 asymmetry).

Net effect: the bottom forces have **no submerged-side asymmetry**, so they can't provide the **roll
righting** (more support under the dipped rail pushes it back up) that keeps the board level. Without it the
board rolls freely and other destabilising terms (rail-lift keel-roll) take over.

## The signal that already carries left/right

Each FD actor already computes its **own** depth at its **own** (X,Y,Z), every tick
([FluidDynamics.cpp:1708](../Source/GoneSurfing/FluidDynamics.cpp#L1708)):

```cpp
const FVector samplePos = GetActorLocation();
const float   waveZ     = waveVelocity->calculateWaveLocationAndNormalAuto(samplePos)[0].Z; // surface at (X,Y)
this->waterColumnAbove  = FMath::Max(0.0f, waveZ - samplePos.Z);                            // vertical gap, floored at 0
```

It's a plain **vertical** gap (actor Z vs wave-surface Z directly above), not a perpendicular depth — an
approximation that overstates true depth by ~1/cos(slope) on a tilted face, but fine for a *relative* L/R
comparison. Because it's sampled at the actor's own position, it **differs between the submerged and raised
rail under roll** by ≈ `2 × (lateral offset) × sin(roll)` (e.g. ~8 cm at an 8° bank, 30 cm half-width).

## Implementation

`AFluidDynamics::actorWetted` (per-actor), computed in the per-tick sampler right after
`effectiveWaterHeight`:

```cpp
const float t = FMath::Clamp(this->waterColumnAbove / this->wettedTransitionDistance, 0, 1);
this->actorWetted = t * t * (3.0f - 2.0f * t);   // smoothstep; sharp band, per-actor
```

Routed into: bottom Bernoulli lift ([:777](../Source/GoneSurfing/FluidDynamics.cpp#L777)) and the
up-thrust / yaw-hydrofoil / slope-thrust (via the local alias at the hydrofoil block). `wettedTransitionDistance`
is copied from `Tuning->wettedTransitionDistance` (live-tunable).

## Calibration (measured 2026-06-30)

Swept `wettedTransitionDistance` and read `actorWetted` on the bottom actors in steady down-the-line surf:
- **At 8 cm: all samples = 1.000 (pegged).** The actors rest far deeper than 8 cm.
- **At 100 cm: `actorWetted` 0.31–0.50, mean 0.39** → inverting the smoothstep, the bottom actors sit
  **~37–50 cm below the surface** (mean ~43 cm).

So 8 cm can't work (the "sharp ~8 cm contact band" is incompatible with these actors' resting depth). The band
must be **roughly the actors' depth** to un-peg and ramp. **Default set to 60 cm**: un-pegs all of them
(deepest ~50 cm), sits them around `actorWetted ≈ 0.7–0.9` at rest, and still ramps much sharper than
`amountUnderWater`'s ~1 m. **The real benefit is per-actor (left/right) variation, not sharpness** — the
actors are too deep for a genuinely sharp band.

Tuning notes:
- A **larger** band (~80 cm) centres the actors nearer the steepest part of the smoothstep (more L/R
  sensitivity) at the cost of lower at-rest strength; **smaller** risks pegging the deepest actors. Live-tune.
- If a genuinely *sharp* wetting is wanted, the bottom actors would need to be **placed shallower** (nearer
  the hull skin) so an 8–15 cm band could ramp across their depth — a level-placement change, not code.

## Recommendation (end state)

**This per-actor `actorWetted` is what every FD force term should use for "is my surface wetted"** — in
place of both the per-SC `amountWetted` (dead, no L/R) and `amountUnderWater` (gradual, no L/R). The per-SC
`amountWetted` member and its `Buoyancy.cpp` computation can be deleted once no consumer reads it.
`amountUnderWater` stays for what it's good at: the smooth, board-wide buoyancy ramp (and board-level
front/back gates like the pitch-damping contact gate in `SurfboardUtils`).

## Caveats

- **Re-tuning:** where the gate was effectively 1.0, it is now <1 for partially-wetted actors → the gated
  forces weaken. The lift/thrust coefficients likely need raising.
- **Vertical vs perpendicular depth:** acceptable for L/R comparison; revisit if it misbehaves on steep faces.
- **Verify righting sign:** confirm the restored asymmetry *opposes* the bank (up-thrust dominates the
  down-lift on the dipped side), not amplifies it.

## Related
- [[submersion-gates-comparison]] — why amountWetted is dead and amountUnderWater is gradual.
- [amount-wetted-split.md](amount-wetted-split.md) — the original (per-SC) amountWetted split.
- [wave-mass-drag-torque-decoupling.md](wave-mass-drag-torque-decoupling.md) — the roll investigation this came out of.
- [bottom-hydrofoil-upthrust-decoupling.md](bottom-hydrofoil-upthrust-decoupling.md) — the up-thrust terms now gated per-actor.
