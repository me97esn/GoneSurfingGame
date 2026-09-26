# Spec: Reduce Planing Drag Attenuation (drop the squaring)

## Overview

Change the planing-attenuation factor on bottom and rail forward-skin-friction drag from `(1 - AmountPlaning)²` to `(1 - AmountPlaning)`. Linear instead of quadratic. Net effect: ~6× more drag during full planing (`AmountPlaning ≈ 0.85`).

Scope: two single-character formula changes in `AFluidDynamics::calcDragForce`. No new UPROPERTYs, no new force shapes.

## Motivation

Captured during the wave-slope-gravity-supplement iteration (see [wave-slope-gravity-supplement.md](wave-slope-gravity-supplement.md)). With the supplement properly gated, the board still cruised indefinitely after the burst:

| t (s) | vx | slopeSin | planing |
|---|---|---|---|
| 7.5 | -567 | 0.03 | 0.85 |
| 9.0 | -667 | 0.04 | 0.85 |
| 10.5 | -754 | 0.02 | 0.85 |

Board still gaining +187 cm/s over 3 seconds during what should be cruise. Debug-log force inventory showed propulsion in the ~3 kN range (yaw hydrofoil + bottom-hydrofoil forwards thrust), against drag attenuated to `(1-0.85)² ≈ 2.25%` of nominal. With drag effectively suppressed during planing, there is no equilibrium speed — net force is steadily positive.

The original `(1-planing)²` was a tuning choice with no obvious physical motivation. Linear attenuation is the more defensible default: planing reduces drag (Reynolds, lifted hull) but doesn't suppress it almost-entirely. The previous `(1-planing)²` was effectively saying "a planing board has 50× less drag than a stationary one" — exaggerated even for a planing surfboard.

## Design

Two single-character changes in [FluidDynamics.cpp](../Source/GoneSurfing/FluidDynamics.cpp):

**Bottom forward drag, line 94:**
```cpp
// Before:
pow((1 - this->sharedCalculations->AmountPlaning), 2)
// After:
(1 - this->sharedCalculations->AmountPlaning)
```

**Rail forward drag, line 225:**
```cpp
// Before:
fwdPlaningAttenuation = FMath::Pow(1.0f - this->sharedCalculations->AmountPlaning, 2);
// After:
fwdPlaningAttenuation = 1.0f - this->sharedCalculations->AmountPlaning;
```

No other formulas touched. Specifically left as-is:
- **Tail drag** — already uses a different shape (`1 - planing × (1 - cosWaterForwards)`); decoupled from this change.
- **Fin drag** — no planing attenuation (uses cos⁴ angle factor instead).
- **Sideways drag** (bottom and rail) — no planing attenuation by design (wetted hull still feels lateral flow when planing).
- **Wave-mass drag** — no planing attenuation (it's a wave-momentum push, not skin friction).

### Expected consequence: wave-catching breaks

At low planing (`AmountPlaning ≈ 0.0–0.3`, the bootstrap regime), the linear vs squared formulas are similar:
- `(1-0.0)² = 1.00` vs `(1-0.0) = 1.00`  (identical)
- `(1-0.3)² = 0.49` vs `(1-0.3) = 0.70`  (linear is 1.4× more)

But the drag coefficients have been tuned against the squared formula for years. With the squaring dropped, low-planing drag rises 1.4× and full-planing drag rises 6.6×. The most likely failure mode is that the board can no longer accelerate to planing speed in the first place — drag exceeds the supplement + gravity-along-slope + hydrofoil thrust budget during the pre-planing bootstrap.

**Mitigation:** scale `bottomDragCoefficient` and `railDragCoefficient` down to compensate, until wave-catching is restored. The drag-vs-planing-state *shape* changes (more linear), but the absolute force magnitude during planing can be set to whatever produces "decelerates but doesn't refuse to surf". Likely starting point: ~0.5× current values (compensates for the 1.4× pre-planing penalty, restores wave-catching). Tune by iteration.

If hardcoded experimental coefficients are wanted (e.g. to test the shape without touching the umap), they can be applied locally at the formula site as a temporary scalar — see Iteration plan below.

## Iteration plan

1. Apply the shape change. Build, run, observe.
2. If board doesn't catch wave: hardcode a multiplier on the offending drag coefficient at the formula site (e.g. `bottomDragCoefficient * 0.5f`) and iterate.
3. Once wave-catching works at some scaled coefficient, verify cruise behavior: board should reach planing, glide briefly, then decelerate visibly within ~3-5 seconds of post-burst.
4. If acceptable, move the scaled coefficient into the umap and remove the hardcoded multiplier.

## Acceptance Criteria

### AC1 — Compiles, mechanical change only

Two formula changes in `calcDragForce`; no header changes, no new UPROPERTY.

### AC2 — Board catches the wave

After whatever coefficient re-tuning is needed, `surf-straight` autopilot still triggers the planing transition (`AmountPlaning` reaches > 0.5) at roughly the same gameSeconds as the pre-change reference (within ±1 s).

### AC3 — Board decelerates after burst

In the post-burst cruise window (after the supplement-gate cuts the slope-down force), board's speed magnitude decreases over the next 3–5 seconds, instead of staying constant or growing.

### AC4 — Drag coefficients documented

Whatever final coefficients land in the umap, add a comment to the relevant UPROPERTY in [FluidDynamics.h](../Source/GoneSurfing/FluidDynamics.h) noting the shape change so future-readers don't expect the old tuning regime.

## Open Questions / Future Work

1. **Tail-drag formula consistency.** Tail drag already uses a linear-in-planing shape (`1 - planing × (1 - cosWaterForwards)`). Should bottom and rail forward drag adopt the same shape (planing-only-attenuates-when-flow-is-parallel)? Defer until current change is validated; this would be a deeper redesign.
2. **Per-actor planing-attenuation shape.** Front actors might want different attenuation than back actors (rocker keeps them at different waterlines). Out of scope for this spec.
3. **Replace planing-attenuation with explicit wetted-area model.** The "correct" physics is that drag scales with how much hull is touching water, not with the planing state machine. Would require modeling wetted-area per actor. Big change; out of scope.

## Status

- [x] (1-planing)² → (1-planing) applied at FluidDynamics.cpp:94 (bottom) and :231 (rail)
- [x] Test run shows wave-catching still works (AC2)
- [x] Drag coefficients tuned: hardcoded 10× multiplier in code (`kExperimentalBottomDragMultiplier`, `kExperimentalRailDragMultiplier`) as a stop-gap; final coefficient should move into the umap
- [x] Test run shows cruise-phase deceleration (AC3) — see also [bottom-hydrofoil-thrust.md](bottom-hydrofoil-thrust.md) turn-gate addendum
- [ ] Move 10× into umap `bottomDragCoefficient`/`railDragCoefficient` and remove hardcoded multipliers
- [ ] Final coefficients documented in header (AC4)

## Outcome notes (2026-05-24)

The linear-attenuation change alone was *not* enough. Iteration found the dominant cruise propulsion was the bottom-hydrofoil's `forwardsThrustCoefficient` (decoupled forward thrust), which was firing on any positive pitch AOA — including straight-line surf — and re-introducing the indefinite cruise problem. The fix that actually landed:

1. Drop the squaring → linear attenuation (this spec)
2. Bump bottom + rail drag coefficients **10×** (hardcoded scalar)
3. **Turn-gate** the bottom-hydrofoil forward thrust on `|sinSlip|` (smoothstep 0.05 → 0.20) — see addendum in [bottom-hydrofoil-thrust.md](bottom-hydrofoil-thrust.md)

Combined, these give: burst peak ~-530 cm/s → rapid decel to ~-300 cm/s equilibrium → slow further decay. Board never crashes through to "indefinite cruise" anymore.
