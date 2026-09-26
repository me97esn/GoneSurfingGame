# Spec: Lateral-turn carve driven by WORLD-relative roll (not wave-relative)

## Overview

The `lateralTurnForce` (the board's primary carve/steering coupling) keys its magnitude on
`waveRelativeRollSin` — the board's roll **relative to the wave surface**. On a steep wave that produces
a strong *involuntary* carve into the wave even at neutral trim, because a board surfing down the line
sits roughly **horizontal in the world**, which is heavily rolled *relative to the tilted face*. This
spec re-references the carve to **world roll** (roll relative to horizontal) so neutral = straight and
only a deliberate lean carves.

## Problem / History

`lateralTurnForce` ([FluidDynamics.cpp:806-828](../Source/GoneSurfing/FluidDynamics.cpp#L806)):

```
lateralAmount = |waveRelativeRollSin| × |sinPitchAngleOfAttack| × relWaterVelMag
              × AmountPlaning × lateralTurnCoefficient × lateralTurnMultiplier
direction     = -sign(waveRelativeRollSin) × board.left
```

There is **no weight/`amountToTheRight` term** — the carve is driven purely by `waveRelativeRollSin`
(plus pitch, speed, planing). `waveRelativeRollSin` is the board's roll measured against the wave plane
([SharedCalculations.cpp:281-286](../Source/GoneSurfing/SharedCalculations.cpp#L281), via
`waveTangentLeft = cross(waveNormal, forwards)`).

### Current behaviour with lateral turn ENABLED (`lateralTurnCoefficient > 0`)

The board **yaws nose-first into the wave** every play, with weight perfectly centered. Confirmed via
the torque budget (`surf.debug.flags torque`): `lateralTurn` is the dominant net (non-cancelling) yaw
contributor across the post-handoff ride.

### Current behaviour with lateral turn DISABLED (`lateralTurnCoefficient = 0`, current default)

No involuntary carve, but the board now **turns far too slowly** — steering authority drops to whatever
the yaw hydrofoil, fins, and weight-driven roll provide, which is insufficient for responsive surfing.

So neither state is acceptable: ON → carves into the wave on its own; OFF → won't turn.

## Root Cause

`waveRelativeRollSin` is the wrong reference frame for "how much is the rider carving."

A surfboard trimming down the line is **mostly horizontal in the world, even on a steep face** (it does
*not* roll to lie parallel to the wave). Therefore at neutral trim `waveRelativeRollSin` is **large by
construction** (≈ the wave's roll angle), and `lateralAmount ∝ |waveRelativeRollSin|` fires a big carve
before the rider has done anything. The term conflates two different things:

- *"the board is on a tilted wave"* — always true, should **not** steer, and
- *"the rider has leaned/rolled the board"* — the input that **should** steer.

(Note: an earlier idea — add a righting tendency so the board planes parallel to the face, driving
`waveRelativeRollSin → 0` at neutral — is **rejected**: it contradicts the real attitude of a board
trimming down the line, which stays horizontal. See [[project_lateral_turn_wave_relative_roll]].)

## Proposed Solution

Drive the carve off **world-relative roll** — the board's roll about its forward axis measured against
the **horizontal** plane — instead of wave-relative roll. Add a `worldRelativeRollSin` to
`SharedCalculations`, computed identically to `waveRelativeRollSin` but with the wave-tangent "left"
replaced by a horizontal tangent:

```
worldTangentLeft  = cross(worldUp(0,0,1), forwards).GetSafeNormal()   // always horizontal
worldRelativeRollCos = VectorPlaneProject(left,  forwardsN).GetSafeNormal() | worldTangentLeft
worldRelativeRollSin = VectorPlaneProject(-up,   forwardsN)              | worldTangentLeft
```

Then `lateralTurnForce` uses `worldRelativeRollSin` for both magnitude and direction-sign.

Consequence:
- **Neutral, horizontal board (any wave steepness)** → `worldRelativeRollSin ≈ 0` → `lateralAmount ≈ 0`
  → goes straight down the line. Fixes the involuntary into-wave carve.
- **Rider leans** → board rolls off horizontal → `worldRelativeRollSin ≠ 0` → carve proportional to the
  lean. `lateralTurnCoefficient` can be raised for strong, responsive turning (it multiplies ~0 at
  neutral, so a high gain does not re-introduce the bias) — fixes "turns too slowly".

## Requirements

- **FR1**: `lateralTurnForce` magnitude and direction key on `worldRelativeRollSin`, not
  `waveRelativeRollSin`.
- **FR2**: `worldRelativeRollSin` is 0 for a board level in the world, regardless of wave slope, and
  grows with roll away from horizontal; its sign indicates the leaned-to side.
- **NFR1**: Do **not** change the other consumers of `waveRelativeRollSin` — the rail-lift submersion
  gate ([FluidDynamics.cpp:684](../Source/GoneSurfing/FluidDynamics.cpp#L684), reads sign only) and the
  torque diagnostic ([SharedCalculations.cpp:123](../Source/GoneSurfing/SharedCalculations.cpp#L123)).
  Keep `waveRelativeRollSin` intact and `worldRelativeRollSin` additive.
- **NFR2**: `lateralTurnMultiplier`, pitch scaling, speed/planing factors, and the back-actor
  counter-carve handling are unchanged ([[project_lateral_turn_back_counter_carve]]).

## Implementation Details

1. `SharedCalculations.h`: add `worldRelativeRollCos` / `worldRelativeRollSin` UPROPERTYs next to the
   wave-relative ones.
2. `SharedCalculations.cpp`: compute them right after the wave-relative roll block (reuse `forwardsN`).
   Add to the STATE debug log line for tuning visibility.
3. `FluidDynamics.cpp::calcLiftForce` (lateralTurn block, ~line 809): read `worldRelativeRollSin` in
   place of `waveRelativeRollSin` for `lateralAmount` and `rollSign`.
4. Update the `lateralTurnCoefficient` doc comment in `FluidDynamics.h`.

## Acceptance Criteria

- **AC1**: With `lateralTurnCoefficient > 0` and centered weight, the board surfs straight down the line
  on a steep face — no involuntary yaw into the wave (torque budget: `lateralTurn` net yaw ≈ 0 at neutral).
- **AC2**: A deliberate lean produces a prompt, proportional carve; turn rate scales with
  `lateralTurnCoefficient`, restoring responsive steering.
- **AC3**: Rail lift and the torque diagnostic are unaffected (still read `waveRelativeRollSin`).

## Status

- [x] Root cause confirmed (torque budget; wave-relative roll large at neutral on a steep face)
- [ ] Add `worldRelativeRollSin` to SharedCalculations
- [ ] Switch `lateralTurnForce` to `worldRelativeRollSin`
- [ ] Re-enable `lateralTurnCoefficient` and tune (AC1/AC2)
- [ ] Verify rail lift / diagnostics unchanged (AC3)

## Related

- [[project_lateral_turn_wave_relative_roll]] — the original flag that the wave-relative roll carves involuntarily.
- [lateral-turn-uniform-along-length.md](lateral-turn-uniform-along-length.md) — why `amountUnderWater` is excluded.
- [[project_lateral_turn_back_counter_carve]] — `lateralTurnMultiplier` zeroes tail actors.
- [barrel-glide-through-bug.md](barrel-glide-through-bug.md), [wave-penetration-resistance.md](wave-penetration-resistance.md) — the board now stays on the face (so this carve became visible).
