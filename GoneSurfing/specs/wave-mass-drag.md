# Spec: Wave-Mass Drag (Bottom, Rail, Tail)

## Overview

Add a new drag term that fires on **steep wave faces** regardless of planing state, applied to the **bottom**, **rail**, and **tail** surfaces. Models the resistance a board feels when plowing horizontally through the column of water in a breaking wave — the "wall of water" effect that the existing planing-attenuated drag doesn't capture.

Scope: additive contribution inside the existing per-side branches of `AFluidDynamics::calcDragForce`. Single shared coefficient (`waveMassDragCoefficient`), default 0 (disabled) so existing tuning is untouched until the umap opts in.

## Motivation

When the board is "down the line" and physically moves through a steep wave face, observed in the latest snapshot:

- `vy` stays at 450-530 cm/s across the entire 1-second wave-crossing window.
- `amountUnderWater` stays at 0.5-0.7 throughout — board is inside the wave's volume.
- `slopeSin` is 0.25-0.50 — board is on a steep face.
- `AmountPlaning` stays at 0.95 throughout.

The board passes through the wave essentially without deceleration. Existing forward drag is planing-attenuated by `(1-AmountPlaning)² ≈ 0.0025`, so it produces 6-50 N per actor — negligible. Sideways drag has zero forward component by construction. The other resistive terms (fin drag's `cos⁴(yaw)`, rail's planing-attenuated forward drag) are also too weak.

Physical intuition: a planing board *on flat water* legitimately has low drag (it's riding on top of the water). A board *plowing through a vertical column of water* should not, because there's no "top of the water" to plane on — the water surrounds it. The planing attenuation is the right model for the flat-water case but the wrong model on a steep wave face. The new term restores drag specifically when the board is in a thick wave column.

## Design

### Force shape

Per surface, in the direction of relative water flow projected onto the surface's relevant axis:

```
waveMassDrag = waveMassDragCoefficient
             × effectiveWaterHeight       // water-mass proxy (already capped at 200)
             × slopeSin                   // steep-face gate (0 on flat water)
             × relVelAlong.SizeSquared()  // v² scaling, drag-shape
```

Applied additively on top of existing drag, in the same direction the existing drag uses for that surface. No planing attenuation, no pitch-AOA gate. The `slopeSin` factor is what restricts this to wave faces — on flat water it's zero, so the existing planing-attenuated formula is the only contributor (preserving current flat-water behavior).

### Per-surface details

| Surface | Existing direction | New term direction | Gate to keep |
|---|---|---|---|
| Bottom (`VE_Down`) forward | `relVelAlongBoard.GetSafeNormal()` | same | none (existing `pitchSinAngleOfAttack >= 0` not applied to new term — wave force doesn't need pitch) |
| Rail left/right (`VE_Left`/`VE_Right`) forward | `relVelAlongBoard.GetSafeNormal()` | same | engagement gate (`fwdEngaged`) — only the rail facing the flow contributes |
| Tail (`VE_Tail`) | `forwards` | same | `cosWaterForwards > 0` — keep the existing tail gate so the term only fires when water comes from behind, matching existing semantics |

The sideways drag terms and fin drag are *not* modified — they already have no planing attenuation and don't need this treatment.

### Why one shared coefficient

The four affected drag formulas already have their own coefficients (`bottomDragCoefficient`, `railDragCoefficient`, `tailDragCoefficient`) for the planing-attenuated portion. The wave-mass term is conceptually the same physics across all three surfaces — bulk water resisting motion of the surface through it — so one knob is the natural shape. If tuning later shows the three surfaces want very different wave-mass response, split into per-surface coefficients.

### Direction choice

Each surface's drag direction is what the *existing* drag uses for that surface. That's "the direction water is flowing relative to the board along the relevant axis" — which is also the direction the board needs to be pushed back against. So the new term's direction is correct by inheritance from the existing formula's logic.

### Why `slopeSin` and not just `effectiveWaterHeight`

`effectiveWaterHeight` already includes a `slopeSin × slopeHeight` contribution, but its `baseHeight = 10` baseline means even on flat water it's non-zero. Multiplying by `slopeSin` explicitly ensures the new term is **zero on flat water** — preserving today's flat-water behavior — and **non-zero only on inclined wave surfaces**. Linear ramp from flat (0) to vertical wall (1).

### Default value

Default `waveMassDragCoefficient = 0.0f` (disabled). The user opts in by setting this in the umap after the code lands. Starting tuning value: order-of-magnitude estimate from `mag = coef × 200 × 0.4 × 250000 ≈ coef × 2×10⁷`. To produce ~2 kN drag per actor on a steep wave face with normal speeds: `coef ≈ 1e-4`. To produce ~5 kN: `coef ≈ 2.5e-4`. Bisect from there.

## Implementation Sketch

In [FluidDynamics.cpp:62](../Source/GoneSurfing/FluidDynamics.cpp#L62), add an additive block inside each affected case.

### Bottom (`case ESide::VE_Down`)

After the existing forward drag computation, before the sideways drag block:

```cpp
// Wave-mass drag: fires on steep wave faces regardless of planing state. Models the
// "wall of water" resistance that the planing-attenuated forwardDrag can't capture
// when the board is plowing through a wave's volume. See specs/wave-mass-drag.md.
if (this->waveMassDragCoefficient > 0.0f && this->slopeSin > 0.0f)
{
    const float forwardComponent = FVector::DotProduct(relWaterVel, boardForwards);
    const FVector relVelAlongBoard = boardForwards * forwardComponent;
    const float waveMassAmount = this->waveMassDragCoefficient
                               * this->effectiveWaterHeight
                               * this->slopeSin
                               * relVelAlongBoard.SizeSquared();
    const FVector waveMassDrag = relVelAlongBoard.GetSafeNormal()
                               * FMath::Clamp(waveMassAmount, 0.0f, maxDragAmount);
    forwardDrag += waveMassDrag;  // additive to existing planing-attenuated drag
}
```

### Rail (`case ESide::VE_Left`/`VE_Right`)

Inside the `fwdEngaged` block, after the existing forward drag computation:

```cpp
if (fwdEngaged && this->waveMassDragCoefficient > 0.0f && this->slopeSin > 0.0f)
{
    const float forwardComponent = FVector::DotProduct(relWaterVel, boardForwards);
    const FVector relVelAlongBoard = boardForwards * forwardComponent;
    const float waveMassAmount = this->waveMassDragCoefficient
                               * this->effectiveWaterHeight
                               * this->slopeSin
                               * relVelAlongBoard.SizeSquared();
    forwardDrag += relVelAlongBoard.GetSafeNormal()
                 * FMath::Clamp(waveMassAmount, 0.0f, maxDragAmount);
}
```

### Tail (`case ESide::VE_Tail`)

Inside the `cosWaterForwards > 0` block, after the existing drag computation:

```cpp
if (this->waveMassDragCoefficient > 0.0f && this->slopeSin > 0.0f)
{
    // Tail drag direction is `forwards` (matching the existing tail formula).
    // Use the same relative velocity magnitude that the existing tail term uses (v²).
    const float waveMassAmount = this->waveMassDragCoefficient
                               * this->effectiveWaterHeight
                               * this->slopeSin
                               * this->sharedCalculations->relativeWaterVelocity.SizeSquared()
                               * cosWaterForwards;  // gate: water moving in +forwards direction
    dragForce += this->sharedCalculations->forwards
               * FMath::Clamp(waveMassAmount, 0.0f, maxDragAmount);
}
```

### Debug logging

Each surface's existing debug log line gets a new field showing the wave-mass component magnitude, so tuning can be done by watching the log under `surf.debug.flags 'drag'`.

## Acceptance Criteria

### AC1 — Compiles, single new UPROPERTY

`AFluidDynamics::waveMassDragCoefficient` exposed as `UPROPERTY(EditAnywhere, BlueprintReadWrite)`. Comment references this spec.

### AC2 — Disabled state preserves pre-spec behavior

With `waveMassDragCoefficient = 0.0` (default), all drag forces are bit-for-bit identical to pre-spec. Verifiable by comparing CSV outputs before and after this change with the coefficient at 0.

### AC3 — Wave penetration slows down

With a non-zero coefficient and the user's existing tuning, the `vy` slowdown across the wave-crossing window (gameSeconds ~8.5-9.5 in the recent snapshot) is meaningfully larger than the current ~50-80 cm/s drop. Target: vy drops by at least 200 cm/s through the wave crossing.

### AC4 — Flat water behavior unchanged

`surf-straight` snapshot trajectory is unchanged (or within snapshot variance). On flat water `slopeSin ≈ 0`, so the new term contributes nothing.

### AC5 — Per-surface contributions are visible

Debug log under `surf.debug.flags 'drag'` shows the wave-mass component magnitude alongside the existing planing-attenuated component for each affected surface, so coefficient tuning can be done by inspection.

## Open Questions / Future Work

1. **Per-surface coefficients.** If tuning shows bottom / rail / tail want very different wave-mass response, split into three coefficients (`bottomWaveMassDragCoefficient`, etc.). Defer until needed.
2. **Apply to fin too?** Fin drag uses `cos⁴(yaw)` which already responds to flow direction. Adding wave-mass would couple fin to wave geometry. Probably *not* wanted — the fin is supposed to be a directional element, not a flat-water-vs-wave-water gate. Out of scope.
3. **Replace planing attenuation entirely.** Long-term, the planing attenuation formula could be generalized to `(1 - planing × (1 - slopeSin))²` so that on steep waves the attenuation collapses naturally. That's a bigger change — would affect every drag tuning. The additive new term is the cheap path that doesn't disturb existing tuning.
4. **Buoyancy spike cap.** The 50 kN vertical buoyancy spike at the wave lip is a separate issue (see prior diagnosis). Not addressed by this spec; do as a follow-up.

## Status

- [ ] `waveMassDragCoefficient` UPROPERTY added to `AFluidDynamics`
- [ ] Bottom forward drag: wave-mass term added
- [ ] Rail forward drag: wave-mass term added
- [ ] Tail drag: wave-mass term added
- [ ] Debug logs include wave-mass component
- [ ] AC2 verified (disabled = unchanged behavior)
- [ ] AC3 verified (wave penetration slows with non-zero coefficient)
- [ ] AC4 verified (flat water unchanged)
- [ ] AC5 verified (debug log shows components)
- [ ] `surfing-down-the-line` re-baselined if behavior changes materially
