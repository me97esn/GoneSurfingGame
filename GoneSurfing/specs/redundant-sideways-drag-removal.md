# Spec: Remove Redundant Per-Actor Sideways Drag (Bottom + Rail)

## Overview

Remove the regular sideways-drag blocks on bottom and rail surfaces. They are a structurally redundant, per-actor version of the wave-mass mechanism that `waveMassFlowDrag` ([wave-mass-flow-drag.md](wave-mass-flow-drag.md)) already handles board-wide. Their per-actor sampling produces a perpendicular force that overpowers the new directional drag during wave-hit scenarios, defeating the purpose of the flow-drag refactor.

Deletes `bottomDragSidewaysCoefficient` and `railDragSidewaysCoefficient` UPROPERTYs. After removal, `waveMassFlowDragCoefficient` is the only knob for wave-mass momentum transfer into the board (both bottom and rail).

## Motivation

After landing the flow-drag refactor, instrumented data at gameS 7.72 (mid breaking-wave, `surfing-down-the-line` autopilot) shows the regular `sidewaysDrag` still dominates by 5-6× over the new `waveMassFlowDrag`:

```
bottom_left_middle-front (C_143) @ gameS 7.72:
  forwardDrag (skin friction): magnitude  1,750 N
  regular sidewaysDrag:        magnitude 30,346 N    ← per-actor, perpendicular
  waveMassFlowDrag (new):      magnitude  5,333 N    ← board-wide, along flow

  Total dragForce direction:   ~perpendicular to board.forwards
                               (driven by sidewaysDrag)
```

The visualization shows the total drag arrow pointing perpendicular to board.forwards — *not* aligned with the absoluteWaterVelocity arrows (which the flow-drag refactor was supposed to make happen).

### Why sidewaysDrag is dominating

Per-actor `absSidewaysVel` at gameS 7.72, C_143:
- Per-actor absSidewaysVel: **389 cm/s** ← front-SC's sample, projected on board.forwards
- Board-wide |absWaterVel|: ~400 cm/s, mostly forward

The front SC and back SC are sampling **very different** wave-velocity vectors at their respective positions on the wave's profile (~1m apart). The front SC, sampling water near the breaking lip, sees water moving in a direction that projects strongly perpendicular to `board.forwards` after the `VectorPlaneProject(_, boardForwards)` step. The board-wide average smooths this out (resulting in mostly-forward flow), but the per-actor `sidewaysDrag` formula doesn't average — it uses the front actor's full per-actor sample.

Result: `absSidewaysVel² = 151,733` per-actor → `30,346 N` perpendicular drag, completely dwarfing the board-wide `flowDrag`'s 5,333 N.

### Same structural problem we already fixed

This is the exact pattern that motivated [wave-mass-flow-drag.md](wave-mass-flow-drag.md) for `waveMassSidewaysDrag`. The regular `sidewaysDrag` has the same shape:

| | regular sidewaysDrag | waveMassSidewaysDrag (removed) |
|---|---|---|
| sampling | per-actor `absSidewaysVel` | per-actor `absSidewaysVel` |
| direction | perpendicular to board.forwards (projection) | perpendicular to board.forwards (projection) |
| input | wave's absolute water velocity (perpendicular part) | wave's absolute water velocity (perpendicular part) |
| differences from each other | no slopeSin gate, no wave-face gating | slopeSin × waveMassCoef multiplier |

Both are degraded per-actor variants of the same physical effect (wave-mass momentum pushing the board perpendicular). We removed one; we should remove the other for consistency.

### What sidewaysDrag was supposedly modeling

The original `sidewaysDrag` represented "lateral water flow pushes the board sideways" — wave-mass momentum transfer perpendicular to board.forwards. It uses *absolute* water velocity (not relative — so it's not skin friction; skin friction lives in `forwardDrag` and uses relative velocity, attenuated by planing).

Same physical interpretation as `waveMassFlowDrag`, just projected onto the perpendicular axis of board.forwards. With flow drag now applying force in the actual water-flow direction (which naturally has both a forward and a lateral component matching the wave's real motion), the perpendicular-projection version is redundant.

### Edge case check: flat water with current

Could `sidewaysDrag` fire in a regime where `flowDrag` doesn't?

- `flowDrag` is gated by `boardWideSlopeSin > 0` — fires only on wave faces.
- `sidewaysDrag` has no slope gate — fires whenever absSidewaysVel² > 0.

So on **flat water with a horizontal current** (no wave geometry, but water itself is moving sideways), `sidewaysDrag` fires while `flowDrag` doesn't. But this scene doesn't have such water — flat water in this game has zero `absoluteWaterVelocity` (waves are the only thing that move water).

Even hypothetically, if the project later adds flat-water currents, the right answer is to *un-gate flowDrag from slopeSin* (or add a separate "flat-water current drag" term) rather than to preserve the per-actor sidewaysDrag with its sampling pathology.

## Background

### What's being removed

**Bottom (`VE_Down`)**: The sideways-drag block at [FluidDynamics.cpp:141-158](../Source/GoneSurfing/FluidDynamics.cpp#L141-L158). Computes `absSidewaysVel`, applies `sidewaysDrag = absSidewaysVel.unit × Clamp(coef × effH × absSidewaysVel²)`.

**Rail (`VE_Left`/`VE_Right`)**: The sideways-drag block inside the `sidewaysEngaged` branch at [FluidDynamics.cpp:280-281](../Source/GoneSurfing/FluidDynamics.cpp#L280-L281). Same shape as bottom.

**UPROPERTYs**: `bottomDragSidewaysCoefficient`, `railDragSidewaysCoefficient`.

### What stays

| force | role | reason |
|---|---|---|
| `forwardDrag` (skin friction) | board moves through water → viscous resistance | Different mechanism (relative velocity, planing-attenuated); not redundant |
| `waveMassThrust` | hydrofoil-redirect: lateral flow → forward propulsion | Distinct physical effect |
| `waveMassFlowDrag` | wave-mass momentum in water's actual flow direction | Replaces both the removed wave-mass sideways drag and now the regular sideways drag |
| Tail wave-mass | tail-specific variant of flow drag | Already migrated in flow-drag spec |

### `absSidewaysVel` no longer needed in bottom branch (mostly)

The bottom branch computes `absSidewaysVel` (line 150) for both `sidewaysDrag` (removed) and `waveMassThrust` (kept). `waveMassThrust` still needs it — its magnitude input is `absSidewaysVel.SizeSquared()`. So we keep the variable, just remove the drag block that consumed it.

In the rail branch, `absSidewaysVel` is computed for the `sidewaysEngaged` check and for the (removed) sideways drag. The check still needs it (we still want to know which rail the absolute flow is hitting). The variable stays.

## Design

### Code change summary

1. **Delete** `bottomDragSidewaysCoefficient` UPROPERTY from `FluidDynamics.h`.
2. **Delete** `railDragSidewaysCoefficient` UPROPERTY from `FluidDynamics.h`.
3. **Delete** the bottom sideways-drag computation block (`sidewaysDragAmount = ...; sidewaysDrag = ...`) inside `case ESide::VE_Down`.
4. **Delete** the rail sideways-drag computation block (`sidewaysDragAmount = ...; sidewaysDrag = ...`) inside the rail's `sidewaysEngaged` branch.
5. **Replace** the `sidewaysDrag` variable in both branches with directly accumulating into a local — or rename to `lateralDragSum` if cleaner. Most natural: keep `sidewaysDrag` declared as `FVector` initialized to ZeroVector, and have flowDrag append to it. Same as the current shape, just with one less contributor.
6. **Debug log cleanup**: Remove `sidewaysAmount(abs)` / `sidewaysDragAmount` from the rail debug log format string. Bottom's `=== SIDEWAYS DRAG === ...` log line should be removed (no longer relevant).

### Final force composition after this spec

**Bottom (`VE_Down`)**:
```cpp
dragForce = forwardDrag + waveMassThrust + waveMassFlowDrag;
```
(No `sidewaysDrag` summand.)

**Rail (`VE_Left`/`VE_Right`)**:
```cpp
dragForce = forwardDrag + flowDragContribution;
```
where `flowDragContribution` fires inside `sidewaysEngaged` (gates the engaged rail).

**Tail (`VE_Tail`)**: unchanged from flow-drag spec.

### Coefficient tuning impact

After removal, `waveMassFlowDragCoefficient` becomes the only knob controlling wave-mass force magnitude. To preserve some perpendicular-to-board behavior during wave hits:

- `flowDrag` direction = `boardWideAbsWaterVel.unit` (~93% forward, ~7% lateral when water is mostly forward — typical case).
- Its **perpendicular component** = `|flowDrag| × sin(angle off forward) ≈ |flowDrag| × 0.07` in typical cases.
- The current `sidewaysDrag` was producing ~30 kN of pure perpendicular force per actor.

To match 30 kN of perpendicular force from flowDrag, we'd need `|flowDrag| ≈ 430 kN per actor` — but that pumps **429 kN of forward force** simultaneously. Not viable.

The right approach is to accept that the perpendicular response will be *smaller* than before (a few kN at most), and instead tune for the *correct direction* of the wave-mass force. The wave-hit behavior change: instead of the board getting punched purely sideways by 30 kN of perpendicular drag (and not knowing why), it gets pushed *in the direction the wave is actually moving* by ~5-10 kN. That's the physically meaningful response we've been aiming for.

Starting tuning point: `waveMassFlowDragCoefficient` ≈ 0.001 to 0.005 (2-10× current 0.0005). Bisect against the wave-hit behavior. Also reconsider `waveMassThrustCoefficient` once tuning is stable, since both terms now contribute forward thrust naturally.

## Implementation Sketch

### `FluidDynamics.h`

Remove the two UPROPERTYs:

```cpp
// REMOVED: bottomDragSidewaysCoefficient — subsumed by waveMassFlowDrag (board-wide)
// REMOVED: railDragSidewaysCoefficient   — subsumed by waveMassFlowDrag (board-wide, engaged rail)
```

### `FluidDynamics.cpp` — Bottom branch

Replace the `(B) Sideways drag` block:

```cpp
// BEFORE: sidewaysDrag computation block (lines ~141-158)
// const FVector absVelInBottomPlane = ...
// const FVector absSidewaysVel = ...
// const float sidewaysDragAmount = bottomDragSidewaysCoefficient × ...
// FVector sidewaysDrag = absSidewaysVel.unit × Clamp(...)
// if (debugDragLog) { UE_LOG ... }

// AFTER: only compute absSidewaysVel (still needed for waveMassThrust input)
//        sidewaysDrag declared as ZeroVector; flowDrag accumulates into it later
const FVector absVelInBottomPlane = FVector::VectorPlaneProject(absWaterVel, boardUp.GetSafeNormal());
const FVector absSidewaysVel     = FVector::VectorPlaneProject(absVelInBottomPlane, boardForwards.GetSafeNormal());
FVector sidewaysDrag = FVector::ZeroVector;  // Now only the flow-drag contribution
```

The `waveMassThrust` block (uses `absSidewaysVel.SizeSquared()`) is unchanged.

The `waveMassFlowDrag` block adds into `sidewaysDrag` (this is the variable that flows into the final sum). Could be renamed to `flowDragForce` for clarity, but keeping `sidewaysDrag` minimizes churn.

Final assembly line unchanged:
```cpp
dragForce = forwardDrag + sidewaysDrag + waveMassThrust;  // sidewaysDrag now == flowDrag's contribution
```

### `FluidDynamics.cpp` — Rail branch

Inside the `sidewaysEngaged` block, remove the sideways drag computation:

```cpp
// BEFORE:
// if (sidewaysEngaged) {
//     sidewaysDragAmount = railDragSidewaysCoefficient × ...
//     sidewaysDrag = absSidewaysVel.unit × Clamp(sidewaysDragAmount, 0.0f, maxDragAmount);
//     // ... (rail-skipped-thrust comment) ...
//     // flow drag block
// }

// AFTER:
// if (sidewaysEngaged) {
//     // (regular sidewaysDrag removed — replaced by waveMassFlowDrag below)
//     // ... (rail-skipped-thrust comment retained for context) ...
//     // flow drag block (unchanged)
// }
```

Remove `sidewaysDragAmount` declaration and `sidewaysDrag = ...` assignment. The `sidewaysDrag` variable still exists (declared at top of branch as `FVector sidewaysDrag = FVector::ZeroVector;`), but only the flow-drag block writes to it.

### Debug log cleanup

**Bottom**: Remove the `=== SIDEWAYS DRAG ===` log line entirely (no longer relevant — the per-actor sideways drag is gone). The `waveMassFlowDrag` line remains.

**Rail**: Update the `=== RAIL DRAG === ...` format string. Remove the `sidewaysAmount(abs): %.2f` field. Keep `absSidewaysVelMag` (still useful for diagnosing what the engagement gate is seeing), `sidewaysEngaged` (gate state), and `flowDragAmount` (the actual contributing force).

## Acceptance Criteria

### AC1 — Compiles, two UPROPERTYs removed

`bottomDragSidewaysCoefficient` and `railDragSidewaysCoefficient` no longer exist. Editor opens existing umaps with their values silently dropped.

### AC2 — `waveMassFlowDrag` becomes the dominant wave-mass perpendicular contributor

At gameS 7.72 (or any moment with active wave geometry), the bottom debug-arrow direction tracks the `boardWideAbsoluteWaterVelocity` direction (the red SC arrows) much more closely than before. Specifically: the angle between the total `dragForce` and `boardWideAbsoluteWaterVelocity` is < 15° (was ~70° before this spec, dominated by perpendicular sidewaysDrag).

### AC3 — Flat water unchanged

`surf-straight` snapshot trajectory: unchanged. On flat water `absoluteWaterVelocity ≈ 0`, so all the removed terms were already zero. No tuning required for this baseline.

### AC4 — Wave-hit behavior re-tuned

After raising `waveMassFlowDragCoefficient` (estimate 0.001-0.005), the wave-hit moment at gameS 9.05 in `surfing-down-the-line` shows the board responding to the wave's actual flow direction. The board's velocity vector ends up rotated *toward* `boardWideAbsoluteWaterVelocity.unit`, not perpendicular to board.forwards.

### AC5 — Carving phase preserved

The trough-carving portion (gameS 6.5 → 7.5) is qualitatively unchanged after re-tuning. Carve angular rate and arc geometry stay within snapshot tolerance.

### AC6 — Debug log no longer mentions removed coefficients

`surf.debug.flags 'drag'` doesn't print `sidewaysAmount(abs)` or `bottomDragSidewaysCoefficient` references. The `waveMassFlowDrag` line is the only sideways-related drag entry.

## Open Questions / Future Work

1. **Should the regular `forwardDrag` (skin friction) also be reconsidered?** It's per-actor too, uses `relVelAlongBoard²` (scale-leaked), and gated by `(1 - planing)`. But it's a fundamentally different mechanism (viscous, relative-velocity-based) and small in magnitude (gateds by `0.15` × `relVelAlongBoard²`). Defer until evidence shows it's misbehaving.

2. **Flat-water current scenario.** If the project later adds horizontal water currents on flat water (e.g., a river), the slopeSin gate on `flowDrag` would prevent it from firing there. Two options if that comes up: ungate the flow drag from slopeSin (with a renamed variable like `waveOrCurrentMassFlowDrag`), or add a separate flat-water-current term. Defer.

3. **Tune `waveMassThrustCoefficient` down to compensate.** With `sidewaysDrag` gone, the only wave-mass forces are `flowDrag` (which has ~93% forward component) and `waveMassThrust` (pure forward). The board may have *too much* forward propulsion now. Lower `waveMassThrustCoefficient` to compensate; possibly disable it entirely depending on tuning feel.

4. **`bottomDragCoefficient` and `railDragCoefficient` (forward versions) — are they similarly redundant?** They model skin friction (relative velocity, planing-attenuated). Skin friction is a real distinct mechanism from wave-mass momentum transfer; not the same physics. Likely keep. But worth a once-over to verify their magnitudes aren't dominating after this cleanup.

## Status

- [x] `bottomDragSidewaysCoefficient` UPROPERTY removed
- [x] `railDragSidewaysCoefficient` UPROPERTY removed
- [x] Bottom: regular `sidewaysDrag` computation block removed (`absSidewaysVel` calculation retained for `waveMassThrust`)
- [x] Rail: regular `sidewaysDrag` computation block inside `sidewaysEngaged` removed
- [x] Bottom debug `=== SIDEWAYS DRAG ===` log line removed
- [x] Rail debug log format string updated (removes `sidewaysAmount(abs)`)
- [ ] AC1 verified (compile, properties gone)
- [ ] AC2 verified (drag direction tracks water flow direction within 15°)
- [ ] AC3 verified (`surf-straight` baseline unchanged)
- [ ] AC4 verified (wave-hit re-tuning produces sensible directional response)
- [ ] AC5 verified (carving phase within tolerance after re-tuning)
- [ ] AC6 verified (debug log cleaned up)
- [ ] `waveMassFlowDragCoefficient` bisected for new sole responsibility
- [ ] `waveMassThrustCoefficient` reconsidered after the new force budget
- [ ] `surfing-down-the-line` re-baselined if trajectory shifts materially
