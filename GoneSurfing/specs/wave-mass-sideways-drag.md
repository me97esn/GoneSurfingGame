# Spec: Wave-Mass Sideways Drag (Symmetric Partner to Wave-Mass Thrust)

## Overview

Add a wave-mass-scaled term to the **sideways** drag direction on the bottom and rail surfaces, mirroring the existing [wave-mass-thrust](wave-mass-thrust.md) which routes the same input (`absSidewaysVel²`) into the **forward** direction. Closes a structural asymmetry where, on a steep wave face, the same sideways water flow gets a `slopeSin × waveMassCoefficient` boost when it converts to forward thrust but no equivalent boost when it should resist the board sideways.

Scope: additive blocks alongside the existing sideways drag in `AFluidDynamics::calcDragForce`. Single new coefficient (`waveMassSidewaysDragCoefficient`), default 0 (disabled). No changes to forward drag, lift, or thrust.

## Motivation

When the board hits a breaking wave at a small angle ("turns into the wave from the trough, expects to be pushed sideways"), it instead **smoothly accelerates forward through the wave** with no meaningful sideways deflection. Snapshot data at the wave-touching moment of `surfing-down-the-line` (gameS ≈ 8.79 → 8.92, run 2026-05-25):

| metric | value | direction |
|---|---|---|
| `absSidewaysVelMag` (wave's lateral water flow, per bottom actor) | 87 → 438 cm/s | growing through hit |
| `slopeSin` (wave face steepness) | 0.03 → 0.53 | growing |
| `amountUnderWater` | 0.48 → 1.00 | full submersion |
| Board vy | 712 → 2452 cm/s | **+10 m/s² forward acceleration** |
| Board vx | 197 → 329 cm/s | barely changed |

The wave's sideways flow has more than enough energy to knock the board off course, and the sideways drag is uncapped and firing at large magnitude. But the same energy is being multiplied through *two* mass-scaled forward channels (`waveMassThrust` and the yaw hydrofoil's induced fwd component) and only *one* unscaled sideways channel. The board ends up *carried by the wave* (correct on a clean face) when it should also be *punched aside by the breaking lip* (missing).

### The asymmetry, side-by-side

Same `absSidewaysVel²` input feeds three terms today. They scale very differently:

| term | direction | mass-proxy | wave-face gate | coefficient (current scene) |
|---|---|---|---|---|
| `waveMassThrust` (bottom) | **+board.forwards** | `boardWideEffectiveH` | `× boardWideSlopeSin` | 0.0100 |
| `sidewaysDrag` (bottom) | along `absSidewaysVel` | `effectiveH` | none | 0.0010 |
| `sidewaysDrag` (rail, engaged) | along `absSidewaysVel` | `effectiveH` | none | `railDragSidewaysCoefficient` |

At `slopeSin = 0.33` (mid-wave-hit), the forward-direction multiplier is `0.0100 × 0.33 = 0.0033`, while the sideways multiplier is `0.0010`. **The wave gets to push you forward 3.3× harder than it pushes you sideways per unit of sideways-flow energy** — and the multiplier grows as the wave steepens (`slopeSin` ↑).

Adding the symmetric `waveMassSidewaysDragCoefficient` puts back the missing channel so that on a breaking lip (high `slopeSin`), the wave's sideways momentum can dissipate as actual lateral force on the board, not only as induced forward thrust.

## Background

### Why not just raise `bottomDragSidewaysCoefficient`?

Raising the base coefficient would increase sideways drag everywhere — including ordinary carving where the regular sideways drag is already correctly tuned to bleed the right amount of speed during turns. Slip-side drag in flat-water/clean-face carving is a different physical regime (skin friction along a slightly yawed surface) from impact-side drag in a breaking-wave hit (wave's body of water slamming the rail). Tuning them with a single coefficient forces a compromise between "carves bleed too much speed" and "wave hits don't knock you over."

The `slopeSin` gate naturally separates the two regimes. Flat-water and clean-face carving have low `slopeSin` (≈ 0.05–0.15 in the test) — the new term contributes little there. Breaking lip impact has high `slopeSin` (≈ 0.3–0.6) — the new term dominates only when the wave is steep enough to physically be a breaking impact. Same pattern that `waveMassThrust` uses for its own gating, mirrored on the resisting side.

### Why not just remove `effectiveH` from lift/thrust?

Considered in conversation (2026-05-25). Removing `effectiveH` from the yaw hydrofoil's forward-induced-thrust would cut its contribution by ~200× and break the steady-carve speed boost we just tuned in via `finLiftMagnitude`. That tuning relies on the mass-proxy multiplier surviving in lift/thrust. The cleaner shape is to add the missing **sideways** channel rather than subtract from the existing **forward** channels.

### Relationship to `maxEffectiveWaterHeight`

The `effectiveWaterHeight` ceiling (currently 200 cm) saturates during the wave hit — every log line shows `effectiveH: 200.00` while raw inputs `col + slopeSin × slopeHeight` would push past it. This caps **both** the forward and sideways channels equally, so the asymmetry above is unchanged by the cap. Raising the cap globally would help the sideways drag, but it would *also* boost `waveMassThrust` proportionally — preserving the asymmetry. The fix has to be channel-specific.

If a future change uncaps `effectiveH` for drag only (the alternative we discussed), this spec composes cleanly with it: the wave-mass sideways drag would benefit from the uncapped value the same way the existing sideways drag would.

## Design

### Force shape

Per surface, force along the wave's actual sideways flow direction:

```
amount = waveMassSidewaysDragCoefficient
       × effectiveWaterHeight
       × slopeSin
       × absSidewaysVel.SizeSquared()

direction = absSidewaysVel.GetSafeNormal()
```

Same shape as `waveMassThrust` ([FluidDynamics.cpp:171-192](../Source/GoneSurfing/FluidDynamics.cpp#L171-L192)) — same inputs, same `slopeSin` gate, same `effectiveWaterHeight` mass proxy — but direction is `absSidewaysVel` (along the wave's lateral flow) instead of `+board.forwards`.

Adds to the existing `sidewaysDrag` vector. The result is that the wave's sideways momentum produces a force vector with components in **both** directions: forward (via `waveMassThrust`, modeling "the wave carries you along the face") and sideways (via this term, modeling "the wave's body slams you laterally"). Their ratio depends on the two coefficients, which can be tuned independently to control the balance between "carried by the wave" and "knocked sideways by the wave."

### Per-surface details

| Surface | Sideways drag location | Add new term? | Notes |
|---|---|---|---|
| Bottom (`VE_Down`) | [FluidDynamics.cpp:151](../Source/GoneSurfing/FluidDynamics.cpp#L151) | yes | symmetric L/R bottom actors → no yaw torque concern |
| Rail left/right (`VE_Left`/`VE_Right`) | [FluidDynamics.cpp:262](../Source/GoneSurfing/FluidDynamics.cpp#L262) | yes, gated by existing `sidewaysEngaged` | see yaw-torque note below |

### Rail yaw-torque consideration

The existing rail sideways drag already fires single-sided (only the rail facing into the absolute lateral flow contributes — `sidewaysEngaged`). The new wave-mass term, applied at the same engaged rail with a `slopeSin × coef` multiplier, amplifies that single-sided force. Applied at the rail's offset from COM, that creates a yaw torque.

Two reasons this is probably OK to ship together with the bottom term, unlike the symmetric concern for `waveMassThrust`:

1. **Direction.** `waveMassThrust` on a single rail pushes along `+board.forwards` — perpendicular to the rail's offset from COM, so the moment arm is maximized. The wave-mass sideways drag pushes along `absSidewaysVel` — which roughly aligns with the rail-to-COM direction itself, so the moment arm is reduced compared to the thrust case.
2. **Sign.** Sideways drag at the engaged rail tends to push the board *with* the wave's flow direction, which rotates the board *toward* the flow. That's directionally what you'd want during a wave hit — the board getting punched into the direction of the wave's lateral push. The thrust at a single rail rotated the board *into* the wave (always +board.forwards), which had no physical justification.

If integration testing shows the rail term still produces excessive yaw rotation, gate it the same way `waveMassThrust` was gated to bottom-only (skip it on rails, see Open Questions below).

### Default value

Default `waveMassSidewaysDragCoefficient = 0.0f` (disabled — opt in via umap or BeginPlay override).

**Starting tuning estimate:** The existing `waveMassThrustCoefficient = 0.0100` produces ≈ 5377 N per bottom actor at peak (log: wall 14:49:05:213, `boardWideSlopeSin=0.332, boardWideEffH=200, absSidewaysVel²=8981`). For symmetric sideways/forward partitioning of wave-mass energy, start at `waveMassSidewaysDragCoefficient ≈ 0.0050–0.0100`. The choice within that range is a feel question: 0.01 means the sideways and forward channels are equal-weighted, 0.005 means the wave still preferentially carries you (forward dominant) but with a real sideways punch. Bisect against the `surfing-down-the-line` autopilot once enabled — target: the board's vx changes by at least 200 cm/s during the wave-hit window (gameS 8.80 → 8.95), instead of the current ~30 cm/s.

## Implementation Sketch

In [FluidDynamics.cpp](../Source/GoneSurfing/FluidDynamics.cpp), `AFluidDynamics::calcDragForce`.

### Bottom (`case ESide::VE_Down`)

Right after the existing bottom sideways drag block ([line 151](../Source/GoneSurfing/FluidDynamics.cpp#L151)), before the `waveMassThrust` block:

```cpp
// Wave-mass sideways drag — symmetric partner to waveMassThrust on the resisting axis.
// Same input (absSidewaysVel²) and same wave-face gate (slopeSin), but force direction is
// along the wave's lateral flow rather than +board.forwards. Closes the asymmetry where
// the wave's sideways momentum currently routes to forward thrust without an equivalent
// resisting channel. See specs/wave-mass-sideways-drag.md.
if (this->waveMassSidewaysDragCoefficient > 0.0f && this->slopeSin > 0.0f)
{
    const float waveMassSidewaysAmount = this->waveMassSidewaysDragCoefficient
                                       * this->effectiveWaterHeight
                                       * this->slopeSin
                                       * absSidewaysVel.SizeSquared();
    const FVector waveMassSidewaysDrag = absSidewaysVel.GetSafeNormal()
                                       * FMath::Clamp(waveMassSidewaysAmount, 0.0f, maxDragAmount);
    sidewaysDrag += waveMassSidewaysDrag;
    if (debugDragLog)
    {
        UE_LOG(LogTemp, Warning, TEXT("  waveMassSidewaysDrag (bottom, abs): coef=%.4f, slopeSin=%.3f, absSidewaysVel²=%.1f -> amount=%.2f, drag=(%.2f, %.2f, %.2f) mag=%.2f"),
            this->waveMassSidewaysDragCoefficient, this->slopeSin, absSidewaysVel.SizeSquared(),
            waveMassSidewaysAmount,
            waveMassSidewaysDrag.X, waveMassSidewaysDrag.Y, waveMassSidewaysDrag.Z,
            waveMassSidewaysDrag.Length());
    }
}
```

The existing `dragForce = forwardDrag + sidewaysDrag + waveMassThrust` at line 194 picks up the new contribution via the `sidewaysDrag +=` above. No change to the assembly line.

### Rail (`case ESide::VE_Left` / `VE_Right`)

Inside the existing `sidewaysEngaged` block ([line 260-270](../Source/GoneSurfing/FluidDynamics.cpp#L260-L270)), right after the existing sideways drag assignment:

```cpp
if (this->waveMassSidewaysDragCoefficient > 0.0f && this->slopeSin > 0.0f)
{
    const float waveMassSidewaysAmount = this->waveMassSidewaysDragCoefficient
                                       * this->effectiveWaterHeight
                                       * this->slopeSin
                                       * absSidewaysVel.SizeSquared();
    sidewaysDrag += absSidewaysVel.GetSafeNormal()
                  * FMath::Clamp(waveMassSidewaysAmount, 0.0f, maxDragAmount);
    // Note: gated by the same sidewaysEngaged check (only the rail facing the flow
    // contributes). See spec for the yaw-torque discussion.
}
```

Update the rail debug log line at [line 275](../Source/GoneSurfing/FluidDynamics.cpp#L275) to include the new component (extend the existing format string with `waveMassSidewaysAmount: %.2f`).

### Single new UPROPERTY

In [FluidDynamics.h](../Source/GoneSurfing/FluidDynamics.h), next to `waveMassThrustCoefficient`:

```cpp
/** Wave-mass sideways drag coefficient. Symmetric partner to waveMassThrustCoefficient on
 *  the resisting axis. Converts the sideways component of wave-driven water flow into
 *  lateral resistance along the wave's flow direction (mirror of waveMassThrust which routes
 *  the same input into forward propulsion). Magnitude scales with
 *  effectiveWaterHeight × slopeSin × absSidewaysVel² × this coefficient. Gated to wave faces
 *  by slopeSin > 0 so flat-water carving is untouched. Default 0 = disabled. Set on bottom
 *  and rail actors. See specs/wave-mass-sideways-drag.md. */
UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(ClampMin = "0.0"))
float waveMassSidewaysDragCoefficient = 0.0f;
```

### Debug visualization

When `surf.debug.flags 'forces'` is set, draw the new component as a distinct arrow alongside the existing sideways drag. Suggested color: dark red (matches "resistive drag" intuition; distinguishes from the lighter red of regular sideways drag).

## Acceptance Criteria

### AC1 — Compiles, single new UPROPERTY

`AFluidDynamics::waveMassSidewaysDragCoefficient` exposed as `UPROPERTY(EditAnywhere, BlueprintReadWrite)` with a comment referencing this spec.

### AC2 — Disabled state preserves pre-spec behavior

With `waveMassSidewaysDragCoefficient = 0.0` (default), all drag forces are bit-for-bit identical to pre-spec. `surf-straight` and `surfing-down-the-line` snapshot baselines unchanged.

### AC3 — Sideways deflection on the wave face

With a non-zero coefficient (≥ 0.005), the board hitting the breaking wave in step 3 of `surfing-down-the-line` shows materially larger sideways deflection. Target: peak `|Δvx|` during the wave-hit window (gameS 8.80 → 8.95) increases from current ~30 cm/s to at least 200 cm/s. The board no longer "smoothly passes through" — it gets pushed perpendicular to its heading by an amount visible in the trajectory CSV's `x` column.

### AC4 — Flat water unchanged

`surf-straight` snapshot trajectory is unchanged regardless of coefficient value. On flat water `slopeSin ≈ 0`, so the new term contributes nothing.

### AC5 — Carving phase preserved

The trough-carving portion of `surfing-down-the-line` (gameS 6.5 → 7.5, the carve-arc we tuned in via `finLiftMagnitude`) is qualitatively unchanged. `slopeSin` is small in the trough (logs show < 0.15 during the carve), so the new term contributes negligibly there. Quantitative check: yaw trajectory through the carve stays within snapshot tolerance.

### AC6 — Debug log shows the new term

Per-surface "waveMassSidewaysDrag" log lines appear under `surf.debug.flags 'drag'`, alongside the existing sideways drag and wave-mass-thrust lines.

### AC7 — No spurious yaw runaway from rails

After enabling with a typical coefficient (0.005–0.01), the wave-hit moment does not produce uncommanded yaw rotation exceeding 100°/s (the threshold that flagged rail `waveMassThrust` as a problem in [wave-mass-thrust.md](wave-mass-thrust.md)). If exceeded, fall back to bottom-only (Open Question #1).

## Open Questions / Future Work

1. **Bottom-only fallback.** If rail integration produces yaw runaway like `waveMassThrust` did, restrict the term to bottom actors only. Bottom alone provides 5 actor-pairs of symmetric sideways drag, which should be enough for the impact behavior. The rail single-engagement asymmetry is the same risk the thrust faced.

2. **Per-surface coefficients.** If bottom and rail want different scaling (e.g., bottom resists steadily, rail contributes briefly during impact transitions), split into `bottomWaveMassSidewaysDragCoefficient` / `railWaveMassSidewaysDragCoefficient`. Defer until tuning demands it.

3. **Compose with `maxEffectiveWaterHeight` uncapping.** If a future change uncaps `effectiveH` for drag only (discussed 2026-05-25), this term would scale linearly with the uncapped value. Re-tune coefficient once that lands.

4. **Per-actor cap?** Today `waveMassThrust` and the regular drags share `maxDragAmount`. This term uses the same. If under tuning it competes with sideways drag for the budget at high `absSidewaysVel²`, consider giving it an independent cap or making it additive past `maxDragAmount`. Reassess after empirical bisection.

5. **Coupling with the hydrofoil-cap regime.** This term is purely additive to drag — it doesn't go through `maxHydrofoilForceAmount` (which only caps the yaw/pitch hydrofoils per [hydrofoil-force-cap.md](hydrofoil-force-cap.md)). So adding sideways drag here doesn't compound with the existing hydrofoil cap concerns. Worth noting for the broader "wave-hit force budget" discussion in the future.

## Status

- [x] `waveMassSidewaysDragCoefficient` UPROPERTY added to `AFluidDynamics`
- [x] Bottom: wave-mass sideways drag term added inside the sideways drag block
- [x] Rail (left/right): wave-mass sideways drag term added inside the `sidewaysEngaged` block
- [x] Debug log shows `waveMassSidewaysDrag` per surface (bottom and rail)
- [ ] Forces visualization includes the new arrow (dark red)
- [ ] AC2 verified (disabled = unchanged; baselines pass)
- [ ] AC3 verified (sideways deflection on wave face)
- [ ] AC4 verified (flat water unchanged)
- [ ] AC5 verified (carving phase preserved)
- [ ] AC7 verified (no rail-driven yaw runaway)
- [ ] Coefficient bisected to a stable value
- [ ] `surfing-down-the-line` re-baselined if trajectory shifts materially
