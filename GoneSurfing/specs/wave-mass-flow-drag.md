# Spec: Wave-Mass Flow Drag — Replace Sideways Drag with Directional Drag

## Overview

Restructure the wave-mass drag/thrust pair so that the **second term's direction is the wave's actual flow direction** rather than the board-perpendicular projection of that flow. Replaces `waveMassSidewaysDrag` (introduced in [wave-mass-sideways-drag.md](wave-mass-sideways-drag.md)) and removes the redundant `waveMassDrag` (forward, scale-leaked) on both bottom and rail surfaces. Uses **board-wide averaged inputs** to eliminate yaw-torque from front/back sampling asymmetry.

Renames `waveMassSidewaysDragCoefficient` → `waveMassFlowDragCoefficient`. Deletes `waveMassDragCoefficient`.

## Motivation

The current `waveMassSidewaysDrag` applies its force along `absSidewaysVel.unit` — purely perpendicular to `board.forwards` by construction (the projection step strips the forward component). This produces a large perpendicular force from a *tiny* perpendicular component of the water flow:

- At gameS 5.07 (paddling on wave face), absoluteWaterVelocity is ~400 cm/s in board.forwards direction with only ~27 cm/s of lateral tilt (3.8° off forward) — confirmed visually via `DebugDrawAbsoluteVelocity` red arrows ([[wave-water-velocity-direction]]).
- The current sideways-drag formula squares the small lateral component (27² = 729) and multiplies by `effH × coef`, producing ~1600 N of purely perpendicular force on the back actor.
- The board ends up getting pushed perpendicular to its direction of travel by a force whose underlying physical cause is "water is moving mostly in the same direction as the board, slightly off-axis."

This is structurally wrong. The wave's water momentum should push the board *in the direction the water is actually flowing*, not perpendicular to the board's axis. Real water mass momentum transfer is direction-of-flow drag, not perpendicular-of-board drag.

### Concrete problematic case

At gameS 9.05 (board hitting breaking wave after carving back), the user observes the board turning **into** the wave instead of being knocked away. Forensic analysis showed the rail-lift + lateral-turn forces are both pushing in board.left direction (carving response to rolled rail), but the wave-mass sideways drag we added — intended to be the wave's counter-push — is applied along a perpendicular direction that doesn't align with the wave's actual flow either. So the wave's energy gets routed into a vector that has no physical justification, creating unpredictable side effects.

The redirect-to-forward part of the wave-mass pair (`waveMassThrust`) is fine — it represents the hydrofoil-like effect of lateral flow being deflected into forward thrust by the rolled board. The mass-momentum-transfer part needs to point where the water actually moves.

### Bonus cleanup: scale-leaked `waveMassDrag` is redundant with the new term

The existing `waveMassDrag` (forward direction, [FluidDynamics.cpp:121-139](../Source/GoneSurfing/FluidDynamics.cpp#L121-L139)) is supposed to model the same physical effect (wave momentum pushing the board along the water's flow direction) but:
1. Projects onto `board.forwards` only (loses the lateral component).
2. Uses the **unnormalized** `boardForwards · absWaterVel` (carries the SC actor's ~0.2 transform scale), producing a 0.0016× scale leak in the magnitude — at the current `waveMassDragCoefficient=0.001` it produces ~28 N where the proper-scale equivalent would be ~17 kN.

The new flow drag subsumes this: it uses the full water flow direction (no projection) and proper-scale magnitude (no leak). Remove `waveMassDrag` and the `waveMassDragCoefficient` UPROPERTY.

The skin-friction `forwardDrag` ([FluidDynamics.cpp:88-114](../Source/GoneSurfing/FluidDynamics.cpp#L88-L114)) is a different physical mechanism (viscous drag from board's motion through water, planing-attenuated, uses *relative* velocity) — keep as-is.

## Background

### Current force shapes

| force | mag input | mag scaling | direction | gate |
|---|---|---|---|---|
| `waveMassDrag` (forward) | `(absWaterVel · boardFwd_unnorm)²` | `coef × effH × slopeSin` | `±board.forwards` (along forward component) | `slopeSin > 0` |
| `waveMassSidewaysDrag` (current) | `absSidewaysVel²` | `coef × effH × slopeSin` | `absSidewaysVel.unit` (perpendicular to board) | `slopeSin > 0` |
| `waveMassThrust` | `absSidewaysVel²` | `coef × bwEffH × bwSlopeSin` | `+board.forwards` | `boardWideSlopeSin > 0` |

### Proposed force shapes

| force | mag input | mag scaling | direction | gate |
|---|---|---|---|---|
| `waveMassThrust` (unchanged) | `absSidewaysVel²` | `coef × bwEffH × bwSlopeSin` | `+board.forwards` | `boardWideSlopeSin > 0` |
| `waveMassFlowDrag` (new) | `\|boardWideAbsWaterVel\|²` | `coef × bwEffH × bwSlopeSin` | `boardWideAbsWaterVel.unit` | `boardWideSlopeSin > 0` |
| `waveMassDrag` (forward) | — | — | — | **removed** |

The `waveMassThrust` is the **hydrofoil-redirect** mechanism (lateral flow → board.forwards). The new `waveMassFlowDrag` is the **mass-momentum-carry** mechanism (water flow → board moves with the flow). Two distinct physical effects, no longer locked to a board-frame axis.

### Why board-wide

Following the precedent in [yaw-hydrofoil-board-wide-slip.md](yaw-hydrofoil-board-wide-slip.md) (yaw hydrofoil) and [per-actor-vs-board-wide-sampling.md](per-actor-vs-board-wide-sampling.md) (general principle), the wave-mass force represents **a single body of water acting on the whole board** — not multiple independent surface samples. Per-actor sampling of the wave's velocity field produces dramatic asymmetries (back actor sees ~400 cm/s, front actor sees ~24 cm/s in the same scenario), and asymmetric force magnitudes applied at offset positions produce **spurious yaw torques** that don't correspond to a physical "the wave is making me yaw" effect.

The fix is to use averaged values across both SCs (front and back) so every bottom actor produces the same magnitude force in the same direction. Total force is then applied at the actors' centroid (≈ board COM), producing zero yaw torque from the new term.

Specifically average across front/back SCs:
- `boardWideAbsWaterVel = 0.5 × (frontSC.absoluteWaterVelocity + backSC.absoluteWaterVelocity)`
- `boardWideEffectiveH` = per-SC value but read consistently across actors (same as `waveMassThrust`)
- `boardWideSlopeSin` = per-SC value (same)

The averaged absoluteWaterVelocity is a new derivation; the other two follow `waveMassThrust`'s existing pattern.

## Design

### Force formula

Per bottom actor and per engaged rail actor:

```cpp
const FVector boardWideAbsWaterVel = (sharedCalculations && sharedCalculations->otherHalfSharedCalculations)
    ? 0.5f * (sharedCalculations->absoluteWaterVelocity
              + sharedCalculations->otherHalfSharedCalculations->absoluteWaterVelocity)
    : sharedCalculations->absoluteWaterVelocity;

const float boardWideAbsWaterVelSq = boardWideAbsWaterVel.SizeSquared();

if (waveMassFlowDragCoefficient > 0.0f
    && boardWideSlopeSin > 0.0f
    && boardWideAbsWaterVelSq > 0.01f)
{
    const float flowDragAmount = waveMassFlowDragCoefficient
                               * boardWideEffectiveH
                               * boardWideSlopeSin
                               * boardWideAbsWaterVelSq;
    const FVector flowDrag = boardWideAbsWaterVel.GetSafeNormal()
                           * FMath::Clamp(flowDragAmount, 0.0f, maxDragAmount);
    // Apply: bottom adds to dragForce; rail adds to sidewaysDrag (kept naming convention)
}
```

### Per-surface details

| Surface | Apply new term? | Gate |
|---|---|---|
| Bottom (`VE_Down`) | yes | `boardWideSlopeSin > 0` |
| Rail (`VE_Left`/`VE_Right`) | yes | `sidewaysEngaged && boardWideSlopeSin > 0` (only engaged rail) |

The rail engagement gate is the same as the existing rail sideways drag — only the rail facing the water flow contributes. This is physically meaningful: a board rolled to one side only has one rail in the water to catch the wave's momentum.

The new directional drag does create a yaw torque on the rail (single-sided engagement at offset position), but the direction of this torque is now in the **water flow direction** rather than the unpredictable perpendicular-to-board direction — physically reasonable for "wave hits one rail and rotates the board with the flow."

### Removed: `waveMassDrag` (forward) on bottom and rail

Delete the entire `waveMassDrag` block (line [121-139](../Source/GoneSurfing/FluidDynamics.cpp#L121-L139) bottom, line [237-247](../Source/GoneSurfing/FluidDynamics.cpp#L237-L247) rail). Delete the `waveMassDragCoefficient` UPROPERTY in [FluidDynamics.h:204-205](../Source/GoneSurfing/FluidDynamics.h#L204).

### Defaults and naming

- New UPROPERTY: `waveMassFlowDragCoefficient`, default `0.0f` (disabled — opt in per actor).
- Old `waveMassSidewaysDragCoefficient`: **renamed** to `waveMassFlowDragCoefficient`. Values in existing umaps will need to be re-set after the rename (UE doesn't carry property renames automatically without explicit redirector).
- Old `waveMassDragCoefficient`: **deleted**. Editor will flag any umap that still has it set; safe to ignore since the property no longer exists.

### Initial tuning estimate

The current `waveMassSidewaysDragCoefficient = 0.02` produces ~1600 N on the back actor with `absSidewaysVel² ≈ 700`. With the new formula using `|boardWideAbsWaterVel|² ≈ 160000` (mostly forward, ~400 cm/s), the coefficient needs to drop by ~230× to keep the force magnitude in the same range:

```
target_force ≈ 1600 N
target_force = waveMassFlowDragCoefficient × 200 × 0.55 × 160000
            → waveMassFlowDragCoefficient ≈ 0.0001
```

So **start at ~0.0001**, then bisect against the carve baseline and the wave-hit behavior. Note this is ~100× *smaller* than the current sideways-drag coefficient — the actually-tiny-perpendicular-component issue means the current coefficient is amplifying a small input into a large force, while the new shape uses the large input (full water speed²) directly.

Also: with the new term contributing ~1600 N mostly forward (instead of ~1600 N purely perpendicular), it now adds to the forward propulsion alongside `waveMassThrust` (~590 N). To preserve the current forward-propulsion feel, consider lowering `waveMassThrustCoefficient` to ~0.005 or so, since the new term is providing additional forward drive.

## Implementation Sketch

In [FluidDynamics.cpp](../Source/GoneSurfing/FluidDynamics.cpp), inside `AFluidDynamics::calcDragForce`.

### Bottom (`case ESide::VE_Down`)

Replace the `waveMassDrag` block ([line 121-139](../Source/GoneSurfing/FluidDynamics.cpp#L121-L139)) and the `waveMassSidewaysDrag` block (added in [wave-mass-sideways-drag.md](wave-mass-sideways-drag.md)) with the new flow-drag block. Place it near the existing `waveMassThrust` block since they share the same shape:

```cpp
// Wave-mass flow drag — applies the wave's momentum to the board in the wave's actual
// flow direction. Replaces both the (forward-only, scale-leaked) waveMassDrag and the
// (perpendicular, board-frame) waveMassSidewaysDrag. Uses board-wide averaged absolute
// water velocity to eliminate front/back per-actor sampling asymmetry. See
// specs/wave-mass-flow-drag.md.
FVector waveMassFlowDrag = FVector::ZeroVector;
if (this->waveMassFlowDragCoefficient > 0.0f && boardWideSlopeSin > 0.0f)
{
    const FVector boardWideAbsWaterVel = (this->sharedCalculations
        && this->sharedCalculations->otherHalfSharedCalculations)
        ? 0.5f * (this->sharedCalculations->absoluteWaterVelocity
                  + this->sharedCalculations->otherHalfSharedCalculations->absoluteWaterVelocity)
        : this->sharedCalculations->absoluteWaterVelocity;

    const float boardWideAbsWaterVelSq = boardWideAbsWaterVel.SizeSquared();
    if (boardWideAbsWaterVelSq > 0.01f)
    {
        const float flowDragAmount = this->waveMassFlowDragCoefficient
                                   * boardWideEffectiveH
                                   * boardWideSlopeSin
                                   * boardWideAbsWaterVelSq;
        waveMassFlowDrag = boardWideAbsWaterVel.GetSafeNormal()
                         * FMath::Clamp(flowDragAmount, 0.0f, maxDragAmount);
        if (debugDragLog)
        {
            UE_LOG(LogTemp, Warning, TEXT("  waveMassFlowDrag (bottom, abs): coef=%.4f, bwSlope=%.3f, bwEffH=%.2f, |bwAbsWaterVel|²=%.1f -> amount=%.2f, drag=(%.2f, %.2f, %.2f) mag=%.2f"),
                this->waveMassFlowDragCoefficient, boardWideSlopeSin, boardWideEffectiveH, boardWideAbsWaterVelSq,
                flowDragAmount, waveMassFlowDrag.X, waveMassFlowDrag.Y, waveMassFlowDrag.Z, waveMassFlowDrag.Length());
        }
    }
}

dragForce = forwardDrag + sidewaysDrag + waveMassThrust + waveMassFlowDrag;
```

### Rail (`case ESide::VE_Left`/`VE_Right`)

Replace the rail `waveMassDrag` block (inside the `fwdEngaged` branch) and the rail `waveMassSidewaysDrag` block (inside the `sidewaysEngaged` branch) with one new block, gated by `sidewaysEngaged`:

```cpp
// Rail wave-mass flow drag — only the engaged rail contributes (gated by sidewaysEngaged).
// Same shape as bottom; see specs/wave-mass-flow-drag.md.
if (sidewaysEngaged
    && this->waveMassFlowDragCoefficient > 0.0f
    && boardWideSlopeSin > 0.0f)
{
    const FVector boardWideAbsWaterVel = (this->sharedCalculations
        && this->sharedCalculations->otherHalfSharedCalculations)
        ? 0.5f * (this->sharedCalculations->absoluteWaterVelocity
                  + this->sharedCalculations->otherHalfSharedCalculations->absoluteWaterVelocity)
        : this->sharedCalculations->absoluteWaterVelocity;

    const float boardWideAbsWaterVelSq = boardWideAbsWaterVel.SizeSquared();
    if (boardWideAbsWaterVelSq > 0.01f)
    {
        const float flowDragAmount = this->waveMassFlowDragCoefficient
                                   * boardWideEffectiveH
                                   * boardWideSlopeSin
                                   * boardWideAbsWaterVelSq;
        sidewaysDrag += boardWideAbsWaterVel.GetSafeNormal()
                      * FMath::Clamp(flowDragAmount, 0.0f, maxDragAmount);
    }
}
```

(`boardWideEffectiveH` and `boardWideSlopeSin` are read from `this->sharedCalculations` — same SC the actor is associated with, same pattern as `waveMassThrust`. Could be promoted to a helper if it's used in a third place.)

### UPROPERTY changes in `FluidDynamics.h`

```cpp
// REMOVED: waveMassDragCoefficient (subsumed by waveMassFlowDrag)

// REMOVED: waveMassSidewaysDragCoefficient (renamed and reshaped)

/** Wave-mass flow drag coefficient. Models the wave's water mass pushing the board in the
 *  direction the water is actually flowing. Force direction = boardWideAbsoluteWaterVelocity.unit
 *  (averaged across both SCs to eliminate per-actor sampling asymmetry). Replaces the previous
 *  waveMassDrag (forward, scale-leaked) and waveMassSidewaysDrag (perpendicular, board-frame).
 *  Magnitude scales with boardWideEffectiveWaterHeight × boardWideSlopeSin × |boardWideAbsWaterVel|².
 *  Gated to wave faces by boardWideSlopeSin > 0. Default 0 = disabled. Applied on bottom and
 *  on the engaged rail. See specs/wave-mass-flow-drag.md. */
UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(ClampMin = "0.0"))
float waveMassFlowDragCoefficient = 0.0f;
```

## Acceptance Criteria

### AC1 — Compiles, properties renamed/removed correctly

- `waveMassFlowDragCoefficient` UPROPERTY added.
- `waveMassSidewaysDragCoefficient` UPROPERTY removed.
- `waveMassDragCoefficient` UPROPERTY removed.
- Editor opens existing umaps without crashing; orphaned property values are silently dropped.

### AC2 — Disabled state preserves no-drag behavior

With `waveMassFlowDragCoefficient = 0.0` (default), the new term contributes zero force. The remaining drag system (`forwardDrag` skin friction, `sidewaysDrag` regular, `waveMassThrust`) behaves identically to today's pre-spec code path.

Note: AC2 is NOT bit-for-bit identical to current behavior because `waveMassDrag` (forward) is being removed. With `waveMassDragCoefficient` set to its existing value, removal subtracts ~28 N of forward force per bottom actor. This is below noise floor for the current scenarios and a known intentional change.

### AC3 — Pure-forward water produces near-pure-forward drag

In a scenario where `absoluteWaterVelocity` is purely along `+board.forwards` (e.g., paddling straight on a steep wave face aligned with heading), the new term's force is along `+board.forwards` with no perpendicular component. Verify: `waveMassFlowDrag.Dot(board.left)` ≈ 0; `waveMassFlowDrag.Dot(board.forwards)` ≈ \|waveMassFlowDrag\|.

### AC4 — Lateral-tilted water produces small lateral component

In a scenario where `absoluteWaterVelocity` is ~400 cm/s mostly forward with ~27 cm/s lateral (the gameS 5.07 scenario), the new term's force has forward/lateral ratio matching the water vector's: ~93% forward / ~7% lateral. Verify: `\|waveMassFlowDrag.Dot(board.forwards)\| / \|waveMassFlowDrag\|` ≈ 0.93.

### AC5 — Front and back actors produce identical force from this term

With board-wide values, every bottom actor produces the same `waveMassFlowDrag` magnitude and direction at a given tick. Verify in logs: `waveMassFlowDrag` log entries for different actor labels at the same wall clock tick show identical `amount`, `drag` vector, and `mag`.

### AC6 — Wave-hit scenario: wave pushes board in wave's actual direction

At gameS 9.05 (wave-hit moment from earlier investigation), the new term's force vector should point along `boardWideAbsoluteWaterVelocity.unit` — *not* perpendicular to board.forwards. Verify by debug-drawing the new force as a colored arrow and confirming it aligns with the SC's `DebugDrawAbsoluteVelocity` red arrows.

### AC7 — Carving phase: forward propulsion preserved

The trough-carving portion of `surfing-down-the-line` (gameS 6.5 → 7.5) maintains the carve quality you've tuned in via `finLiftMagnitude` and `lateralTurnCoefficient`. After re-tuning `waveMassFlowDragCoefficient` (target ~0.0001) and possibly lowering `waveMassThrustCoefficient` to compensate for the new forward contribution, snapshot trajectory stays within tolerance.

### AC8 — Flat water unchanged

`surf-straight` snapshot trajectory unchanged. `boardWideSlopeSin ≈ 0` on flat water, so the new term contributes nothing — but verify with the snapshot to catch incidental side effects (e.g., from a typo in the removal block).

### AC9 — Debug log shows the new term

Per-surface `waveMassFlowDrag` log lines appear under `surf.debug.flags 'drag'`, alongside the existing drag log lines. Format includes coef, boardWideSlopeSin, boardWideEffectiveH, `|bwAbsWaterVel|²`, amount, drag vector, mag.

## Open Questions / Future Work

1. **Should the rail term skip rail entirely?** Following `waveMassThrust`'s precedent ([wave-mass-thrust.md](wave-mass-thrust.md), Open Question #2), single-rail-engaged forces produce yaw torque. The new directional drag also has this. The torque is now in the water-flow direction rather than purely sideways, but it's still asymmetric. Bottom-only first pass would be the conservative choice; this spec includes rail to maintain rough parity with the current `waveMassSidewaysDrag` it's replacing. If wave-hit testing shows excessive yaw, drop the rail contribution.

2. **Should `boardWideEffectiveH` and `boardWideSlopeSin` also be averaged across both SCs?** Currently they're per-SC (one SC per side of the board, but each actor reads its own SC's "boardWide" values). True board-wide would average them. Marginal benefit since the per-SC values are already much less noisy than per-actor; defer unless asymmetry between left/right halves causes visible problems.

3. **Tuning relationship to `waveMassThrustCoefficient`.** Both terms now have substantial forward contributions (the new flow drag's forward projection plus `waveMassThrust`'s pure forward push). They reinforce naturally. Open question whether `waveMassThrustCoefficient` should be lowered to keep total forward propulsion at current tuned levels, or whether keeping both at current values produces a desirable faster-on-the-wave-face behavior. Decide empirically.

4. **Compose with `maxEffectiveWaterHeight` uncapping.** If a future change uncaps `effectiveH` for drag only (discussed 2026-05-25), this term scales linearly with the uncapped value. Re-tune coefficient if that lands.

5. **Should the magnitude use velocity² or |velocity|?** Currently uses `|bwAbsWaterVel|²` (quadratic in water speed, matching `waveMassThrust`'s shape). An alternative is linear in `|bwAbsWaterVel|`, which would be more like "constant carry rate" rather than "kinetic-energy carry." Stick with quadratic for consistency with the existing terms; revisit if tuning behavior feels wrong.

## Status

- [x] `waveMassFlowDragCoefficient` UPROPERTY added to `AFluidDynamics`
- [x] `waveMassSidewaysDragCoefficient` UPROPERTY removed
- [x] `waveMassDragCoefficient` UPROPERTY removed
- [x] Bottom: `waveMassFlowDrag` term added, `waveMassDrag` + `waveMassSidewaysDrag` removed
- [x] Rail (left/right): `waveMassFlowDrag` term added inside `sidewaysEngaged`, `waveMassDrag` (in `fwdEngaged`) + `waveMassSidewaysDrag` (in `sidewaysEngaged`) removed
- [x] Tail: migrated existing wave-mass formula to use `waveMassFlowDragCoefficient` (tail-specific direction constraint: +board.forwards, with cosAbsForwards gate)
- [x] Debug log shows `waveMassFlowDrag` per surface
- [ ] AC2 verified (disabled = no force from new term; carve baseline unchanged after re-baseline)
- [ ] AC3 verified (pure-forward water → pure-forward drag)
- [ ] AC4 verified (lateral-tilted water → small lateral force)
- [ ] AC5 verified (front and back actors produce identical force)
- [ ] AC6 verified (wave-hit: force aligns with wave's actual flow direction)
- [ ] AC7 verified (carving phase preserved)
- [ ] AC8 verified (`surf-straight` baseline unchanged)
- [ ] Coefficient bisected to a stable value
- [ ] `surfing-down-the-line` re-baselined if trajectory shifts materially
