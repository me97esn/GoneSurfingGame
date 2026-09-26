# Spec: Split `amountUnderWater` into a separate `amountWetted` for hydrofoil forces

## Overview

Introduce a second submersion scalar, `amountWetted`, that maps depth to a near-step contact signal (5–10 cm transition), and use it on the hydrofoil-family forces. Keep the existing `amountUnderWater` (100 cm linear ramp + sigmoid) for the buoyancy-family forces. The two scalars represent two distinct physical phenomena that share one variable today; their depth-response curves are incompatible, and trying to satisfy both with one curve is what currently makes the planing/upthrust force impossible to tune.

Scope: one new scalar plumbed through to `AFluidDynamics::calcThrustForce` (pitch + yaw hydrofoil) and the `bottomLift` term in `AFluidDynamics::calcLiftForce`. No buoyancy formulas change. Re-tune `upwardsThrustCoefficient` (and the corresponding bottom Bernoulli `liftMagnitude`) downward to compensate for the much larger multiplier reaching them in steady state.

## Motivation

Walking the rider's weight forward (the `hang-ten` autopilot) causes the nose to dip and the board to slow + turn instead of trimming forward as a real surfboard would. Diagnostic walk-through of the upthrust formulas showed the lift center is structurally already forward-of-CoM via per-actor rocker — the pitch-plane `sinAOA = flowDir · this->up` at [FluidDynamics.cpp:880](../Source/GoneSurfing/FluidDynamics.cpp#L880) reads the actor's rocker-tilted up vector, so nose actors generate v²-scaled upthrust even at zero board AOA. The mechanism is there; the magnitude is the problem.

The bottleneck is `amountUnderWater`. In normal planing the bottom skin sits roughly 5–15 cm under the surface. At that depth, the current formula stack returns a tiny fraction of its max:

```cpp
// Buoyancy.cpp:68-91
rawAmount      = -surfaceDistance / distanceWhereMaxForceShouldBeApplied;   // 100 cm ramp
radians        = (rawAmount * 180 - 90) * π/180;                            // → [-π/2, π/2]
sinusoidAmount = (sin(radians) + 1) / 2;                                    // S-curve, max derivative at 50 cm
```

Plugging the corners:

| depth (cm) | rawAmount | sinusoidAmount |
|---:|---:|---:|
|   5 | 0.05 | 0.006 |
|  10 | 0.10 | 0.024 |
|  20 | 0.20 | 0.095 |
|  30 | 0.30 | 0.206 |
|  50 | 0.50 | 0.500 |
| 100 | 1.00 | 1.000 |

So at the typical operating depth, the hydrofoil upthrust multiplier sits at ~2–10 % of its max. To make the nose hold against any meaningful weight shift, `upwardsThrustCoefficient` has to be cranked up to compensate. But the same coefficient feeds the wipeout / pop-up / trough-strike moments where `amountUnderWater` jumps to ~1.0 — and you get a 40× spike that launches the board.

That squeeze is the structural reason hang-ten can't be tuned without breaking other regimes.

### Why not just narrow the existing ramp

Two adjacent fixes were rejected by the project owner:

1. **Lower `distanceWhereMaxForceShouldBeApplied` (e.g. 100 → 20 cm).** Buoyancy becomes unstable — waves chatter contact in/out of the narrowed band, force chattering follows.
2. **Drop the sigmoid for the hydrofoil path while keeping the 100 cm ramp.** Removes the squashing in the small-depth region but the underlying ramp is still too wide to give the near-step response the hydrofoil wants; only a partial improvement, and it destabilizes buoyancy slightly because the two paths share the variable today (the change leaks into anything that still reads the unified field).

The remaining option is to give the hydrofoil path its own signal whose shape matches its physics, and leave buoyancy's signal alone.

**Update 2026-09-22: the owner took option 1 anyway** — the compiled default
`distanceWhereMaxForceShouldBeApplied` is now **30 cm**, not 100. Everything above describes the
100 cm ramp and is kept as the reasoning that was in front of that decision; the table's
`rawAmount` column is now `depth / 30`, so the sigmoid reaches 0.5 at 15 cm and saturates at 30.
Two things to watch, both named above: the chattering this section predicted, and `amountUnderWater`
pegging at 1.0 — the board rides 30–50 cm under (see `trick-scoring.md`), which is at or past the
new ramp's top, so the only live depth signal in the physics goes flat for most of a ride, the way
`amountWetted` did at 8 cm. Any local `Saved/BoardTuning/<id>.json` still holding an older value
(the shortboard's 40 on the author's PC) now sits ABOVE the default rather than below it.

## Physical reasoning

Two terms, two physical phenomena, two depth responses:

**Static buoyancy** (and horizontal-velocity buoyancy, vertical-velocity buoyancy) = `ρ × V_displaced × g`. The relevant quantity is **how much volume is submerged**. A board sinking from "skin barely touching" to "deck under water" displaces increasing volume the whole way down. A wide ramp is physically correct: each corner at different depth displaces different volume.

**Hydrofoil lift / Bernoulli suction** = `½ ρ v² A · Cl(α)`. The relevant quantity is **whether the bottom skin is wetted**. Once the skin is in contact with moving water, the full hydrodynamic force is generated regardless of how much water is above. A keel in deep ocean and a keel in shallow water at the same speed, AOA, and skin immersion produce the same lift. The wetted-area `A` is a near-step function in depth: zero just above the surface, fully wetted within a few cm below.

The same logic supported decoupling `effectiveWaterHeight` from upthrust in [bottom-hydrofoil-upthrust-decoupling.md](bottom-hydrofoil-upthrust-decoupling.md): lift physics doesn't scale with water-column-above. The current spec is the same principle applied one rung down — lift physics also doesn't scale with depth-below, only with skin contact.

## Background

### What's being changed

A new scalar `amountWetted ∈ [0, 1]` representing bottom-skin contact, with a narrow transition (~5–10 cm). Read by:

1. **Bottom hydrofoil upthrust** ([FluidDynamics.cpp:902](../Source/GoneSurfing/FluidDynamics.cpp#L902))
   `commonMagUp        = v² × sinAOA × amountWetted` (was `× amountUnderWater`)
2. **Bottom hydrofoil forward thrust, per-actor and board-wide** ([FluidDynamics.cpp:906, 916](../Source/GoneSurfing/FluidDynamics.cpp#L906))
   `commonMagPerActor  = v² × sinAOA × amountWetted × effectiveWaterHeight`
   `commonMagBoardWide = v² × sinAOA × amountWetted × boardWideEffH`
3. **Bottom Bernoulli suction (bottomLift)** ([FluidDynamics.cpp:588](../Source/GoneSurfing/FluidDynamics.cpp#L588))
   `liftAmount = v² × Cl × cosYaw × liftMagnitude × amountWetted`
4. **Yaw hydrofoil** ([FluidDynamics.cpp:1026](../Source/GoneSurfing/FluidDynamics.cpp#L1026))
   uses `amountWetted` in place of `amountUnderWater`.

### What stays

All of these continue to read `amountUnderWater` (the wide-ramp + sigmoid field):

- `Buoyancy::calculateBuoyancyForce` — basic float buoyancy, `horizontalVelocityBuoyancy`, `verticalVelocityBuoyancy` (all volume-based; the wide ramp matches the physics)
- `WeightDistribution` — does not read submersion
- `lateralTurnForce` — intentionally not gated by submersion today (board-wide rotation coupling; [FluidDynamics.cpp:664-666](../Source/GoneSurfing/FluidDynamics.cpp#L664-L666))
- `railLift` / `finLift` — out of scope for v1; rails are a partial-buoyancy case (revisit if rail behavior degrades; see Open Questions). Fins are fully submerged during planing — already not gated by amountUnderWater.

### What changes in observable behavior

In steady-state planing at ~10 cm bottom-skin depth:

- `amountUnderWater` continues to read ~0.024.
- `amountWetted` reads ~1.0.

To keep current planing forces the same after the split, the hydrofoil-family **coefficients must be re-tuned downward** by roughly the inverse of the depth-dependent multiplier they used to see at cruise (very rough estimate: `1 / 0.024 ≈ 40×` reduction, but the real factor depends on the actual cruise depth distribution; bisect empirically).

After re-tuning:

- **Cruise upthrust matches current behavior** at the typical 5–15 cm operating range — `amountWetted ≈ 1.0` × small coefficient = same force as before's `0.024 × large coefficient`.
- **Wipeout / pop-up / trough-strike spikes are flattened** — `amountWetted` never exceeds 1.0, while `amountUnderWater` previously surged from ~0.024 → ~1.0 (a ~40× spike). Now the multiplier sits at 1.0 across the whole range; only `v²` and `sinAOA` swing.
- **Hang-ten holds the nose** — at full forward weight, the nose dips a little, AOA on the rocker'd nose actors rises (`sin(rocker + θ)`), and the upthrust × moment-arm balances the gravity torque from the shifted CoM with enough authority to settle at small θ instead of running away to the 30° gate.

## Design

### Curve for `amountWetted`

Two reasonable shapes — pick (A) for v1, (B) if jitter shows up:

A. **Smoothstep over a narrow band.** A 5–10 cm Hermite step. `t = clamp(-surfaceDistance / wettedTransitionDistance, 0, 1)`; `amountWetted = t² (3 - 2t)`. One UPROPERTY: `wettedTransitionDistance` (default 8 cm).

B. **Hard step with small hysteresis.** `1` if `surfaceDistance < -1 cm`, `0` if `> +1 cm`, last value in between. Slightly cheaper, more robust to a single-frame wave-tip flicker, but introduces state.

Start with (A). Reach for (B) only if surface-ripple chatter shows up in the snapshot tests.

### Where to compute it — physical correctness vs. mobile cost

Calculating `amountWetted` in `FluidDynamics` gives the most physically correct result; calculating it in `SharedCalculations` or `Buoyancy` is less correct. Mobile performance is crucial, so the location decision is a trade-off between correctness and per-tick cost — profile before committing.

1. **In each `FluidDynamics` actor** (~20 evaluations per tick) — **most physically correct**.
   - **Cost:** ~20 wave-height lookups per tick per board. Highest cost; on mobile this could be the bottleneck.
   - **Behavior:** true per-actor `amountWetted`. Nose-region samplers see `1.0` while tail-region samplers see `0.0` during a strong forward weight shift; the upthrust gradient along the board's length becomes a real pitch-distribution signal, not a 2-bucket one. Restores per-actor pitch resolution that the 2-SC topology collapsed.
   - **When to pick:** when mobile profiling shows enough headroom to absorb the extra ~16–18 lookups vs. option (2)/(3), and the finer pitch resolution materially improves trim behavior.

2. **In `SharedCalculations`** (one value per SC, so 2 board-wide values: front-SC, back-SC) — **less physically correct**.
   - **Cost:** 2 evaluations of `waveHeight->calculateWaveLocationAndNormal()` per tick (one per SC). Lowest added cost.
   - **Behavior:** front-half FD actors all read the same value; back-half FD actors read another. Same coarseness as the current `amountUnderWater` post the 2-SC topology change. Captures front-vs-back pitch distribution but not finer.
   - **When to pick:** when mobile budget is tightest and the front/back split is sufficient resolution for the planing forces.

3. **In `Buoyancy` (one per corner)**, last-writer wins on the shared field — **less physically correct (same observable result as (2))**.
   - **Cost:** 4 lookups (one per corner), but `Buoyancy::Tick` already runs and already calls `calculateWaveLocationAndNormal()` at the corner. Adding `amountWetted = narrowStep(surfaceDistance)` is essentially free on top of the existing lookup.
   - **Behavior:** identical to (2) in terms of what FD reads, because all 4 corner writes collapse onto the two shared SC fields (front-SC, back-SC) — last-writer-wins. So the per-corner detail is lost the moment FD reads it.
   - **When to pick:** for code-locality / consistency with existing pattern. **Same observable effect as (2)** but with the existing structure.

Decision path: profile the per-tick cost of (1) on a representative mobile target. If it fits the budget, prefer (1) for physical correctness. Otherwise fall back to (2) or (3) — they're functionally interchangeable, pick whichever is cleaner to wire.

### Naming

`amountWetted` is concise and contrasts cleanly with `amountUnderWater`. If a future refactor wants to be more explicit, candidates: `bottomSkinContact`, `hydrofoilWetting`.

## Implementation Sketch

### New scalar in `SharedCalculations` (option 1)

`SharedCalculations.h`:
```cpp
UPROPERTY(VisibleAnywhere, BlueprintReadOnly)
float amountWetted = 0.0f;
```

`SharedCalculations.cpp` — compute in the same Tick that already computes `amountUnderWater`-adjacent state, alongside `amountUnderWater` plumbing. Need access to a representative `componentZ` for the SC location (which is at the front-half or back-half centroid of the board) and the wave Z there. Pattern mirrors `Buoyancy::calcAmountUnderWater` but with a narrow transition:

```cpp
const float surfaceDistance = componentZ - waveZ;  // negative => under water
if (surfaceDistance >= 0)
{
    this->amountWetted = 0.0f;
}
else
{
    const float t = FMath::Clamp(-surfaceDistance / this->wettedTransitionDistance, 0.0f, 1.0f);
    this->amountWetted = t * t * (3.0f - 2.0f * t);  // smoothstep
}
```

with `wettedTransitionDistance` as a new UPROPERTY (default 8 cm).

### Option 2 (in `Buoyancy`)

Inside `ABuoyancy::calculateBuoyancyForce` after the existing wave lookup, compute `amountWetted` from the **same** `surfaceDistance` already calculated, then assign `this->sharedCalculations->amountWetted = ...`. Last-writer-wins of the 4 corner buoyancy actors per SC, exactly mirroring the existing `amountUnderWater` write pattern. Zero new wave lookups.

### `FluidDynamics` reads

Where `amountUnderWater` is used today in the hydrofoil/upthrust/Bernoulli paths, read `this->sharedCalculations->amountWetted` instead:

```cpp
// FluidDynamics.cpp line ~866:
const float amountUnderWater = this->sharedCalculations->amountUnderWater;   // kept for any non-hydrofoil reader, if any
const float amountWetted     = this->sharedCalculations->amountWetted;       // new

// line ~588 (bottomLift):
liftAmount = abs(v² * Cl * trueCosYaw * liftMagnitude * amountWetted);

// line ~902 (pitch hydrofoil commonMagUp):
const float commonMagUp = v2InPitchPlane * sinAOA * amountWetted;

// line ~906 (pitch hydrofoil commonMagPerActor):
const float commonMagPerActor = v2InPitchPlane * sinAOA * amountWetted * effectiveWaterHeight;

// line ~916 (pitch hydrofoil commonMagBoardWide):
const float commonMagBoardWide = v2InPitchPlane * sinAOA * amountWetted * boardWideEffH;

// line ~1026 (yaw hydrofoil):
yawMag = ... * amountWetted * ... ;
```

Audit FluidDynamics for any remaining `amountUnderWater` reads after the change — anything not in the hydrofoil/Bernoulli family that reads it should keep reading the old field.

### Debug logging

Add `amountWetted` to the existing Bottom Hydrofoil log line ([FluidDynamics.cpp:965-967](../Source/GoneSurfing/FluidDynamics.cpp#L965-L967)) and to the bottomLift "FIRED" log line ([FluidDynamics.cpp:594-597](../Source/GoneSurfing/FluidDynamics.cpp#L594-L597)).

### Re-tuning the coefficients

These must come down in BP to compensate for the much larger multiplier:

- `upwardsThrustCoefficient` on every bottom FluidDynamics actor
- `forwardsThrustCoefficient` and `actorForwardsThrustCoefficient` on every bottom FluidDynamics actor
- `liftMagnitude` (the Bernoulli suction coefficient) on every bottom FluidDynamics actor
- `yawHydrofoilCoefficient` on every bottom FluidDynamics actor

Starting estimate: previous cruise `amountUnderWater ≈ 0.02–0.10`, new cruise `amountWetted ≈ 1.0`, so divide each coefficient by `1 / cruise_amountUnderWater` (rough: ÷ 10× to ÷ 40×). Bisect against snapshot tests.

## Acceptance Criteria

### AC1 — Compiles, one new UPROPERTY

`amountWetted` exists as a `SharedCalculations` field; `wettedTransitionDistance` exists as a UPROPERTY (default 8 cm) on whichever actor type computes it. No new dependencies; no new files.

### AC2 — Hydrofoil/Bernoulli paths now read `amountWetted`

Grepping `FluidDynamics.cpp`, no occurrence of `amountUnderWater` remains in `calcThrustForce` or the `bottomLift` branch of `calcLiftForce`. Buoyancy formulas in `Buoyancy.cpp` continue to read `amountUnderWater` unchanged.

### AC3 — Cruise behavior preserved after re-tuning

After coefficient re-tuning, `surf-straight` and the carve phase of `surfing-down-the-line` produce trajectories within snapshot tolerance of their re-approved baselines.

### AC4 — Hang-ten holds the nose

In the `hang-ten` autopilot (or manual playthrough with the weight shifted to the nose), the board does **not** dive past the bottomLift gate angle (`absTrueCosPitch < 0.87`, ~30°). Equilibrium pitch with full forward weight shift stays in a reasonable surfing range (target: ≤ 15° nose-down at planing speed). Subjective: "the surfer can walk to the nose without the board nose-diving."

### AC5 — Wipeout / spike scenarios stay sane

Pop-up moment and any scenario where the board briefly fully submerges no longer produces the ~40× upthrust spike. Specifically: peak vertical acceleration during pop-up and during the rail-contact moment in `surfing-down-the-line` (gameS 8.04 → 8.42) does not exceed the existing baseline by more than 20%.

### AC6 — Debug logs include the new field

`Bottom Hydrofoil` and `Bottom Lift FIRED` log lines both print `amountWetted` alongside their existing fields.

## Open Questions / Future Work

1. **Rails.** `railLift` ([FluidDynamics.cpp:~701](../Source/GoneSurfing/FluidDynamics.cpp)) is `|sinYaw| × cosYawFromForward × v² × coef × amountUnderWater`. Physically a rail is wetted in a near-step fashion too (rail surface in water, or not). But rails are also partial-displacement contributors. Defer until we see whether the hydrofoil split alone fixes hang-ten and doesn't introduce roll regressions. If rails feel like they engage too softly during a roll, revisit and decide.
2. **Per-actor option (3).** Profile mobile first. If 2-bucket resolution is insufficient for fine pitch control, do the per-FD-actor `amountWetted` calculation, accepting the ~20× wave-lookup cost.
3. **`wettedTransitionDistance` default.** 8 cm is a starting guess matching realistic board thickness. May need to widen (15–20 cm) if jitter shows up in calm-water idle, or narrow (3–5 cm) for crisper feedback if the smoothstep is masking pitch response. Bisect.
4. **Buoyancy still uses last-writer-wins on `amountUnderWater`** ([Buoyancy.cpp:145](../Source/GoneSurfing/Buoyancy.cpp#L145)). Not addressed by this spec. Worth a separate look — see [[project_actor_topology_per_board]] — but the wide ramp + sigmoid makes the front/back delta small enough that it's not the bottleneck for hang-ten.
5. **`nose-dive-bug.md` is partially stale.** Its claim that per-actor `amountUnderWater` causes a bottomLift positive-feedback loop predates the 2-SC topology collapse. After this spec lands, that destabilizer story is fully retired (bottomLift will use board-wide `amountWetted` which is near-1.0 across both SCs during planing). Update or close out `nose-dive-bug.md` when this spec is implemented.

## Status

- [x] `amountWetted` field added to `SharedCalculations`
- [x] `wettedTransitionDistance` UPROPERTY added (default 8 cm)
- [x] Computation wired (option 3 — in `Buoyancy::calculateBuoyancyForce`, zero added wave lookups; reassess if 2-bucket front/back resolution proves insufficient)
- [x] `calcThrustForce` (pitch + yaw) reads `amountWetted` instead of `amountUnderWater`
- [x] `calcLiftForce` `bottomLift` branch reads `amountWetted`
- [x] Buoyancy formulas verified unchanged (still read `amountUnderWater`)
- [x] Debug logs include `amountWetted`
- [x] Hydrofoil coefficients re-tuned in BP (user-confirmed 2026-06-01; baselines re-passed after re-tune)
- [x] AC3 verified (cruise within snapshot tolerance — all 4 baselined autopilots OK 2026-06-01)
- [ ] AC4 verified (hang-ten holds) — **NOT YET**: post-wetted run still shows pitch min -23.6° and forward-velocity collapse. Needs further BP tuning or escalation to option 1 (per-FD-actor) for per-actor pitch resolution.
- [x] AC5 verified (wipeout/pop-up spikes don't worsen — pop-up trajectory matches baseline within tolerance)
- [ ] `nose-dive-bug.md` updated / closed once verified

## Related

- [[bottom-hydrofoil-upthrust-decoupling]] — same principle (decouple lift physics from non-lift factor) one rung up, removed `effectiveWaterHeight` from upthrust.
- [[nose-dive-bug]] — partial-fix history (`horizontalVelocityBuoyancyFrontBias`); this spec retires that workaround's root cause.
- [[per-actor-vs-board-wide-sampling]] — same coarseness question (per-FD vs board-wide) for forward thrust; informs the option-1-vs-3 choice here.
