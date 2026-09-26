# Spec: Wave-Slope Gravity Supplement — Slope Threshold Gate

## Overview

Add a soft slope-magnitude gate (smoothstep) to `waveSlopeGravityForce` in `AFluidDynamics::calcLiftForce`, on top of the existing `AmountPlaning` gate. Force only fires on slopes meaningfully larger than the residual ripple noise the board sees during cruise.

Scope: single multiplier added to the existing per-bottom-actor supplement formula. No new force shapes, no new UPROPERTYs (smoothstep thresholds hardcoded — they're physical, not tuning, knobs).

## Background

The supplement was retired 2026-05-19 (commit e6bebea57), re-introduced 2026-05-23 (commit 5337561cb) as a `× amountUnderWater` term, then switched to `× AmountPlaning` later the same day after the user observed the board drifting backwards in the trough before the wave arrives (waveSlopeDownVec on the back side of an approaching wave points along +X — into the previous trough). The AmountPlaning gate fixed the backwards-drift.

This spec addresses the next symptom: with the planing gate, the supplement keeps firing at near-full strength (planing ≈ 0.85) on the small residual slopes the board sees after the wave has passed. Because drag is heavily planing-attenuated, the supplement's continued kicks are enough to maintain cruise speed indefinitely — the board never decelerates.

## Motivation

Observed in `surf-straight` CSV at 2026-05-23 19:50, run with the AmountPlaning gate:

| t (s) | x | vx | slopeSin | planing |
|---|---|---|---|---|
| 6.7 | 6848 | -565 | 0.09 | 0.85 |
| 8.0 | 6177 | -498 | 0.06 | 0.85 |
| 9.0 | 5674 | -487 | 0.02 | 0.85 |
| 10.0 | 5227 | -420 | 0.03 | 0.85 |
| 11.0 | 4805 | -426 | 0.04 | 0.85 |
| 12.0 | 4290 | -420 | 0.03 | 0.85 |

5.5 seconds after the burst, board still cruising at ~420 cm/s on slopes of 0.02–0.09. Estimating force per actor at slopeSin=0.04, planing=0.85, coef=45000:

```
F_actor  ≈ 45000 × 0.04 × 0.85 = 1,530 N
F_total  ≈ 15,300 N (× 10 bottom actors)
```

Against planing-attenuated drag, this is enough to hold cruise speed. The supplement is acting as a propulsion source on slopes that shouldn't be propulsive — they're just the geometric noise of being on top of a wave field after the surf wave has rolled past.

Same CSV's burst phase had `slopeSin = 0.20–0.40`, an order of magnitude higher. There is a clean separation between "real wave face" and "residual ripple" in the data; the supplement should only fire on the former.

## Design

Apply a `smoothstep(low, high, slopeSin)` multiplier to the supplement formula:

```cpp
const FVector waveDown = this->sharedCalculations->waveSlopeDownVec;
const float amountPlaning = this->sharedCalculations->AmountPlaning;
const float slopeSin = waveDown.Size();
const float slopeGate = FMath::SmoothStep(0.10f, 0.20f, slopeSin);
if (amountPlaning > 0.0f && slopeGate > 0.0f)
{
    waveSlopeGravityForce = waveSlopeGravityCoefficient * waveDown * amountPlaning * slopeGate;
    ...
}
```

(`FMath::SmoothStep(min, max, x)` returns 0 below min, 1 above max, smooth Hermite ramp between.)

### Why a soft ramp, not a binary threshold

A binary `if (slopeSin < 0.10) zero` creates a discontinuity at the boundary, which becomes a discontinuity in board acceleration when riding a wave whose slope grazes the threshold. The Hermite ramp avoids that — force fades in/out smoothly over a 0.10-wide window.

### Why hardcoded thresholds, not UPROPERTYs

The 0.10 and 0.20 numbers describe the *physical* separation between "real wave face" and "residual ripple", not a tuning surface — they should be the same for any board, any wave, any test. Coefficient strength is the right knob; threshold position is calibration. If a later wave geometry shows the thresholds need adjusting, the change is one constant in one place.

### Threshold values: 0.10 → 0.20

Calibrated to the cruise vs. burst data above:

- Cruise-phase slopeSin observed range: 0.02–0.09 → multiplier = 0 across the board.
- Burst-phase slopeSin range: 0.20–0.42 → multiplier = 1.0 across the board.
- Transition window 0.10–0.20 covers the wave's rising and falling edges, where the force should ramp not snap.

If a future wave geometry has its real-surf slopeSin below 0.20 (small, mellow wave), the high threshold needs lowering — verifiable by checking that the burst-phase `Wave-Slope Gravity` log lines show the expected force magnitudes.

### Combined gate chain

The supplement now has three multiplicative gates:

1. `(slope direction)` — `waveSlopeDownVec` itself, magnitude `sin(slope_angle)`. Carries direction.
2. `AmountPlaning` — only fires while board is actively surfing. Cuts trough-side drift.
3. `smoothstep(0.10, 0.20, slopeSin)` (this spec) — cuts residual-ripple kicks during cruise.

Coefficient (`waveSlopeGravityCoefficient`) multiplies all three. Bottom-side gate (`side == VE_Down`) wraps everything.

## Per-actor force cap (added 2026-05-25)

A `maxSupplementForce` UPROPERTY caps the per-actor supplement force magnitude. Default 5000 N/actor, matching `maxHydrofoilForceAmount`. Set to 0 or negative to disable.

```cpp
if (maxSupplementForce > 0.0f && waveSlopeGravityForce.Size() > maxSupplementForce)
    waveSlopeGravityForce *= (maxSupplementForce / waveSlopeGravityForce.Size());
```

### Why the cap is needed

The supplement is the dominant lever for forward speed — bisection (2026-05-25) showed coefficient changes account for ~75% of the speed delta between "slow surf" and "fast surf"; yaw hydrofoil ~1%, slope-gate ~8%. So tuning forward speed wants a high coefficient (the user's original 85,000 worked).

But: all 10 bottom actors read the SC-shared `waveSlopeDownVec`, so they all push in the same direction. At high coefficients during the wave-catch transition, per-actor force can spike to 60-70 kN (700 kN board-total) for a brief window. When the board's bottom plane is tilted by even a few degrees during a transient, the direction of waveSlopeDownVec has a Z component, and 700 kN × small Z component is enough to overwhelm the vertical/sideways damping and launch the board. This was the "chaos at 7:30" the user reported.

The cap bounds the per-actor force without affecting steady-state cruise (where per-actor force is naturally ~1-4 kN). At 5000 N/actor × 10 actors = 50 kN total cap — enough propulsion for fast surf, not enough to fling the board when tilted.

## Future Work / Fallback: project supplement onto board.forwards

If the cap proves insufficient — i.e. even at capped magnitude the directional leak through tilt still causes chaos — the next step is to **project the supplement force onto `board.forwards`** before applying:

```cpp
const float forwardComponent = FVector::DotProduct(waveSlopeGravityForce, board.forwards);
waveSlopeGravityForce = board.forwards * forwardComponent;
```

This eliminates the Y/Z leak entirely — the supplement can ONLY push along board heading. Trade-offs:
- (+) No more chaos from board tilts. The "speed wall" can be the forward damping where it belongs.
- (-) Loses the "physically correct gravity-along-slope" direction. The supplement no longer pushes the board down the wave face correctly when the board is angled across the slope.
- (-) Reproduces the design of the original retired `waveSlopeSupplementCoefficient` (pre-2026-05-19), which was retired specifically because it caused skid (sideways force relative to heading). The current `waveSlopeDownVec` direction was the fix for that skid. Reverting this is a step backwards in physical correctness.

So this is a fallback for the case where physical-correctness costs more than the chaos costs. Not the first choice — start with the cap, only project if cap alone doesn't suffice.

## Acceptance Criteria

### AC1 — Compiles, no new UPROPERTY

`calcLiftForce` gains the smoothstep multiplier. No header changes; no umap re-touching.

### AC2 — Cruise decelerates

Re-running `surf-straight` with the same `waveSlopeGravityCoefficient` as the 19:50 reference run shows vx decaying after the burst (post-6.7s) instead of holding at ~420 cm/s. Specifically: vx at t=12s should be measurably lower than vx at t=7s.

### AC3 — Burst preserved

Burst peak vx (around t=5.5–6.5s, when slopeSin crosses through 0.30+) is within ±15% of the reference run's peak. The threshold gate is fully open in this range, so behavior should be unchanged.

### AC4 — Trough drift still gated

The pre-wave backwards-drift (t=0–3.5s, slopeSin in 0.03–0.13, planing=0 in the reference) stays gated — the AmountPlaning gate handles this regardless, but this AC confirms the threshold doesn't accidentally re-enable it.

### AC5 — Disable threshold reproduces post-fix-without-threshold behavior

Setting the smoothstep's lower bound to a negative value (or stubbing it to return 1.0) reproduces the indefinite-cruise behavior observed in the 19:50 run.

## Open Questions / Future Work

1. **Per-board thresholds.** If multiple board types end up needing different thresholds (e.g. a longboard with broader planing range), promote the thresholds to UPROPERTYs on `ASharedCalculations` or `ASurfboardUtils`. Defer until a second board exists.
2. **Tail-end taper.** The supplement currently snaps off the moment the board exits planing (after `PlaningDecayTime ≈ 2 s`). A smoothstep on planing could make that gentler too, but planing's own decay already provides a soft fade — measure first.
3. **Direction-aware version.** A `max(0, waveDown · board.forwards)` projection would zero the supplement whenever it would pull the board sideways or backwards. More physically motivated than slope-magnitude thresholding but reintroduces the heading-coupling the 2026-05-23 re-design specifically chose to leave out. Revisit only if the current shape produces visible sideways skid.

## Status

- [ ] Smoothstep multiplier added to `waveSlopeGravityForce` in `calcLiftForce`
- [ ] Log line updated to include slope gate factor (so debug output makes the gate visible)
- [ ] AC2 verified (cruise decelerates)
- [ ] AC3 verified (burst peak preserved within ±15%)
- [ ] AC4 verified (trough drift still gated)
- [ ] AC5 verified (disable reproduces indefinite-cruise)
- [ ] `surf-straight` baseline re-approved if trajectory shifts beyond Compare.ps1 thresholds
