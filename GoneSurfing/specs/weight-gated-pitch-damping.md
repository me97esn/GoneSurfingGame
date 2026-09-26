# Spec: Weight-gated pitch damping

> **SUPERSEDED (2026-06-25) by [surface-relative-pitch-damping.md](surface-relative-pitch-damping.md).**
> This design damped nose-down rotation gated by flat-water proxies (weight-forward / nose-deeper-than-tail).
> Its own Open Question 5 identified the right signal as "pitch relative to the wave surface"; that signal
> (`waveRelativePitchSin`) now exists, and the successor spec keys an asymmetric (away/toward) pitch damping
> on it so it works on any surface shape — flat *and* steep wave. The `AngularDampingYNoseDownExtra`
> mechanism described below is removed by that work. Kept for history.

## Overview

Apply extra pitch-axis angular damping only on the **nose-down rotation direction**, and only when **the rider's weight is shifted forward AND the nose is currently deeper than the tail**. The rest of the time the damping is fully off, so the board pitches freely with wave slope, recovers naturally, and behaves identically to the current build during ordinary surfing.

Scope: a small engine-fork addition (sign-conditional extra damping on the local-Y angular velocity), and a project-side computation that pushes a gated value to the new engine CVar each tick.

## Motivation

Hang-ten test: when the rider walks to the nose (high `WeightDistribution::amountInFront`), the board nose-dives and slows. The user wants the surfboard to support nose-walks the way real surfboards do — visual pitch should stay in a reasonable surfing range even with full forward weight.

Previous attempts (committed and then reverted on 2026-06-02):

1. **`amountWetted` split** (see `amount-wetted-split.md`) — bottom-skin contact gets its own near-step scalar so hydrofoil coefficients can be tuned without the squeeze of the 100 cm + sigmoid `amountUnderWater` ramp. **Kept** — orthogonal to this spec, and used as one of the gate inputs here.
2. **`upwardsThrustPitchSensitivity`** — quadratic-in-sinAOA term on the up-axis thrust so high-AOA dive moments get more counter-torque. **Kept**, default 0 (lever exists; not tuned).
3. **Asymmetric `AngularDampingY{Pos,Neg}Extra`** — sign-dependent extra damping on the pitch-axis angular velocity. **Reverted** because the *value* of the extra was constant — it was always on, regardless of whether the board was in the dive-prone state. At values large enough to matter for the dive, it also suppressed pitch rotation that should have followed a rising wave slope. Cranking `YNegExtra` to 0.5 in editor produced a board that wouldn't pitch in either direction even with weight centered (visible as "no pitch motion at all").

The right shape is **conditional asymmetric damping** — the asymmetric mechanism (only damps one rotation direction) gated on two project-side signals (rider command OR current attitude), combined with a sign check. The damping engages when **EITHER** the rider has committed weight forward **OR** the nose is already deeper than the tail, **AND** the rotation is actually nose-down. Either of the first two is sufficient by itself: weight-forward says "the rider is in a dive-prone configuration", nose-deeper says "the board is already in a dive-prone attitude" — independent reasons to brake further nose-down rotation. The sign check then makes sure we only brake the bad direction.

## Background

### Why both sign-conditional AND project-side-gated

The pitch-axis angular velocity has two directions. The mechanism we want only damps one of them — the nose-down direction — so that natural nose-up recovery is never impeded. That sign check has to happen inside the integrator (which knows the angular velocity); it cannot be expressed by changing the value of a symmetric damping CVar.

But sign alone isn't enough. There's still natural nose-down rotation we don't want to dampen during ordinary surfing — bottom-turn carves, slope-following on a gentle wave, etc. The project-side gate disables the damping in those cases by requiring that the system be in a **dive-prone state** before engaging, signalled by either of two independent indicators:

- **Weight-forward** (`amountInFront → 1`): the rider has committed the hang-ten command. Gravity is now trying to rotate the nose down; this is the canonical dive scenario.
- **Nose-deeper-than-tail** (`front.amountWetted > back.amountWetted`): the board's current attitude is already nose-low. Further nose-down rotation deepens the dive regardless of why the board got there. This indicator catches dive moments that don't start from a weight shift (e.g. cresting a wave, sharp transition between wave faces).

Either signal alone is sufficient to suggest damping. They're combined with **OR**, then ANDed with the sign check.

### Signals available

- `AWeightDistribution::amountInFront` ∈ [0, 1] — 0.5 = centered, 1.0 = all weight at nose. Already read in `SurfboardUtils::Tick`.
- `ASharedCalculations::amountWetted` ∈ [0, 1] — bottom-skin contact signal introduced by `amount-wetted-split.md`. Each board has two SCs (front + back); the difference `front.amountWetted - back.amountWetted` is the nose-deeper-than-tail signal. Already wired into `ASurfboardUtils` (the actor has `sharedCalculationsFront` and `sharedCalculationsBack`).

### Why a single new CVar in the engine fork (not two)

The reverted attempt added both `YPosExtra` and `YNegExtra`. In practice only one direction is interesting for the hang-ten use case — the nose-down direction. A single CVar `AngularDampingYNoseDownExtra` keeps the engine API focused. If symmetric or other-direction extras are ever needed, they can be added later; YAGNI for now.

The engine-side sign convention is fixed at "the negative side of `W_local.Y`" because empirically that's the surfboard's nose-down rotation (verified 2026-06-02). If a future body has the opposite convention, the easiest fix is a per-body sign multiplier UPROPERTY pushed from the project, not engine-side configuration.

## Design

### Engine-fork addition

One new CVar `p.Chaos.Solver.AngularDampingYNoseDownExtra` (default 0) added next to the existing `AngularDampingX/Y/Z`. Applied in the angular damping block of `PBDRigidsEvolutionGBF.cpp` (~line 1057):

```cpp
const float yExtraNoseDown = (angularVelocityLocalSpace.Y < 0.0)
    ? CVars::AngularDampingYNoseDownExtra
    : 0.0f;
angularVelocityLocalSpace.X *= 1 - CVars::AngularDampingX;
angularVelocityLocalSpace.Y *= 1 - (CVars::AngularDampingY + yExtraNoseDown);
angularVelocityLocalSpace.Z *= 1 - CVars::AngularDampingZ;
```

At the default 0 value, behaviour is byte-identical to current.

### Project-side gates

Two smoothsteps OR'd together via `max`:

```
weightGate = SmoothStep(weightGateLo, weightGateHi, amountInFront)
wettedGate = SmoothStep(wettedGateLo, wettedGateHi, front.amountWetted - back.amountWetted)
combinedGate = max(weightGate, wettedGate)
effectiveExtra = AngularDampingYNoseDownExtra * combinedGate
```

Defaults:
- `weightGateLo = 0.5`, `weightGateHi = 1.0` — fully closed at centered weight, fully open at all-weight-forward.
- `wettedGateLo = 0.0`, `wettedGateHi = 0.2` — fully closed when nose ≤ tail submersion, fully open when nose is ~20 % more wetted than tail. (The two amountWetted values are already smoothstep-saturated by the wetted-transition distance, so a difference of 0.2 is a clear nose-deeper signal; pick conservatively, retune from there.)

`max` rather than `weightGate + wettedGate` because both gates saturate at 1.0; adding could exceed the budget. `max` rather than `1 - (1-w)(1-d)` because the inputs are already smoothstep-saturated, so the discontinuous derivative at `w = d` is invisible in practice.

Pushed to the engine CVar at the same spot `SurfboardUtils::Tick` already pushes `AngularDampingY`:

```cpp
const float weightGate  = FMath::SmoothStep(0.5f, 1.0f,
    this->weightDistribution ? this->weightDistribution->amountInFront : 0.5f);
const float wettedDiff  = (this->sharedCalculationsFront ? this->sharedCalculationsFront->amountWetted : 0.5f)
                        - (this->sharedCalculationsBack  ? this->sharedCalculationsBack->amountWetted  : 0.5f);
const float wettedGate  = FMath::SmoothStep(0.0f, 0.2f, wettedDiff);
const float combinedGate = FMath::Max(weightGate, wettedGate);
const float effective    = this->AngularDampingYNoseDownExtra * combinedGate;
this->AngularDampingYNoseDownExtraCVar->Set(effective);
```

### Truth table

| Scenario | amountInFront | nose vs tail wetted | rotating nose-down? | Damping engaged? |
|---|---|---|---|---|
| Cruising, balanced | 0.5 | nose ≈ tail | maybe | **No** (both gates 0) |
| Bottom turn carve | 0.5 | nose ≈ tail | yes | **No** (gates 0) |
| Pop-up, weight back | 0.3 | tail deeper | no (nose-up) | **No** (sign gate blocks) |
| Wave slope rising, weight forward (gravity wins, board rotating down) | 1.0 | tail deeper | yes | **YES** (weight gate open) |
| Recovery after overshoot, centered weight | 0.5 | nose ≈ tail | yes (settling) | **No** (both gates 0) |
| Cresting a wave with centered weight, nose briefly dipped | 0.5 | nose deeper | yes | **YES** (wetted gate open) |
| **Hang-ten dive (starting, weight just shifted)** | 1.0 | nose ≈ tail (not yet) | yes | **YES** (weight gate open — engages early) |
| **Hang-ten dive (deep)** | 1.0 | **nose deeper** | **yes** | **YES** (both gates open) |
| Hang-ten recovery (nose-up rotation) | 1.0 | nose still deeper | no (positive W.Y) | **No** (sign gate blocks) |
| Centered weight on falling wave, following slope nose-down | 0.5 | nose deeper | yes | **YES** ⚠ (over-fires, see open question 4) |

The hang-ten rows are the whole point: the damping fires early as soon as the weight goes forward (without waiting for the wetted asymmetry), and is off during the natural recovery. The "weight forward on rising wave" row is the case AND-logic would miss — gravity-induced dive on a rising face — and OR-logic catches it.

### New UPROPERTY

```cpp
// SurfboardUtils.h
/** Extra angular damping on the pitch axis (body-local Y), applied only when:
 *    1) The rotation is in the nose-down direction (engine handles the sign check), AND
 *    2) EITHER the rider has committed weight forward (amountInFront → 1)
 *       OR the nose is currently deeper than the tail (front.amountWetted > back.amountWetted).
 *  Default 0 = no behaviour change. Tune in BP per surfboard.
 *  See specs/weight-gated-pitch-damping.md. */
UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Damping")
float AngularDampingYNoseDownExtra = 0.0f;
```

Gate thresholds (`weightGateLo/Hi`, `wettedGateLo/Hi`) start as compile-time constants in `SurfboardUtils.cpp`. Promote to UPROPERTYs only if playthrough tuning shows they actually need to vary; YAGNI otherwise.

## Implementation Sketch

### Engine fork

```cpp
// PBDRigidsEvolutionGBF.cpp, near existing AngularDamping CVars
FRealSingle AngularDampingYNoseDownExtra = 0.0f;
FAutoConsoleVariableRef CVarAngularDampingYNoseDownExtra(
    TEXT("p.Chaos.Solver.AngularDampingYNoseDownExtra"),
    AngularDampingYNoseDownExtra,
    TEXT("Extra angular damping on the negative side of local-Y angular velocity. "
         "Project gates this by weight-forward × nose-deeper-than-tail."));

// In the angular damping block (~line 1057):
const float yExtraNoseDown = (angularVelocityLocalSpace.Y < 0.0)
    ? CVars::AngularDampingYNoseDownExtra
    : 0.0f;
angularVelocityLocalSpace.X *= 1 - CVars::AngularDampingX;
angularVelocityLocalSpace.Y *= 1 - (CVars::AngularDampingY + yExtraNoseDown);
angularVelocityLocalSpace.Z *= 1 - CVars::AngularDampingZ;
```

### Project

```cpp
// SurfboardUtils.h, in protected: section alongside the other CVar handles
IConsoleVariable *AngularDampingYNoseDownExtraCVar =
    IConsoleManager::Get().FindConsoleVariable(TEXT("p.Chaos.Solver.AngularDampingYNoseDownExtra"));
```

```cpp
// SurfboardUtils.cpp Tick, alongside the existing AngularDampingY push
const float weightGate = FMath::SmoothStep(0.5f, 1.0f,
    this->weightDistribution ? this->weightDistribution->amountInFront : 0.5f);
const float frontW = this->sharedCalculationsFront ? this->sharedCalculationsFront->amountWetted : 0.5f;
const float backW  = this->sharedCalculationsBack  ? this->sharedCalculationsBack->amountWetted  : 0.5f;
const float wettedGate = FMath::SmoothStep(0.0f, 0.2f, frontW - backW);
const float combinedGate = FMath::Max(weightGate, wettedGate);
const float effectiveExtra = this->AngularDampingYNoseDownExtra * combinedGate;
if (this->AngularDampingYNoseDownExtraCVar)
{
    this->AngularDampingYNoseDownExtraCVar->Set(effectiveExtra);
}
```

### Debug log

Add to the existing `damping`-flag block in `SurfboardUtils::Tick`:

```cpp
UE_LOG(LogTemp, Warning,
    TEXT("PitchDamping: amountInFront=%.3f weightGate=%.3f frontW=%.3f backW=%.3f wettedDiff=%.3f wettedGate=%.3f combinedGate=%.3f -> effectiveExtra=%.4f"),
    amountInFront, weightGate, frontW, backW, frontW - backW, wettedGate, combinedGate, effectiveExtra);
```

## Acceptance Criteria

### AC1 — `AngularDampingYNoseDownExtra = 0` is a no-op

With the UPROPERTY at default 0, the value pushed to the engine CVar is 0, the integrator's `yExtraNoseDown` is 0, and behaviour is byte-identical to current. All four baselined autopilots pass snapshot tolerance unchanged.

### AC2 — Hang-ten visual pitch holds

With `AngularDampingYNoseDownExtra` tuned, the hang-ten autopilot's visual pitch (CSV `pitch` column — confirmed visual pitch on 2026-06-02) stays better than -15° throughout step 5. Default baseline is -23.6°; target is ≤ -15° while keeping other autopilots within tolerance.

### AC3 — Wave-slope tracking preserved during normal surfing

With the same value that satisfies AC2, the `surfing-down-the-line` autopilot's per-step pitch divergence stays within snapshot tolerance of its committed baseline. The wetted gate's job is to keep this AC achievable — if it isn't, the wetted-gate thresholds need re-tuning.

### AC4 — Manual playthrough validates the qualitative behaviour

In editor:
- With weight centered on a gentle wave, the board pitches freely with wave slope and during pop-up. Identical to current.
- With weight forward on a rising wave face, the board follows the wave up (nose-up rotation isn't damped — sign gate blocks). When gravity tries to pull the nose down faster than the wave rises, the descent is braked.
- With weight forward on a flattening wave, the dive is braked as soon as the weight shifts (no need to wait for the nose to actually dip — weight gate alone opens the damping).
- When the rider walks back to center while the nose is still deep, the damping stays engaged via the wetted gate until the board levels out, then disengages crisply.

### AC5 — Damping engages and disengages crisply with weight shift

Toggle `amountInFront` between 0.5 and 1.0 mid-cruise while watching the debug log. `combinedGate` should respond within a few ticks; `effectiveExtra` should drop to 0 within ~50 ms of weight returning to center (assuming the wetted gate has also closed by then — if the board is still in a nose-deeper attitude, damping legitimately persists).

## Open Questions / Future Work

1. **Tail-walk gate** mirroring on the [0, 0.5] range of `amountInFront` plus a "tail deeper than nose" wetted gate. Brakes nose-UP rotation when the rider walks back. Defer until v1 lands and tail-walk failure mode is characterised.
2. **Over-firing in centered-weight-on-falling-wave case** (last row of the truth table). When the rider rides a falling wave face with weight centered, the nose is naturally deeper than the tail and the board is rotating nose-down to stay aligned with the slope. OR-logic fires damping here, which would impede wave-following. Mitigation: in real surfing the rider would shift weight back to control the descent, breaking this exact scenario. If playthrough shows it actually hurts, options are: (a) require both signals (back to AND), (b) gate the wetted gate further by some "is the wave slope actually inducing this" signal, (c) clamp the wetted gate to a smaller maximum so even a maximally-asymmetric attitude only weakly engages damping.
3. **`wettedGateHi` value**: 0.2 is the starting guess. Tighter (0.1) → engages earlier in the dive (good prevention) but might also fire briefly during normal turning transitions where front/back wetting shifts. Looser (0.4) → later engagement, less spurious. Re-tune from playthrough.
4. **Coupling with `AngularDampingZ`'s tilt-back modulation.** Yaw damping is already modulated by `amountTiltingBack × avgAmountPlaning`. The pattern of "damping responds to commanded weight shift" is the same idea applied to a different axis. Worth thinking about whether the two should share helpers or remain independent. Probably independent — different physical mechanisms.
5. **The conceptually right signal is "pitch relative to the wave surface".** A visual-pitch (world-frame) gate would be wrong: on a steep wave face the board can have a nose-down world orientation while still riding flush against the wave (tail submerged, nose lifted above the local water surface). The wetted-asymmetry gate `(front.amountWetted - back.amountWetted)` is in effect a proxy for *board-relative-to-water-surface* pitch — it measures whether the bottom is more wetted at one end than the other, which is exactly what the rider feels as "the nose is dropping into the wave" regardless of world orientation. A direct geometric measurement (angle between `board.up` and the local wave-surface normal) could replace the proxy if precision becomes important, but the proxy reads the same physical signal with less compute.
6. **Interaction with `upwardsThrustPitchSensitivity`.** Both this spec and pitch-sensitivity are pitch-dive mitigations operating through different mechanisms (damping vs lift coefficient curve). Together they could either compound usefully or interfere. Tune them in order: this spec first (pure damping, no force changes), pitch-sensitivity second (changes the lift response and may need re-tuning of other coefficients).

## Status

- [ ] Engine fork: `AngularDampingYNoseDownExtra` CVar + sign-conditional application in integrator
- [ ] Project: `AngularDampingYNoseDownExtra` UPROPERTY on `ASurfboardUtils`
- [ ] Project: gated value computed and pushed each tick (max(weightGate, wettedGate))
- [ ] Project: debug log emits all gate inputs + combined gate + final extra value
- [ ] AC1 verified (default 0 → byte-identical, baselines pass)
- [ ] `AngularDampingYNoseDownExtra` tuned in BP
- [ ] AC2 verified (hang-ten visual pitch ≤ -15°)
- [ ] AC3 verified (`surfing-down-the-line` within tolerance)
- [ ] AC4 verified (editor playthrough)
- [ ] AC5 verified (gate response time)

## Related

- `amount-wetted-split.md` — provides the `amountWetted` signal this spec uses as the wetted-asymmetry gate input. Kept committed.
- `nose-dive-bug.md` — the original hang-ten symptom report. Update to mark this spec as the chosen fix once AC2/AC4 verify.
- `bottom-hydrofoil-upthrust-decoupling.md` — earlier "decouple lift from non-lift factor" precedent. Same shape of "the lever should only fire in the relevant regime".
- Reverted attempts: commits `e9d594185` / `4f470e895` (project) and `7c74987916ce` / `8be56273427f` (engine fork). Reverted on 2026-06-02 in favour of the design in this spec.
