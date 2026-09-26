# Spec: Per-Actor Hydrofoil Force-Magnitude Cap

## Overview

Add a per-actor saturation on the bottom hydrofoil's pitch and yaw terms in `AFluidDynamics::calcThrustForce`. The pitch and yaw forces are computed independently and each is clamped to a configurable maximum magnitude before being summed and returned. Mirrors the existing `maxDragAmount` pattern in `calcDragForce`.

Scope: per-actor clamps on the two hydrofoil force vectors. No new force shapes, no engine-fork changes. Stacks above the existing `maxEffectiveWaterHeight` cap (force-input bound) and the engine-fork velocity ceilings (integration-output bound).

## Motivation

When the board is pitched nose-up over a wave crest, the back half of the board is much more submerged than the front (`amountUnderWater` ≈ 0.85 vs 0.21 in observed data). Both the pitch and yaw hydrofoils scale linearly with `amountUnderWater × effectiveWaterHeight` and quadratically with `v²InPlane`. At high v² this produces single-actor forces in the 40-60 kN range, with front/back asymmetry of ~4×. The asymmetric distribution × position arm = a yaw torque that overwhelms angular damping and spins the board ~600°/s into a sharp uncommanded right turn.

The asymmetric distribution is *physically correct* — a real surfboard with the tail engaged and nose airborne does experience asymmetric per-surface forces. What's wrong is the absolute force magnitude per actor: real hydrofoils stall and shed lift past a certain angle-of-attack / velocity combination. This simulation has no stall model and no bound on per-actor force, so the force grows quadratically with v² with nothing to cap it.

A per-actor force cap is the simulation's stand-in for "the surface cavitates / stalls". The asymmetric distribution is preserved (front actors still produce less force than back actors), but neither can run away.

## Design

After computing `pitchForce` and `yawForce` in `calcThrustForce`, clamp each vector's magnitude:

```cpp
if (maxHydrofoilForceAmount > 0.0f)
{
    const float pitchMag = pitchForce.Size();
    if (pitchMag > maxHydrofoilForceAmount)
    {
        pitchForce = pitchForce * (maxHydrofoilForceAmount / pitchMag);
    }
    const float yawMag = yawForce.Size();
    if (yawMag > maxHydrofoilForceAmount)
    {
        yawForce = yawForce * (maxHydrofoilForceAmount / yawMag);
    }
}
```

Same parameter clamps both pitch and yaw. Single knob to tune. Negative or zero value disables the clamp (preserves pre-spec behavior).

### Why clamp the vector magnitude, not the components

`pitchForce` has up + boardFwd + actorFwd components from independent coefficients. `yawForce` has anti-slip + forward components. Clamping the vector magnitude preserves direction while bounding amplitude. Clamping individual components would distort direction.

### Why per-force, not summed

If pitch and yaw fire simultaneously and we clamp the sum, the rocker-tilted actorFwd term (which has a strong up component on front actors) could be hidden by an opposing yaw force during the cap. Clamping each independently keeps the two physics models from masking each other.

### Default value

Anchored to observed normal-play vs spike magnitudes:

- **Normal aggressive surf:** per-actor forces in the 100-2000 N range.
- **Observed runaway spike:** 40-60 kN per actor.

Default `maxHydrofoilForceAmount = 5000.0f` (5 kN per actor) sits ~2-3× above normal aggressive surf and ~10× below the runaway. Cuts the spike to 1/10 magnitude while leaving plenty of headroom for hard carving. Tunable in the umap per actor — front/back can have different caps if the asymmetry needs further shaping.

### Interaction with other safety nets

Three progressively more aggressive bounds, all complementary:

1. **`maxEffectiveWaterHeight` (200 cm)** — caps the water-mass input. Keeps any single tick's force formula from spiking due to under-wave-wall submersion.
2. **`maxHydrofoilForceAmount` (this spec, 5 kN)** — caps the per-actor force output. Catches asymmetric distribution runaway when individual inputs are already capped but the multiplier product is still large.
3. **`MaxVelocityX/Y/ZUp/ZDown`** — caps the integrated velocity. Catches anything that slips past the force-level caps.

Each layer fires under different conditions; none replaces the others.

## Acceptance Criteria

### AC1 — Compiles, single new UPROPERTY

`AFluidDynamics::maxHydrofoilForceAmount` exposed as `UPROPERTY(EditAnywhere, BlueprintReadWrite)` with a comment referencing this spec.

### AC2 — Spike capped

Re-running `surfing-down-the-line` with the cap active produces no per-actor `Bottom Hydrofoil` or `Yaw Hydrofoil` log lines with force magnitudes above the cap × 1.01 (1% rounding margin).

### AC3 — Right-turn spike subsides

The sharp yaw transient described in the diagnosis (yaw rate ~600°/s during the wave-crest crossing window) drops materially — peak yaw rate within step 4 should fall to under ~150°/s at the suggested default cap.

### AC4 — Normal play behavior preserved

Normal-velocity surfing produces forces below the cap, so the cap doesn't fire and behavior is unchanged. Verifiable by checking that low-magnitude log lines in the cruise phase show identical force vectors before and after this change.

### AC5 — Disable preserves pre-spec behavior

Setting `maxHydrofoilForceAmount = 0.0` (or any non-positive value) skips the clamp and reproduces the pre-spec runaway.

## Open Questions / Future Work

1. **Separate caps per force type.** If pitch and yaw need very different bounds (e.g., yaw cap = 3 kN while pitch needs 8 kN for steep wave acceleration), split into `maxPitchHydrofoilForce` and `maxYawHydrofoilForce`. Defer until tuning shows the single knob isn't expressive enough.
2. **Cap the lateral turn / fin lift / rail lift.** Same shape (per-actor v² × ... force) applies to lift forces in `calcLiftForce`. If similar runaway shows up there, extend this pattern. Currently those terms haven't shown spike behavior, so out of scope.
3. **Smoothly saturate instead of hard-clamp.** A `force × (1 - exp(-cap/|force|))` shape would asymptote to the cap rather than discontinuously truncating. Smoother but adds compute and complexity. Defer until hard clamp shows visible artifacts.

## Status

- [ ] `maxHydrofoilForceAmount` UPROPERTY added to `AFluidDynamics`
- [ ] Clamp applied to `pitchForce` and `yawForce` in `calcThrustForce`
- [ ] AC2 verified (no log magnitudes exceed cap)
- [ ] AC3 verified (right-turn yaw rate subsides)
- [ ] AC4 verified (normal play forces below cap, behavior unchanged)
- [ ] AC5 verified (disable reproduces pre-spec runaway)
- [ ] `surfing-down-the-line` baseline re-approved if trajectory shifts
