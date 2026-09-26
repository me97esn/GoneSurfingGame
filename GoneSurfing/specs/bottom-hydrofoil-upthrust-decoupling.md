# Spec: Decouple Bottom Hydrofoil Upthrust from `effectiveWaterHeight`

## Overview

Remove the `effectiveWaterHeight` factor from the **upthrust** (up-axis) component of the bottom hydrofoil's pitch force. Forward components (`boardFwdForce`, `actorFwdForce`) keep their existing scaling.

The current per-actor `effectiveWaterHeight` multiplier amplifies the upthrust dramatically when the board's bottom enters the wave's deep-water column, producing a launch-out-of-the-water spike when the rails touch the breaking wave. Real hydrofoil lift physics doesn't scale with water-column-above; the `effH` factor was repurposed from the wave-mass-energy theme but doesn't belong on the lift component.

Scope: one factor removed from one term inside `AFluidDynamics::calcThrustForce`. No new properties. Re-tune `upwardsThrustCoefficient` to compensate for the removed multiplier.

## Motivation

At gameS 8.26 in `surfing-down-the-line` ("rails touch the breaking wave"), the board jumps upward at ~1g of net acceleration (`vz` grows 11 → 50 cm/s over 0.4s) and pops out of the water (`amountUnderWater` drops 0.49 → 0.14). Forensic data:

**Pitch hydrofoil force per actor at gameS 8.42 (peak of the jump):**

```
Pitch Hydrofoil CAPPED [C_136] - raw mag: 48,479 → cap:  5,000   ← back actor
Pitch Hydrofoil CAPPED [C_143] - raw mag: 25,963 → cap:  5,000
Pitch Hydrofoil CAPPED [C_144] - raw mag: 11,341 → cap:  5,000
Pitch Hydrofoil CAPPED [C_150] - raw mag:  9,368 → cap:  5,000

Pre-cap force vector on C_136: (-7901, 50742, 34153)
                                          ^^^^^^^
                                          Z = 34 kN upward
After cap to 5000: Z ≈ 2750 N upward per actor.
```

With ~10 bottom actors all capped, total upward force ≈ 20–25 kN → ~1g vertical acceleration on a ~2-3 ton board. **The cap is masking the magnitude but not the structural problem**: removing the cap exposes raw upthrusts of 50 kN per actor that would launch the board to 10g+.

### Why it spikes at the rail-contact moment

The upthrust formula at [FluidDynamics.cpp:890-944](../Source/GoneSurfing/FluidDynamics.cpp#L890-L944):

```cpp
const float commonMagPerActor = v² × sinAOA × amountUnderWater × effectiveWaterHeight;
const FVector upForce = upUnit × (upwardsThrustCoefficient × commonMagPerActor × upDot);
```

All four factors grow as the board enters the wave (data from `surfing-down-the-line`):

| factor | gameS 8.04 (pre-wave) | gameS 8.42 (rail contact) | growth |
|--------|------------------------|---------------------------|--------|
| `slopeSin` (drives effH cap) | 0.063 | 0.237 | 3.8× |
| `effectiveWaterHeight` | ~120 | 200 (capped) | 1.7× |
| `amountUnderWater` | 0.28 | 0.47 | 1.7× |
| `v²` | ~700K | ~750K | 1.07× |

Product: `1.7 × 1.7 × 1.07 ≈ 3.1×`. With more actors hitting the cap as the wave engages them, the aggregate upthrust roughly triples.

The `effectiveWaterHeight` factor is the variable doing the wrong thing here: it represents "how much water mass is around this actor" (wave-mass-energy proxy), which is appropriate for **wave-mass momentum transfer** but not for **hydrofoil lift**.

### Physical reasoning

Real-world hydrofoil lift: `L = ½ ρ v² A Cl(α)`.

- `ρ`: water density (constant — never appears in our formula because it's absorbed into the coefficient)
- `v²`: relative flow speed squared — present in our formula as `v²InPitchPlane`
- `A`: wetted surface area — present in our formula as `amountUnderWater` (fraction of actor submerged)
- `Cl(α)`: lift coefficient as function of angle of attack — present in our formula as `sinAOA × upwardsThrustCoefficient` (a linear-in-sinAOA approximation)

Notice the **depth of water below or column above the hydrofoil does not appear** in real lift physics. Lift depends only on the local flow regime, not on the bulk water mass surrounding the foil. A keel in deep open ocean and a keel in shallow water at the same speed, AOA, and immersion produce the same lift.

The `effectiveWaterHeight` factor was added to the upthrust as part of the wave-mass-energy theming, but it's the wrong proxy for lift — lift isn't a mass-momentum-transfer mechanism.

### Why this isn't fixed by lowering `maxHydrofoilForceAmount`

The cap is already firing 100% of ticks at the launch moment (every back-actor raw mag is 10× above the 5000 cap). Lowering the cap further would just shift the throttle point — the *direction* of the force (which is mostly up + forward) doesn't change, the *magnitude* just gets ratioed down. With ~10 actors all capped at the same value, the aggregate upward force still equals `(number of capped actors) × (cap × upDot)`, growing as more actors enter the wave.

The cap is the wrong tool here because it's symmetric across all axes. We don't want to throttle the *forward* thrust at the wave-contact moment — we want forward thrust to keep working. We want the **upthrust specifically** to stop amplifying with wave depth.

### Why this is different from the wave-mass-flow-drag cleanup

The `waveMassFlowDrag` change moved per-actor sampling to board-wide for the directional drag. Same shape would also help here (board-wide `effectiveH` would smooth the back-vs-front asymmetry), but it wouldn't address the **root issue**: even with a board-wide-averaged `effH ≈ 160` at gameS 8.42, the upthrust would still grow ~2.7× from pre-wave to wave-contact (because both `amountUnderWater` and `effH` grow). The real fix is to remove `effH` from the lift formula entirely — at which point board-wide vs per-actor becomes moot for the upthrust.

## Background

### What's being changed

In [FluidDynamics.cpp:890-944](../Source/GoneSurfing/FluidDynamics.cpp#L890), the bottom hydrofoil computes:

```cpp
// Current:
const float commonMagPerActor  = v² × sinAOA × amountUnderWater × effectiveWaterHeight;
const float commonMagBoardWide = v² × sinAOA × amountUnderWater × boardWideEffectiveH;

const FVector upForce       = upUnit       × (upwardsThrustCoefficient × commonMagPerActor       × upDot);
const FVector boardFwdForce = boardFwdUnit × (turnGate × forwardsThrustCoefficient      × commonMagBoardWide × boardFwdDot);
const FVector actorFwdForce = actorFwdUnit × (turnGate × actorForwardsThrustCoefficient × commonMagPerActor  × actorFwdDot);

pitchForce = upForce + boardFwdForce + actorFwdForce;
```

After this spec:

```cpp
// New:
const float commonMagUp        = v² × sinAOA × amountUnderWater;                       // ← no effectiveH
const float commonMagPerActor  = v² × sinAOA × amountUnderWater × effectiveWaterHeight; // (kept for actorFwd)
const float commonMagBoardWide = v² × sinAOA × amountUnderWater × boardWideEffectiveH;  // (kept for boardFwd)

const FVector upForce       = upUnit       × (upwardsThrustCoefficient × commonMagUp           × upDot);
const FVector boardFwdForce = boardFwdUnit × (turnGate × forwardsThrustCoefficient      × commonMagBoardWide × boardFwdDot);
const FVector actorFwdForce = actorFwdUnit × (turnGate × actorForwardsThrustCoefficient × commonMagPerActor  × actorFwdDot);
```

The forward thrust components are unchanged — they represent wave-mass-driven propulsion through hydrofoil redirection (carve coupling, see [bottom-hydrofoil-thrust.md](bottom-hydrofoil-thrust.md)), which legitimately scales with wave-mass energy.

### What stays

- `forwardDrag` (skin friction) — different mechanism, unchanged
- `waveMassThrust` — uses board-wide effH for its hydrofoil-redirect path, unchanged
- `waveMassFlowDrag` — uses board-wide effH for its wave-mass-momentum path, unchanged
- Bottom hydrofoil `boardFwdForce` — keeps `effH` (wave-mass-driven forward propulsion)
- Bottom hydrofoil `actorFwdForce` — keeps `effH` (same reasoning, with per-actor rocker direction)
- Bottom Bernoulli "lift" (downward suction) at [FluidDynamics.cpp:588-633](../Source/GoneSurfing/FluidDynamics.cpp#L588) — uses `amountUnderWater` only (already correctly *not* using `effH`), unchanged
- Yaw hydrofoil — separate path, has its own `effectiveH` factor that may have the same issue but is **out of scope** for this spec (see Open Questions)

### What changes in observable behavior

Before: upthrust amplifies with `effectiveWaterHeight` going from ~100 (pre-wave) → 200 (capped on wave). After re-tuning, that 2× amplification on the upthrust path is gone.

With `upwardsThrustCoefficient` re-tuned by ~150–200× upward (to compensate for the removed `effH ≈ 150–200` factor), the steady-state upthrust during planing should match current behavior. **What changes is the wave-contact transient**: instead of the upthrust jumping 1.7× as effH saturates, it stays flat. The board no longer pops up when the rails contact the breaking wave.

## Design

### Tuning estimate

Current `upwardsThrustCoefficient` is multiplied by `effectiveH ≈ 200` (cap value) at the wave-contact moment, and ~100–150 during normal cruising. To keep the cruising-phase upthrust at its current value, raise `upwardsThrustCoefficient` by the **inverse of the typical cruising-phase effectiveH** ≈ 100–150:

```
new_upwardsThrustCoefficient ≈ old_upwardsThrustCoefficient × 120  (approximate factor)
```

Then bisect against the cruising and wave-catching phases of `surfing-down-the-line`. Specifically:

- gameS 5.5–6.0 (pop-up + catch wave): board should still get the right amount of vertical lift to plane up onto the wave. Same vz trajectory as before within tolerance.
- gameS 6.5–7.5 (carving phase): upthrust contribution should match current behavior.
- gameS 8.0–8.6 (wave-rail contact): vz should stay flat (or grow only slightly), instead of jumping 11 → 50 cm/s.

### Why not simply lower upwardsThrustCoefficient?

That's "Option A" from the diagnosis discussion. It reduces the wave-contact spike *and* the cruising upthrust by the same factor. You lose the planing lift you need to wave-catch in the first place.

Decoupling from `effH` keeps the cruising upthrust at full strength (after re-tuning) but removes the wave-contact amplification — which is the regime-specific change you actually want.

### Naming / variable layout

The current code reuses `commonMagPerActor` for both upForce and actorFwdForce. After this spec, upForce wants its own no-`effH` version. Two natural ways:

A. Introduce a new local `commonMagUp = v² × sinAOA × amountUnderWater` and use it for upForce.
B. Inline the multiplication for upForce: `upForce = upUnit × (upCoef × v² × sinAOA × amountUnderWater × upDot)`.

(A) is cleaner — keeps the per-actor / board-wide pattern visible in the variable names, and the new `commonMagUp` has a clear physical meaning ("the per-actor lift-shape factor, sans water-column-above").

### Logging update

The Bottom Hydrofoil log line currently prints `commonMag(perActor/boardWide)`. After the change, also print `commonMagUp` so we can see all three.

## Implementation Sketch

In [FluidDynamics.cpp:890](../Source/GoneSurfing/FluidDynamics.cpp#L890):

```cpp
// Lift formula matches real hydrofoil physics: L = ½ρv²A·Cl(α).
// effectiveWaterHeight does NOT appear — water-column-above doesn't change
// local lift. effectiveH stays in commonMagPerActor / commonMagBoardWide
// because those magnitudes drive wave-mass-redirected forward propulsion,
// which is a different mechanism. See specs/bottom-hydrofoil-upthrust-decoupling.md.
const float commonMagUp = v2InPitchPlane
                        * sinAOA
                        * amountUnderWater;

const float commonMagPerActor = v2InPitchPlane
                              * sinAOA
                              * amountUnderWater
                              * this->effectiveWaterHeight;

// boardWideEffH calculation unchanged
const float boardWideEffH = FMath::Min(...);

const float commonMagBoardWide = v2InPitchPlane
                               * sinAOA
                               * amountUnderWater
                               * boardWideEffH;

// ... thrustDir calculation unchanged ...
// ... upDot/boardFwdDot/actorFwdDot calculation unchanged ...

const FVector upForce       = upUnit       * (upwardsThrustCoefficient                  * commonMagUp        * upDot);
const FVector boardFwdForce = boardFwdUnit * (turnGate * this->forwardsThrustCoefficient      * commonMagBoardWide * boardFwdDot);
const FVector actorFwdForce = actorFwdUnit * (turnGate * this->actorForwardsThrustCoefficient * commonMagPerActor  * actorFwdDot);
pitchForce = upForce + boardFwdForce + actorFwdForce;
```

Debug log format string updated to include `commonMagUp` alongside `commonMagPerActor` and `commonMagBoardWide`.

## Acceptance Criteria

### AC1 — Compiles, no new properties

One factor removed from one term. No UPROPERTY changes. No new files.

### AC2 — Wave-contact upthrust spike eliminated

At gameS 8.26 → 8.42 in `surfing-down-the-line`, vz growth significantly reduced compared to baseline. Target: vz at gameS 8.42 stays below 25 cm/s (was 46 cm/s in baseline with current effH-multiplied formula). Equivalent: board's vertical acceleration during the wave-rail-contact window stays below 0.5g (was ~1g).

### AC3 — Cruising/planing upthrust preserved

After re-tuning `upwardsThrustCoefficient` by the appropriate factor, the cruising-phase board height stays at its current value (within ±5 cm) in both flat-water `surf-straight` and the carve portion of `surfing-down-the-line` (gameS 6.5–7.5).

### AC4 — Wave-catch behavior preserved

The pop-up + wave-catch phase (gameS 5.5 → 6.5) maintains its current vz trajectory within ±20 cm/s. The board still gets pushed up onto the wave by the bottom hydrofoil during pop-up, just no longer amplified by the wave's water column above.

### AC5 — Forward thrust unchanged

`boardFwdForce` and `actorFwdForce` magnitudes per actor are unchanged (verify in debug log — only the `upForce` magnitude / `commonMagUp` is new). Carve geometry through gameS 6.5–7.5 stays within snapshot tolerance after upthrust coefficient is re-tuned.

### AC6 — Debug log shows the new factor

Bottom Hydrofoil log line includes `commonMagUp` value alongside the existing `commonMagPerActor` and `commonMagBoardWide`.

## Open Questions / Future Work

1. **Yaw hydrofoil has a similar `effectiveH` multiplier** at [FluidDynamics.cpp:1014-1018](../Source/GoneSurfing/FluidDynamics.cpp#L1014-L1018). Same physical question applies — yaw hydrofoil's anti-slip "lift" component shouldn't scale with water-column-above either, by the same reasoning. Deferred to a follow-up spec; testing this one first to validate the decoupling principle.

2. **The Bernoulli "lift" (downward suction) at the bottom** uses only `amountUnderWater` already, no `effectiveH`. This is consistent with the principle this spec applies, and validates that decoupling is the right shape. No change needed.

3. **`maxHydrofoilForceAmount` cap** behavior after this change. Once `effectiveH` is gone from upthrust, the upForce per-actor magnitude will drop substantially (rough estimate: 5–10× smaller raw values, mostly *below* the 5000 N cap). The pitch hydrofoil cap will fire less often. That's good — caps that fire constantly are masking misbehavior; we want them to fire only at genuine extremes. Re-evaluate the cap value after this spec lands.

4. **`amountUnderWater` is still per-actor** in the upthrust formula. The back-vs-front sampling asymmetry now lives only in this term (the dominant `effectiveH` amplification is gone). At ~0.28 (front) vs ~0.49 (back), the ratio is 1.75× — still some asymmetry, but much less dramatic than the previous compound effect. Defer board-wide-averaging of `amountUnderWater` until evidence shows it's still misbehaving.

5. **Tuning `upwardsThrustCoefficient`** is a Blueprint-side change. The factor change (~120×) is rough; bisect empirically.

## Status

- [x] `commonMagUp` introduced in `calcThrustForce`, used by `upForce`
- [x] `effectiveWaterHeight` removed from upthrust path
- [x] `commonMagPerActor` and `commonMagBoardWide` unchanged (drive `actorFwdForce` and `boardFwdForce` respectively)
- [x] Debug log includes `commonMagUp` value
- [ ] AC2 verified (vz at gameS 8.42 below 25 cm/s)
- [ ] AC3 verified (cruising/carving height unchanged after re-tuning)
- [ ] AC4 verified (wave-catch vz trajectory within tolerance)
- [ ] AC5 verified (forward thrust components unchanged)
- [ ] AC6 verified (debug log updated)
- [ ] `upwardsThrustCoefficient` re-tuned in BP (~120× starting estimate)
- [ ] `surfing-down-the-line` re-baselined if trajectory shifts materially
