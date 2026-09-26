# Spec: Wave-Mass Thrust (Forward Propulsion from Sideways Wave Flow)

## Overview

Add a new forward-thrust term on the **bottom** and **rail** surfaces that converts the *sideways* component of wave-driven water flow into propulsion along `board.forwards`. Symmetric partner to the [wave-mass-drag](wave-mass-drag.md) spec: drag opposes the board's motion through the wave volume; thrust extracts forward propulsion from the wave's lateral water mass.

Scope: additive blocks in the existing per-side branches of `AFluidDynamics::calcDragForce`. Single shared coefficient (`waveMassThrustCoefficient`), default 0 (disabled).

## Motivation

Observation: with wave-mass drag at 1e-3, the board no longer free-glides through the wave (good), but feels too slow when surfing "sideways touching the breaking wave" — exactly the scenario where real surfers see their highest speeds. The wave's lateral water mass is large at that position, the sideways drag (and the wave-mass drag's resistive component) registers it as a **force on the board**, but currently none of that force translates to **forward propulsion**.

The physics: a yawed planing surface in a perpendicular flow deflects the flow and feels a reaction force perpendicular to the flow direction in the plane of the surface. Decomposed in the board frame, that perpendicular force has **both anti-slip and forward components**. Real surfboards (and sailboats across the wind) extract forward propulsion this way. The codebase already has the existing yaw hydrofoil ([FluidDynamics.cpp:745-805](../Source/GoneSurfing/FluidDynamics.cpp#L745) — see [bottom-yaw-hydrofoil.md](bottom-yaw-hydrofoil.md)), but it's intentionally capped at 5 kN per actor by `maxHydrofoilForceAmount` to prevent the spin-out we worked around earlier, and its forward component scales as `sin²(slip)` so it's geometrically small at typical slips.

What this spec adds is a **wave-face-gated** forward thrust that fires only on steep wave geometry (`slopeSin > 0`) and is fed by the same `effectiveWaterHeight` that the wave-mass drag uses. Gating to wave faces means flat-water carving is untouched (the yaw hydrofoil is the only flat-water mechanism), so the cap that prevents flat-water spin-out is preserved. On wave faces, this new term adds the propulsion that real surfers experience.

## Background

### Symmetric pair with wave-mass drag

| Term | Input | Direction | Effect |
|---|---|---|---|
| Wave-mass **drag** ([wave-mass-drag.md](wave-mass-drag.md)) | `relVelAlong` (parallel to board.forwards) | along flow | resists motion through wave volume |
| Wave-mass **thrust** (this spec) | `sidewaysVel` (perpendicular to board.forwards) | along board.forwards | propels along board.forwards |

Same shape: `coef × effectiveWaterHeight × slopeSin × v_relevant² × direction`. Same `slopeSin` gate. Both are zero on flat water; both grow with the wave-mass proxy. Together they model the wave as **a column of water that exchanges momentum with the board** — taking energy from the board's forward motion (drag) while giving it forward propulsion from its lateral mass (thrust).

### How this differs from the existing yaw hydrofoil

The yaw hydrofoil ([FluidDynamics.cpp:745](../Source/GoneSurfing/FluidDynamics.cpp#L745)) and wave-mass thrust both convert sideways flow into a force with a forward component. The differences:

| | Yaw hydrofoil | Wave-mass thrust |
|---|---|---|
| Where it fires | Anywhere the board has slip | Wave faces only (`slopeSin > 0`) |
| Force shape | Perpendicular to in-plane flow: anti-slip + forward | Pure forward |
| Forward scaling | `sin²(slip)` (geometric: cosSlip × absSinSlip × yawThrustMag where yawThrustMag has another absSinSlip) | linear in sideways velocity² |
| Per-actor cap | 5 kN via `maxHydrofoilForceAmount` | shares `maxDragAmount` (same as drag) |

The yaw hydrofoil is the *general-purpose* mechanism: it works everywhere there's slip, with anti-slip naturally coupling forward speed to direction control. Wave-mass thrust is the *wave-specific* mechanism: it converts the wave's water mass directly into forward speed when riding the face.

The two coexist without overlap: the yaw hydrofoil contributes its bounded share, the wave-mass thrust adds on top when on a wave face. Tuning each independently lets you set flat-water carving feel (yaw hydrofoil) separately from wave-face acceleration (wave-mass thrust).

### Why no tail term

Tail drag ([FluidDynamics.cpp:225-256](../Source/GoneSurfing/FluidDynamics.cpp#L225)) already produces a forward force when water comes from behind (`cosWaterForwards > 0`, force direction `board.forwards`). The existing tail formula and the wave-mass drag I added to it both push forward in that scenario, so the "water mass driving the board forward via the tail" case is already captured. Adding a sideways-fed thrust on the tail would double-count. **Bottom and rails only.**

## Design

### Force shape

Per surface, force in the `board.forwards.GetSafeNormal()` direction:

```
waveMassThrust = waveMassThrustCoefficient
               × boardWideEffectiveWaterHeight  // board-wide water-mass proxy
               × boardWideSlopeSin              // board-wide wave-face gate (zero on flat water)
               × sidewaysVel.SizeSquared()      // sideways flow contribution
```

Direction: `board.forwards` (normalized to avoid the scale-leak issue the wave-mass drag inherits from the unnormalized `sharedCalculations->forwards`).

**Both `slopeSin` and the water-column part of `effectiveWaterHeight` are *board-wide* values, sampled once at the SharedCalculations actor's position rather than per-actor. The wave-mass thrust represents the wave's lateral water mass pushing the board forward as a single phenomenon; per-actor sampling of `slopeSin` would inject discretization noise that becomes a spurious yaw torque because L/R-symmetric actors would see slightly different sampled slopes. See [per-actor-vs-board-wide-sampling.md](per-actor-vs-board-wide-sampling.md) for the principle. The per-actor `baseHeight` and `slopeHeight` stay because they're typically uniform across the board's FluidDynamics actors anyway.**

### Per-surface details

| Surface | `sidewaysVel` source | Gate to keep |
|---|---|---|
| Bottom (`VE_Down`) | Existing `sidewaysVel` from the sideways drag block — `VectorPlaneProject(VectorPlaneProject(relWaterVel, boardUp), boardForwards)` | none |
| Rail left/right (`VE_Left`/`VE_Right`) | **skipped — see below** | n/a |

Sideways drag and wave-mass thrust on the bottom read the **same** `sidewaysVel`. They produce orthogonal forces: drag along `sidewaysVel`, thrust along `board.forwards`. Adding both is physically consistent — the wave's lateral flow hits the surface, gets partially absorbed (drag along the flow direction) and partially deflected (thrust perpendicular to the flow direction, in the board's plane).

### Rail wave-mass thrust intentionally skipped

The original spec design included rail wave-mass thrust gated by `sidewaysEngaged` (only the engaged rail contributes — same gate as rail sideways drag). Testing showed this produces a **single-sided** force: the left rail at peak might produce 4 kN of forward thrust while the lifted-out right rail produces 0 N. Applied at offset positions, the 4 kN differential generates a yaw torque ~20× bigger than the symmetric bottom thrust ever produces, runaway-yawing the board ~100°/s during a wave-touch event.

Real surfers compensate via weight shift, but the autopilot doesn't currently do that, and uncommanded yaw runaway is worse gameplay than slightly underestimated propulsion. **Skipped for the bottom-only first pass.** The bottom alone provides plenty of forward thrust (6 actors × ~500 N at coef=1e-4 = ~3 kN total). The rail term can be re-added later once the autopilot or a player can counter the yaw — see Open Questions.

### Why `slopeSin` and not just `effectiveWaterHeight`

`effectiveWaterHeight` is non-zero on flat water (it's `baseHeight + ...` with `baseHeight = 10`). If the thrust used only `effectiveH`, it would fire even when the board is in a calm trough moving sideways slightly — adding phantom acceleration. The `slopeSin` factor pins it to actual wave geometry. Same reasoning as wave-mass drag.

### Why normalize `board.forwards`

The existing per-surface drag formulas use the *unnormalized* `sharedCalculations->forwards`, which carries the SC actor's ~0.2 transform scale. That works for the existing drags because their coefficients are tuned for that scale convention. The new wave-mass *thrust* is in a definite physical direction (board forward) and its magnitude should mean what it says — `waveMassThrustCoefficient × inputs`. Normalizing decouples the coefficient's meaning from any actor-scale leak. Existing drags can stay as-is.

### Default value

Default `waveMassThrustCoefficient = 0.0f` (disabled — opt in via umap or BeginPlay override).

**Empirical bisection** (snapshot tests, 2026-05-22): the thrust formula's `sidewaysVel²` is ~130k at the wave-touching position. With `effectiveH=200` and `slopeSin=0.5`, each bottom actor produces `coef × 13,000,000` Newtons of force. Useful tuning target ~500 N per actor (≈ 100 m/s² total board acceleration with 10 bottom actors and a 50 kg board) → **coef ≈ 1e-4**. Confirmed in test: 1e-4 produces the surfer-like "slows on wave contact, accelerates to higher speed than before contact" trajectory. 1e-3 (the wave-mass drag coefficient) was 10× too high and stalled the board with yaw runaway.

**Important**: this coefficient is NOT on the same scale as `waveMassDragCoefficient` despite the symmetric force shape. The drag uses the unnormalized `boardForwards · relWaterVel` (which inherits a ~0.2 SC transform scale, producing `relVelAlong²` ~400), while the thrust uses the bottom-plane projection `sidewaysVel` which is properly scaled (no leak, ~130k). The two coefficients are ~300× apart in physical magnitude. Don't reuse one as a starting point for the other.

## Implementation Sketch

In [FluidDynamics.cpp:62](../Source/GoneSurfing/FluidDynamics.cpp#L62), inside `calcDragForce`.

### Bottom (`case ESide::VE_Down`)

Inside the existing sideways drag block, right after computing `sidewaysVel` and the sideways drag, before assigning `dragForce`:

```cpp
// Wave-mass thrust — sideways wave flow converts to forward propulsion on a wave face.
// Symmetric partner to the wave-mass drag above; same input (sidewaysVel) but force direction
// is +board.forwards instead of along the sideways flow. See specs/wave-mass-thrust.md.
FVector waveMassThrust = FVector::ZeroVector;
if (this->waveMassThrustCoefficient > 0.0f && this->slopeSin > 0.0f)
{
    const float thrustAmount = this->waveMassThrustCoefficient
                             * this->effectiveWaterHeight
                             * this->slopeSin
                             * sidewaysVel.SizeSquared();
    waveMassThrust = boardForwards.GetSafeNormal()
                   * FMath::Clamp(thrustAmount, 0.0f, maxDragAmount);
    if (debugDragLog)
    {
        UE_LOG(LogTemp, Warning, TEXT("  waveMassThrust (bottom): coef=%.4f, slopeSin=%.3f, sidewaysVel²=%.1f -> amount=%.2f, thrust=(%.2f, %.2f, %.2f) mag=%.2f"),
            this->waveMassThrustCoefficient, this->slopeSin, sidewaysVel.SizeSquared(),
            thrustAmount, waveMassThrust.X, waveMassThrust.Y, waveMassThrust.Z, waveMassThrust.Length());
    }
}

dragForce = forwardDrag + sidewaysDrag + waveMassThrust;
```

### Rail (`case ESide::VE_Left`/`VE_Right`)

Inside the existing `sidewaysEngaged` block, after computing the rail sideways drag:

```cpp
if (sidewaysEngaged && this->waveMassThrustCoefficient > 0.0f && this->slopeSin > 0.0f)
{
    const float thrustAmount = this->waveMassThrustCoefficient
                             * this->effectiveWaterHeight
                             * this->slopeSin
                             * sidewaysVel.SizeSquared();
    sidewaysDrag += boardForwards.GetSafeNormal()
                  * FMath::Clamp(thrustAmount, 0.0f, maxDragAmount);
}
```

(Adding to `sidewaysDrag` since it's already part of the final `dragForce = forwardDrag + sidewaysDrag` sum in the rail branch. Alternatively, introduce a separate `railWaveMassThrust` variable for clarity in the debug log.)

### Single new UPROPERTY

In [FluidDynamics.h](../Source/GoneSurfing/FluidDynamics.h), next to `waveMassDragCoefficient`:

```cpp
/** Wave-mass thrust coefficient. Converts the sideways component of wave-driven water flow
 *  into forward propulsion along board.forwards. Symmetric partner to waveMassDragCoefficient:
 *  drag opposes motion through the wave volume, thrust extracts forward propulsion from the
 *  wave's lateral water mass. Magnitude scales with
 *  effectiveWaterHeight × slopeSin × sidewaysVel² × this coefficient. Gated to steep wave
 *  faces by slopeSin so flat-water carving is untouched. Default 0 = disabled. Set on
 *  bottom and rail actors. See specs/wave-mass-thrust.md. */
UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(ClampMin = "0.0"))
float waveMassThrustCoefficient = 0.0f;
```

### Debug visualization

When `surf.debug.flags 'forces'` is set, draw the new term as a distinct-colored arrow alongside the existing per-surface drag draws. Suggested color: green (matches "forward propulsion" intuition).

## Acceptance Criteria

### AC1 — Compiles, single new UPROPERTY

`AFluidDynamics::waveMassThrustCoefficient` exposed as `UPROPERTY(EditAnywhere, BlueprintReadWrite)` with a comment referencing this spec.

### AC2 — Disabled state preserves pre-spec behavior

With `waveMassThrustCoefficient = 0.0` (default), all drag forces are bit-for-bit identical to pre-spec.

### AC3 — Forward acceleration on the wave face

With a non-zero coefficient, the board surfing "sideways touching the breaking wave" (high `slopeSin`, high `sidewaysVel`, board aligned along the wave) gains forward speed materially faster than it does today. Target: peak `vy` in step 4 of `surfing-down-the-line` increases by at least 20% over the current baseline at the chosen tuning value.

### AC4 — Flat water unchanged

`surf-straight` snapshot trajectory is unchanged. On flat water `slopeSin ≈ 0`, so the new term contributes nothing.

### AC5 — Wave-mass drag still active

The wave-mass drag's slowdown effect during wave-volume crossings is not undone by the new thrust. Specifically: the board's `vy` still drops measurably when penetrating the wave (the test from wave-mass-drag.md AC3), it just doesn't drop as much because the sideways thrust is partially offsetting the parallel drag.

### AC6 — Debug log shows the new term

Per-surface "waveMassThrust" log lines appear under `surf.debug.flags 'drag'`, alongside the existing drag and wave-mass-drag lines.

## Open Questions / Future Work

1. **Per-surface coefficients.** If tuning shows bottom vs rails want very different thrust response, split into `bottomWaveMassThrustCoefficient` / `railWaveMassThrustCoefficient`. Defer until needed.
2. **Couple with the existing yaw hydrofoil cap.** Once both terms are active, the per-actor force budget may need revisiting. If wave-mass thrust pushes per-actor force past the existing yaw hydrofoil cap, that cap may need to apply to the new term too — or the wave-mass thrust should get its own cap. Reassess after tuning.
3. **Symmetric force on the wave?** Real waves lose energy to the surfer that they propel. This spec adds energy to the board without removing it from the wave model — the wave is a free energy source. That's a longstanding sim simplification; out of scope here but worth flagging.
4. **Tail term.** Skipped for the reason above (existing tail drag already pushes forward from behind-water). Revisit only if tail behavior turns out to need this contribution after all.

## Status

- [ ] `waveMassThrustCoefficient` UPROPERTY added to `AFluidDynamics`
- [ ] Bottom: thrust term added inside the sideways drag block
- [ ] Rail (left/right): thrust term added inside the `sidewaysEngaged` block
- [ ] Debug log shows `waveMassThrust` per surface
- [ ] AC2 verified (disabled = unchanged)
- [ ] AC3 verified (forward acceleration on the wave face)
- [ ] AC4 verified (flat water unchanged)
- [ ] AC5 verified (wave-mass drag still slows wave-volume crossing)
- [ ] Coefficient bisected to a stable value
- [ ] `surfing-down-the-line` re-baselined if trajectory shifts materially
