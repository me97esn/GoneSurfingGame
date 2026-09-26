# Spec: Lateral Turn Force — Decouple from Per-Actor `amountUnderWater`

## Overview

Remove the `amountUnderWater` factor from the lateral-turn force formula. The lateral-turn force represents the board-wide carving response to a rolled-rail engaging the water surface, but the current formula multiplies it by the **per-actor** submersion (`amountUnderWater`), which varies along the board's length depending on which end is deeper. The side effect is exactly opposite to the force's intent: when the board is pitched up (tail submerged, nose out of water), the nose's lateral-turn contribution collapses to ≈ 0, weakening the carve at exactly the moment it should be strongest.

Scope: single multiplicative factor removed from `calcLiftForce`'s lateral-turn block in `AFluidDynamics`. No new properties. Tuning step (lower `lateralTurnCoefficient` to compensate for the removed attenuation) included.

## Motivation

The lateral-turn force ([FluidDynamics.cpp:679-710](../Source/GoneSurfing/FluidDynamics.cpp#L679)) models a single board-wide phenomenon: the rolled board's dipped rail catching the water surface, redirecting the board sideways. The formula currently is:

```cpp
float lateralAmount = absWaveRollSin
                    × absSinPitch
                    × relWaterVelMag
                    × AmountPlaning
                    × amountUnderWater       // ← the term being removed
                    × lateralTurnCoefficient;
```

`amountUnderWater` is computed per-actor from the local water-column sample at each FluidDynamics bottom actor's position. With a ~1m-long board, the front and back bottom actors can see very different local water depths.

### The pop-up scenario (where this misbehaves)

During pop-up / wave-catching, the board is pitched nose-up: tail submerged, nose above water. Per-actor `amountUnderWater` (snapshot data from `surfing-down-the-line`, step 3 "Pop up"):

| actor | `amountUnderWater` |
|---|---|
| `bottom_left_back` | ~0.9 |
| `bottom_left_middle` | ~0.5 |
| `bottom_left_front` | ~0.1 |

Lateral-turn force scales linearly with this. Result: the nose actors contribute ≈ 10% of their formulaic strength, the tail contributes ≈ 90%. The lateral-turn force distribution along the board is **weighted toward the tail**.

That's the opposite of what the force is supposed to model. A carving turn pivots around a point near the back of the board, with the nose swinging across the water — the **nose** is where the rail engagement converts board roll into yaw rotation, because the nose has the largest moment arm from the rotation point. By gating the front contribution on local submersion, the formula throws away the part of the lateral force that produces the most useful yaw torque.

Empirically (snapshot data 2026-05-26): during the gameS 6.0 → 7.5 carve, the front bottom actors fire lateral-turn at ~30% of the tail's magnitude despite being the same distance off-axis. The result is a carve that "drags from the back" instead of "pivots through the nose."

### The deeper conceptual issue

`amountUnderWater` makes sense as a gate for forces that **depend on per-actor surface contact**: skin-friction drag scales with wetted area, buoyancy scales with displaced volume, etc. The lateral-turn force is none of those. It's modeling **the entire board's rail acting as a rotation-coupling surface against a single body of water**. Whether the nose's bottom actor is exactly at the waterline or 30 cm above it doesn't change the physics of the rolled board's carving — what matters is that the rolled rail is in the water somewhere along the board's length. That condition is already captured by the existing `absWaveRollSin > 0.001` gate and the board-wide `AmountPlaning` factor (which measures the whole board's planing state, not a single actor).

`amountUnderWater` was likely added defensively to prevent the force from firing on a board fully out of water, but `AmountPlaning` already covers that case — a board that's not in the water doesn't have a planing force feedback loop active.

## Background

### What `amountUnderWater` represents (so we know what we're losing)

Per-actor submersion fraction (0–1) derived from the local water-column sample at each FluidDynamics actor position. Smooth-varying from 0 (actor fully above water) to 1 (actor fully submerged). Used by:

- Bottom Bernoulli lift ("downward suction" — only makes sense when actor is in water)
- Bottom hydrofoil thrust (pitch and yaw — flow only deflects against a wetted surface)
- Yaw hydrofoil (same reasoning)
- Wave-slope gravity supplement (gates gravity contribution to in-water portion)

Each of those cases is genuinely per-actor: they model "flow past this specific surface." Lateral turn does not — it models a board-wide rotation coupling.

### Other formulas with the same shape get to stay

- `AmountPlaning`: stays. It's a board-wide planing state, not a per-actor submersion.
- `absWaveRollSin`: stays. The "is the board rolled" gate.
- `absSinPitch`: stays. Provides the rocker-amplification the existing comment notes ("amplifies via rocker — nose actors have higher pitch") — though see Open Question #2 about whether this also distorts the per-actor distribution.
- `relWaterVelMag`: stays. The water-speed factor.

After removal, the formula represents: "rolled board × pitched board × moving through water × in planing state × coefficient." That's the full intent of the lateral-turn force as originally documented.

### Compounding with related issues (out of scope but flagged)

The user has separately identified that the *lift* forces (rail lift, in particular) include `effectiveWaterHeight` while lateral turn does not — a different asymmetry, also producing distortions during wave-hit. That fix is intentionally separate from this spec. This spec only addresses the `amountUnderWater` distortion in the lateral-turn formula's length-wise distribution.

## Design

### The change

In [FluidDynamics.cpp:688-693](../Source/GoneSurfing/FluidDynamics.cpp#L688-L693), remove the `amountUnderWater` factor:

```cpp
// Before
float lateralAmount = absWaveRollSin
                    * absSinPitch
                    * relWaterVelMag
                    * AmountPlaning
                    * amountUnderWater         // ← remove this line
                    * lateralTurnCoefficient;

// After
float lateralAmount = absWaveRollSin
                    * absSinPitch
                    * relWaterVelMag
                    * AmountPlaning
                    * lateralTurnCoefficient;
```

The debug log line ([FluidDynamics.cpp:704-708](../Source/GoneSurfing/FluidDynamics.cpp#L704-L708)) doesn't print `amountUnderWater` today, so no log-format change.

The `AddPlaningForce(lateralTurnForce)` call at [line 700](../Source/GoneSurfing/FluidDynamics.cpp#L700) is unaffected — it's downstream of the multiplication.

### Re-tuning `lateralTurnCoefficient`

Removing `amountUnderWater` typically increases the lateral-turn magnitude by `1 / mean(amountUnderWater)`. Across the carve window of `surfing-down-the-line` (gameS 6.0 → 7.5), the bottom actors' `amountUnderWater` averages ≈ 0.40 (snapshot data 2026-05-26). So the un-attenuated magnitude is ≈ 2.5× higher.

Compensate by reducing `lateralTurnCoefficient` from its current value to ≈ 40% of it. The starting point: if current coefficient is `K`, set to `0.40 × K`. Then bisect against the carve baseline.

The carve feel target: the gameS 6.0 → 7.5 trajectory matches the post-`finLiftMagnitude`-tuning baseline within snapshot tolerance. Specifically: peak yaw angular rate during the carve, and the gameS at which the carve apex is reached, should stay within ±1° and ±50 ms of the baseline respectively.

The pop-up feel target: during the pop-up (gameS ~5.9 → 6.0 in `surfing-down-the-line`), the board should respond more visibly to weight-shift input than before — the front lateral-turn contribution comes alive when the nose is out of water, which it wasn't before.

## Acceptance Criteria

### AC1 — Compiles, no new properties

Single multiplicative factor removed from `calcLiftForce`. No UPROPERTY changes, no new build dependencies.

### AC2 — Pre-tuning behavior change is monotonic

Before re-tuning, the lateral-turn force magnitude is uniformly larger across all bottom actors. No actor's lateral force decreases. Verify: debug log "lateralAmount" values are higher than baseline at every timestamp; sign of `lateralTurnForce` direction is unchanged.

### AC3 — After re-tuning, carving trajectory matches baseline

With `lateralTurnCoefficient` bisected to the new value, the `surfing-down-the-line` carve (gameS 6.0 → 7.5) stays within `±1°` peak yaw angular rate and `±50 ms` carve-apex timing vs. the baseline. Use the existing snapshot test for this — re-baseline only if the AC is met for a stable coefficient.

### AC4 — Pop-up phase fires nose lateral-turn

During step 3 "Pop up" (board pitched nose-up), debug log shows non-trivial lateral-turn force on the front bottom actors (`bottom_*_front`, `bottom_*_middle-front`). Previously these were ≈ 0 due to `amountUnderWater ≈ 0`. Target: front actors' `lateralAmount` is at least 50% of the back actors' `lateralAmount` at the same timestamp during pop-up (vs current ~10%).

### AC5 — Flat-water surf-straight unchanged

`surf-straight` snapshot trajectory unchanged after re-tuning. On a level board moving straight, `absWaveRollSin ≈ 0` regardless of the formula, so this AC is structurally satisfied — but verify the snapshot still passes to confirm no incidental side effects (e.g., from a buggy implementation).

### AC6 — Total lateral turn force on a fully-submerged board

For a hypothetical scenario where every bottom actor has `amountUnderWater = 1.0` (board fully submerged): the new formula produces the same lateral-turn force as the old formula × `(1 / amountUnderWater) = 1`. So in this corner case the new and old formulas agree (after re-tuning to the same total). Sanity-check that the deep-water carve trajectory in `surfing-down-the-line` (anywhere `amountUnderWater ≈ 1.0` at all bottom actors) matches the baseline.

## Open Questions / Future Work

1. **Should `absSinPitch` also be reconsidered for the same reason?** The existing comment ("amplifies via rocker — nose actors have higher pitch") suggests pitch is intentionally per-actor to weight the nose. But pitch should be a *board* property, not a per-actor property — every actor on a rigid board sees the same board pitch, the per-actor `sinPitchAngleOfAttack` is an angle-of-attack computation that includes the flow direction at each actor. If the goal is "nose contributes more because of rocker," that's better modeled as a rocker-curvature multiplier (each actor's position along the board × rocker) than via the angle-of-attack to local flow. Deferred — separate concern.

2. **Should `amountUnderWater` be replaced by a board-wide "any-bottom-in-water" gate?** As written, the new formula has nothing preventing lateral turn from firing when the board is fully out of water. `AmountPlaning` covers the planing case but a board that's airborne and rolled would still see the lateral-turn term try to fire (with non-zero waveRollSin × sinPitch). The proper gate is probably a board-wide check that *some* bottom is in water, not the per-actor check. Defer until a test case actually exposes this edge — typically planing being zero already handles the airborne case.

3. **Connection to `effectiveWaterHeight` removal in rail/bottom lift** — the user has separately identified that the lift forces include `effectiveWaterHeight` (wave-mass proxy), which causes the carving response to balloon during wave-hits. That fix is a separate spec. This one is a prerequisite in spirit (both are about decoupling the carving-response forces from local water-depth proxies), but they don't depend on each other.

## Status

- [x] `amountUnderWater` factor removed from lateral-turn formula in `calcLiftForce`
- [ ] `lateralTurnCoefficient` re-tuned to keep carve feel (target ~0.40× of current)
- [ ] AC2 verified (pre-tuning: lateral force uniformly larger)
- [ ] AC3 verified (post-tuning: carve trajectory within tolerance)
- [ ] AC4 verified (pop-up: front actors fire ≥ 50% of back's magnitude)
- [ ] AC5 verified (`surf-straight` baseline unchanged)
- [ ] AC6 verified (deep-water carve unchanged)
- [ ] `surfing-down-the-line` re-baselined if trajectory shifts within AC3 tolerance
