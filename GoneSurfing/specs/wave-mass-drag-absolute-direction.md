# Spec: Wave-Mass Drag Uses Absolute Water Velocity Direction

## Overview

The drag terms that represent **wave-mass momentum transfer** (force from the wave's body of water acting on the board) should use the **absolute water velocity** direction, not the relative water velocity direction. The current implementation uses relative velocity for both their direction and their magnitude scaling, which produces forces pointing in the wrong direction when the board is moving with the wave's flow.

Scope: bottom drag, rail drag (forward + sideways components, the parts not attenuated by planing), and tail drag's wave-mass component. Keeps planing-attenuated drag (skin friction) on relative velocity.

## Motivation

Diagnosed 2026-05-22 by tracing the orange rail-drag debug arrows in the editor. Snapshot data at the wave-touching moment:

| quantity | direction | magnitude |
|---|---|---|
| **absoluteWaterVelocity** (SC front) | (-0.997, +0.002, -0.073) | 412 cm/s |
| **absoluteWaterVelocity** (SC back) | (-0.998, -0.013, -0.057) | 389 cm/s |
| Board velocity | (-149, +421, +8) | ~450 cm/s |
| **relativeWaterVelocity** (= abs − board) | (-0.527, -0.847, -0.076) | ~497 cm/s |

The wave's water is moving almost purely in -X (toward shore / wave propagation direction). The board is moving partly with the wave's X direction but mostly perpendicular (+Y, "down the line"). As a result, `relativeWaterVelocity` is dominated by the board's own forward motion showing up as "water moving backward relative to me" — its direction (-0.527, -0.847, -0.076) is ~58° off from the wave's actual flow direction (-0.997, +0.002, -0.073).

The wave-mass drag terms currently use the relative-velocity direction. Consequence: the **wave's lateral momentum push** ends up applied in a direction shaped largely by the board's own motion vector, not by the wave's actual flow direction. Specifically:

- The combined rail drag (forward + sideways) vector ends up at (-10801, -1371, -434) — mostly -X, slight -Y. Looks "leftward-with-a-touch-down" instead of the expected "downward (wave's direction)" in the screenshot view.
- The board ends up decelerated in its own +Y forward motion (because the projected relative-velocity drag pulls -Y) rather than carried along by the wave's -X push.

This is a fundamental physics issue, not a tuning issue. The wave's momentum acts in the wave's direction regardless of how the board happens to be moving at that moment.

## Physical Background

Two distinct physical mechanisms get conflated in the existing drag formulas:

1. **Skin friction (form drag)**: viscous resistance from water flowing past a surface. Force direction = along *relative* water velocity (opposes board motion). Scales with `relVel²` and surface properties. Important on flat water and at moderate speeds.
2. **Wave-mass momentum transfer**: the wave's body of water (with its own momentum, in its own direction) interacts with the body sitting in it. Force direction = along *absolute* water velocity (the wave's actual flow). Scales with `absVel² × effectiveWaterHeight × slopeSin` (wave's energy × water mass interacting with the surface).

When the board moves with the wave (same direction, similar speed), `relativeWaterVelocity` shrinks (or reverses), but `absoluteWaterVelocity` remains large — the wave is still carrying massive water mass past the board. The wave-mass mechanism keeps applying force in the wave's direction, modeling "the wave carries the surfer along."

Conversely, on flat water with no wave, `effectiveH × slopeSin = 0`, so the wave-mass term vanishes and only skin friction operates. Same as today.

## Design

For each affected drag formula, **split the force into two components**:

1. **Skin friction** (existing behavior): planing-attenuated where applicable, scales with relative velocity, direction along relative velocity. **Unchanged.**
2. **Wave-mass push** (new direction): no planing attenuation, scales with effectiveH × slopeSin (or just effectiveH for sideways drag, since that's already wave-mass-like), uses **absolute water velocity** for both direction and magnitude.

The skin friction term captures the small viscous drag from the planing surface; the wave-mass term captures the wave's actual momentum push.

### Per-surface specifics

#### Bottom (`ESide::VE_Down`)

| Component | Current | Proposed |
|---|---|---|
| Forward drag (planing-attenuated) | `relVelAlongBoard.normalize() × C × effectiveH × pitchSin × (1-planing)² × relVelAlongBoard²` | **Unchanged** (skin friction) |
| Wave-mass forward drag | `relVelAlongBoard.normalize() × C × effectiveH × slopeSin × relVelAlongBoard²` | Replace with: `absVelAlongBoard.normalize() × C × effectiveH × slopeSin × absVelAlongBoard²` |
| Sideways drag | `sidewaysVel.normalize() × C × effectiveH × sidewaysVel²` (from relative) | Replace with: `absSidewaysVel.normalize() × C × effectiveH × absSidewaysVel²` (from absolute) |

Where `absVelAlongBoard = boardForwards × (absWaterVel · boardForwards)` and `absSidewaysVel = VectorPlaneProject(VectorPlaneProject(absWaterVel, boardUp), boardForwards)`.

#### Rail left/right (`ESide::VE_Left/Right`)

Same shape as bottom:

| Component | Current | Proposed |
|---|---|---|
| Forward rail drag (planing-attenuated) | uses relative | **Unchanged** (skin friction) |
| Rail wave-mass forward drag | uses relative | uses absolute (same pattern as bottom) |
| Rail sideways drag (no planing) | uses relative | uses absolute (same pattern as bottom) |

The engagement gates stay as they are (`fwdEngaged`, `sidewaysEngaged`) — those are about which rail is in the water, independent of which direction the water is flowing. Compute the engagement from the absolute velocity to be consistent, but the gate's purpose is geometric so either should work.

#### Tail (`ESide::VE_Tail`)

Tail drag direction is **always** `board.forwards` — the rocker/geometry constrains where the tail can push. Direction doesn't change; only the magnitude basis switches:

| Component | Current | Proposed |
|---|---|---|
| Tail drag (planing-attenuated, has `cosWaterForwards` gate) | `cos × relVel² × C × planingAtten × effectiveH` along board.forwards | **Unchanged** (skin friction) |
| Tail wave-mass drag (no planing) | `C × effectiveH × slopeSin × relVel² × cosWaterForwards` along board.forwards | Replace with: `C × effectiveH × slopeSin × absVel² × cosAbsWaterForwards` along board.forwards |

Where `cosAbsWaterForwards = absWaterVel.normalize() · board.forwards`. The gate `cosWaterForwards > 0` becomes `cosAbsWaterForwards > 0` for the wave-mass component.

This captures the key tail-drag scenario: **a wave catching up to the board from behind pushes the board forward**. Currently this requires the relative water velocity to be in board.forwards direction, which means the board needs to be moving slower than the wave. With the absolute version, the wave keeps pushing the tail forward even when the board is at matching speed (`relVel ≈ 0`) — accurately modeling the wave carrying the surfer along.

### Input plumbing

`absoluteWaterVelocity` is already computed and stored in `ASharedCalculations::setup` (line 100). FluidDynamics actors access it via `this->sharedCalculations->absoluteWaterVelocity`. No new fields needed.

### Skin friction vs wave-mass: when does each dominate?

- **Flat water**: `slopeSin → 0` for forward wave-mass drag (so that term vanishes). Sideways drag scales with `effectiveH`, which has a `baseHeight` floor of 10 cm. The sideways drag still exists but is small.
- **Wave face**: `slopeSin` and `effectiveH` both grow. Wave-mass terms become the dominant drag, just like today, *but pointing in the correct (wave) direction*.
- **Board moving with the wave**: `relativeWaterVelocity → 0`. Skin friction vanishes (correct: no relative motion = no friction). Wave-mass terms keep pushing in the wave's direction (correct: the wave still has its momentum).

### Why this isn't just a coefficient-tuning issue

The current implementation's direction is wrong in a way no tuning can fix. Reducing the wave-mass coefficient just makes the wrong-direction force smaller. The board still gets pulled in the board-motion-shaped direction rather than the wave's direction. The fix has to be at the direction level.

## Acceptance Criteria

### AC1 — Compiles, no new UPROPERTYs needed

`absoluteWaterVelocity` is already accessible via SC. The change is internal to `calcDragForce`.

### AC2 — Direction follows absolute water velocity for wave-mass terms

In `surf.debug.flags 'drag,abs_velocity'` log during the wave-touching window, the wave-mass drag and sideways drag direction vectors are aligned with `absoluteWaterVelocity` direction (within numerical tolerance), not with `relativeWaterVelocity` direction.

### AC3 — Board carried by wave momentum

In the snapshot where the wave is moving in -X at 412 cm/s and the board is at yaw=-21° moving -149/+421 (mostly +Y), the bottom + rail wave-mass drag combined force has a **dominant -X component** (along the wave's flow), not a dominant -Y component (the board's motion-derived direction). Verifiable in the debug-draw arrows.

### AC4 — Tail force when board moves with wave

In a scenario where the wave catches up to the board from behind, the tail wave-mass drag fires with `cosAbsWaterForwards > 0` and pushes the board forward — even when `relativeWaterVelocity` is small. This is the "wave carries the surfer" mechanism that doesn't work today.

### AC5 — Flat water behavior unchanged

`surf-straight` snapshot trajectory unchanged. On flat water with no wave, `slopeSin = 0` (wave-mass forward drag vanishes), and the sideways drag's `effectiveH` is small. The dominant drag remains the planing-attenuated forward drag, which is unchanged.

### AC6 — Yaw reversal at wave-touching window subsides

The user-observed "tail drifts sideways while nose doesn't" pattern at gameSeconds ~8.05 visibly subsides — peak yaw drop during the window measurably smaller. Mechanism: the rail drag now pushes the board along the wave's flow rather than at a 58°-misaligned direction, so the localized lateral force concentration at the tail is reduced.

## Open Questions / Future Work

1. **Engagement gates**: `fwdEngaged` and `sidewaysEngaged` use the cosine between rail and *relative* velocity. Should these be computed from absolute? The gates' purpose is "which rail is in the water given its roll," which is a geometric question — should be unaffected by which direction the water is flowing. Probably keep using relative.
2. **Skin friction coefficient retune**: with the wave-mass push now correctly directed, the existing skin friction coefficients might need to be increased slightly to compensate for the directionality change. Bisect after AC3 is verified.
3. **The `cosAbsWaterForwards` for tail might need an explicit cap or smoothing.** If the absolute water velocity occasionally aligns perfectly with board.forwards (e.g., as the board yaws into a particular angle), the tail drag could spike. Not addressed in this spec; revisit if observed.

## Status

- [ ] `absoluteWaterVelocity` accessibility verified from FluidDynamics
- [ ] Bottom wave-mass drag direction changed to absolute
- [ ] Bottom sideways drag direction changed to absolute
- [ ] Rail wave-mass drag direction changed to absolute
- [ ] Rail sideways drag direction changed to absolute
- [ ] Tail wave-mass drag uses absolute velocity for magnitude and gate
- [ ] Debug logs show `absVel` direction alongside `relVel`
- [ ] AC2 verified (drag direction aligns with abs vel)
- [ ] AC3 verified (force has dominant -X component on wave face)
- [ ] AC5 verified (flat water unchanged)
- [ ] AC6 verified (yaw reversal subsides)
- [ ] `surfing-down-the-line` re-baselined if trajectory shifts
