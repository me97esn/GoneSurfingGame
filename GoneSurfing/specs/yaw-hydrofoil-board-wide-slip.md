# Spec: Board-Wide sinSlip for Yaw Hydrofoil

## Overview

Make the yaw hydrofoil's `sinSlip` calculation read a board-wide `relativeWaterVelocity` (averaged from the front and back `ASharedCalculations` actors) rather than each FluidDynamics actor's own per-SC value. Eliminates a sign-flip in `sinSlip` between front and back SCs that produces a spurious yaw torque turning the board toward the wave.

Scope: `calcThrustForce` yaw hydrofoil block in `AFluidDynamics::calcThrustForce`. Adds `otherHalfSharedCalculations` cross-reference between the two SC actors so either SC can compute the board-wide average. The yaw hydrofoil's *magnitude inputs* (effectiveH, amountUnderWater) stay per-actor; only the *direction-determining* `sinSlip` becomes board-wide.

## Motivation

The 2026-05-22 diagnosis of the "board sharply yaws toward the wave" event found this signature in the log:

| time | actor | sinSlip | force_x |
|---|---|---|---|
| 19.117 | left-back | **+0.066** | +510 |
| 19.118 | right-back | **+0.071** | +339 |
| 19.118 | other backs | +0.066 | +510 |
| **19.127** | left-back | **-0.026** | **-177** |
| **19.127** | right-back | **-0.028** | **-120** |

The back-SC's `sinSlip` crossed zero in ~10 ms. The yaw hydrofoil's anti-slip direction depends on `sign(sinSlip)`, so this flips the back anti-slip force by 180° instantly. Meanwhile the front-SC's `sinSlip` stays solidly positive (+0.6 throughout). Before the flip: front and back both push the board's bottom toward the wave (no yaw torque from this asymmetry). After the flip: front pushes toward the wave, back pushes *away* from the wave → instantaneous front-back force opposition → yaw torque turning the nose toward the wave.

This is exactly the [per-actor-vs-board-wide](per-actor-vs-board-wide-sampling.md) failure mode, but with a subtler trigger. The yaw hydrofoil's *direction-determining* quantity (`sign(sinSlip)`) is computed from a per-actor input (`relativeWaterVelocity` via the per-half SC), and small per-SC differences become catastrophic when the front-vs-back signs disagree. The board's slip past the water is physically a *board-wide* property — one slip angle for one rigid board — so sampling it at two positions and treating them as independent is the discretization artifact.

The board is roughly 1.5 m long; the two SC actors are at front- and back-half positions ~75 cm apart. Wave-surface flow varies on a similar spatial scale, so each SC's local `relativeWaterVelocity` can legitimately differ — but the *signed slip angle* of the rigid board through the water should be one value, computed from one representative flow.

## Design

### Cross-reference between the two SCs

Add a nullable pointer on `ASharedCalculations`:

```cpp
/** The OTHER half-board's ASharedCalculations actor. Wired in ASurfboardUtils::BeginPlay
 *  so each SC knows its sibling. Used by yaw hydrofoil to compute a board-wide slip
 *  signal rather than each half-board's local one — see specs/yaw-hydrofoil-board-wide-slip.md
 *  and specs/per-actor-vs-board-wide-sampling.md. */
UPROPERTY(VisibleAnywhere, BlueprintReadOnly)
ASharedCalculations* otherHalfSharedCalculations = nullptr;
```

In `ASurfboardUtils::BeginPlay`, link the two SCs:

```cpp
if (this->sharedCalculationsFront && this->sharedCalculationsBack)
{
    this->sharedCalculationsFront->otherHalfSharedCalculations = this->sharedCalculationsBack;
    this->sharedCalculationsBack->otherHalfSharedCalculations  = this->sharedCalculationsFront;
}
```

### Board-wide sinSlip computation

In the yaw hydrofoil block in `AFluidDynamics::calcThrustForce`, replace the `relVel` used for the `sinSlip` calculation with the average across both SCs:

```cpp
const FVector relVelBoardWide = this->sharedCalculations->otherHalfSharedCalculations
    ? 0.5f * (this->sharedCalculations->relativeWaterVelocity
              + this->sharedCalculations->otherHalfSharedCalculations->relativeWaterVelocity)
    : this->sharedCalculations->relativeWaterVelocity;

const FVector upUnit = this->up.GetSafeNormal();
const FVector relVelInBottomPlane = relVelBoardWide - FVector::DotProduct(relVelBoardWide, upUnit) * upUnit;
// ... rest of the yaw hydrofoil block uses relVelInBottomPlane and sinSlip from board-wide flow
```

The board-wide `relVel` is used for:
- `relVelInBottomPlane` (projection into the bottom plane)
- `flowDirYaw` (direction)
- `sinSlip` (signed slip via dot product with leftUnit)
- `v²InBottomPlane` (magnitude squared)

Everything else in the yaw hydrofoil block (`amountUnderWater`, `effectiveH`, `coef`, the actor's local `up`/`left`/`forwards` direction vectors) stays per-actor.

### Falls back gracefully

If `otherHalfSharedCalculations` is null (e.g., during early frames before wiring, or a misconfigured umap with only one SC), the formula falls back to the actor's own SC value — same as today's behavior.

### Why average vs sample-at-center

Either approach works. Averaging is simpler — no new wave-velocity sampling needed, just uses what the SCs already compute. The result is approximately what we'd get from sampling at the geometric midpoint between the two SCs, which is approximately the board center.

### Why only the yaw hydrofoil, not the pitch hydrofoil

The pitch hydrofoil's `sinAOA` comes from the *actor's own up axis* (rocker-tilted), not from the SC's `left`. Front actors have +Y rotation-up; back actors have -Y rotation-up. So `sinAOA` is *genuinely* per-actor for the pitch hydrofoil, driven by rocker geometry — not by per-SC sampling of flow. Leave per-actor.

The yaw hydrofoil's `sinSlip` is `flow · left`, where `left` is the SC's left (board-wide-ish per half). The asymmetry comes from per-SC `flow`, not from rocker. Make board-wide.

## Acceptance Criteria

### AC1 — Compiles, cross-references wired

After build, log inspection or BP introspection shows `sharedCalculationsFront->otherHalfSharedCalculations == sharedCalculationsBack` and vice versa after `BeginPlay`.

### AC2 — All bottom actors see the same sign(sinSlip)

In a `surf.debug.flags 'thrust'` log during the wave-touching window, the `sinSlip` values logged by front-side and back-side bottom actors are identical (or differ only in numerical noise — they should be the same averaged value). Specifically: the back actors no longer show their own per-SC sinSlip but the averaged board-wide value.

### AC3 — Yaw reversal at the wave-touch moment is reduced

`surfing-down-the-line` snapshot at the previously-diagnosed event window (gameSeconds 8.05-8.5): peak yaw rate during the reversal drops measurably from the current ~30°/0.04s (~750°/s peak) to a much smaller value, and total yaw delta across the window drops from ~70° to under 30°.

### AC4 — Disabled fallback preserves pre-spec behavior

If `otherHalfSharedCalculations` is null, the yaw hydrofoil uses the actor's own SC — bit-for-bit identical to today. Verifiable by temporarily nulling the wiring and confirming snapshot reproduces.

### AC5 — Straight-line behavior unchanged

`surf-straight` snapshot trajectory unchanged. On flat water with the board aligned, front-SC and back-SC see roughly identical `relativeWaterVelocity`; averaging gives essentially the same value. So no behavioral change for the straight case.

## Open Questions / Future Work

1. **Should `relativeWaterVelocity` itself be board-wide for *all* uses?** The yaw hydrofoil is the only place we've identified where the sign-flip matters. Other forces using `relativeWaterVelocity` may have similar latent issues but haven't been observed yet. Don't broaden the change preemptively; revisit if a future tuning pass surfaces another sign-flip-driven artifact.
2. **Sample at board center instead of averaging.** Tide is alternative implementation — sample wave velocity at the surfboard's `GetActorLocation()` and use that. More accurate at the cost of an extra wave-velocity sample per tick. Averaging is cheaper and approximately equivalent for boards that aren't huge.

## Status

- [ ] `otherHalfSharedCalculations` UPROPERTY added to ASharedCalculations
- [ ] ASurfboardUtils::BeginPlay wires the cross-references
- [ ] Yaw hydrofoil block in calcThrustForce uses board-wide relVel for the slip calculation
- [ ] AC1 verified (cross-references wired)
- [ ] AC2 verified (all bottom actors share sinSlip in debug log)
- [ ] AC3 verified (yaw reversal reduced)
- [ ] AC4 verified (fallback works if pointer is null)
- [ ] AC5 verified (surf-straight unchanged)
- [ ] [per-actor-vs-board-wide-sampling.md](per-actor-vs-board-wide-sampling.md) updated with this case
