# Spec: Frame-rate-independent angular damping (engine fork)

> **STATUS (2026-07-17): implemented + validated.** Engine-fork change on branch
> `dt-aware-angular-damping` in `E:\windowsgrejor\git\UnrealEngine`. Spec kept in the game repo by
> convention. Follows directly from [trace-trajectory-comparison.md](trace-trajectory-comparison.md).
> Covers angular **and** linear per-axis damping (a shared `FrDampMul` helper). Acceptance is
> **statistical** (consistent feel across frame rates), not trajectory-match — the surfing loop is chaotic,
> so exact paths can't converge across frame rates, but the behaviour that defines feel does. See Results.

## Overview

Make the custom per-axis Chaos **angular** damping decay a function of **real time**, not of the frame, so
the board behaves the same regardless of frame rate — across a 60 fps flagship, a 30 fps budget phone, and a
frame-rate-jittery session. Today the damping is applied once per frame as `W_local *= (1 - D)`, which makes
the integration frame-rate dependent (and therefore device- and load-dependent).

## Motivation

[trace-trajectory-comparison.md](trace-trajectory-comparison.md) measured this directly: replaying one input
trace, three identical headless PC runs diverged from **each other** by **6.8–22.8 m / up to 180° yaw** under
the default variable timestep, and that collapsed to **cm / a few degrees** with `-usefixedtimestep -fps=60`.
Root cause: the per-axis angular damping removes a **fixed fraction per frame**, so a run (or device) with a
different or jittery frame rate integrates the attitude differently. The phone (variable, per-device frame
rate) diverges from PC for the same reason.

Target is Android, primarily high-perf phones but ideally mid/low-perf too. That rules out the
"fixed-substep" alternative (it adds solver cost exactly when a weak phone is already slow, and degrades back
to variable behaviour once `MaxSubsteps` caps it). A dt-aware decay is **zero added compute** and correct at
any frame rate — the right fit for a wide device range.

## Background — current code

`Engine/Source/Runtime/Experimental/Chaos/Private/Chaos/PBDRigidsEvolutionGBF.cpp`, inside the dynamic-particle
integration loop (`Dt` is the physics step delta, in scope):

```cpp
// ~line 1181, gated by CVars::UseCustomAngularDamping; angularVelocityLocalSpace is the board-local W
angularVelocityLocalSpace.X *= 1 - CVars::AngularDampingX;   // roll  (board-forward axis)
angularVelocityLocalSpace.Y *= 1 - yPitchDamping;            // pitch (base AngularDampingY +/- away/toward extra)
angularVelocityLocalSpace.Z *= 1 - CVars::AngularDampingZ;   // yaw
```

`yPitchDamping = Clamp(AngularDampingY + directional away/toward extra, 0, 1)` (surface-relative pitch damping;
see [surface-relative-pitch-damping.md](surface-relative-pitch-damping.md)). All three are per-frame multipliers.

## Design

### FR1 — dt-aware exponential decay
Replace the per-frame `*= (1 - D)` with a decay keyed to a **reference frame time** `RefDt`:

```
W_local.axis *= pow( clamp(1 - D, 0, 1), Dt / RefDt )
```

Rationale: `(1 - D)` is the fraction of angular velocity **retained per frame at the tuning frame rate**. To
retain the same fraction **per real second** regardless of `Dt`, raise it to `Dt / RefDt`. This is the exact
discrete form of continuous exponential decay `W(t) = W0 · e^(−k·t)`.

- At `Dt == RefDt` the exponent is 1 → **identical to today's behaviour** (backward compatible).
- At `Dt == 2·RefDt` (half the frame rate) the exponent is 2 → `(1 - D)²`, i.e. exactly what two frames at the
  reference rate would have removed. No frame-rate bias.

Applied via a shared helper `FrDampMul(D, Dt)` to **all per-axis damping**:
- **Angular** X/Y/Z (Y uses `yPitchDamping`, preserving the surface-relative pitch behaviour).
- **Linear**: `DampAsymmetricalLinear` (`DampingLocalX/Y/Z`) and the world-vertical `DampingZUp/ZDown`.

The planing-redirect and carve-grip velocity rotations were **audited and are already dt-correct**
(`RotateVelocityTowardDir(..., rate * Dt)`), so they're untouched. `DampSymmetricalLinear` is dead code
(uncalled) and left as-is.

### FR2 — CVars (shared by angular + linear)
- `p.Chaos.Solver.DampingRefDt` (`FRealSingle`, default `1/60 ≈ 0.016667`): the frame time the current damping
  values were tuned at. Exposed so it can be matched to the real tuning rate and A/B'd.
- `p.Chaos.Solver.FramerateIndependentDamping` (`bool`, default `true`): master toggle. `false` restores the
  exact legacy per-frame `*= (1 - D)` for comparison.

### Edge cases
- **Clamp the base** to `[0, 1]` before `pow` (negative base → NaN; `D` should be ≤ 1 but `AngularDampingX/Z`
  aren't hard-clamped at the call site, and `yPitchDamping` already is).
- `RefDt <= 0` → treat as disabled (fall back to legacy) to avoid divide-by-zero.
- `Dt == 0` (paused) → exponent 0 → `pow(base,0) = 1` → no damping. Correct.
- Large `Dt` (a hitch) → large exponent → strong decay that frame. This is the *correct* continuous-decay
  result over that real-time gap; no special clamp needed here. (Bounding the physics `Dt` itself is an
  engine-level concern, out of scope.)

### Performance / device-range rationale
One `pow` per axis per dynamic body per frame (there is effectively one surfboard body). Negligible, and — key
point — **the cost does not grow when the frame rate drops**, unlike fixed substepping. A 60 fps and a 30 fps
phone apply the same effective damping per real second, so the board feels and behaves the same on both.

## Why not trajectory-match

The surfing loop (attitude → hydro forces → attitude) is chaotic, and explicit-Euler integration has O(Dt)
truncation error, so a 30 fps and a 60 fps run seed a tiny difference that amplifies exponentially — exact
trajectories **cannot** converge across frame rates no matter how dt-correct the coefficients are (confirmed:
after this fix, 60 vs 30 fps still diverge to ~100°+ yaw within ~3 s). What matters for players is that the
board **feels and behaves** the same on any device — a statistical property, not a per-frame one.

## Acceptance criteria (statistical)

Replay one trace on PC at `-usefixedtimestep -fps=60` and `-fps=30`; compare aggregate behaviour metrics
(mean/max speed, path length, mean |yaw rate|, planing fraction, mean submersion).

- `FramerateIndependentDamping=true`: 30-vs-60 deviation small (few %) on the feel-defining metrics.
- `FramerateIndependentDamping=false`: legacy per-frame path (large 30-vs-60 deviation).
- `-fps=60` unchanged vs a pre-change baseline (exponent = 1 at `RefDt`).
- Chaos compiles; editor launches; surfing still planes/turns.

## Results (2026-07-17, `phone-2026-07-17-08-39-13.csv`, 30 fps vs 60 fps deviation)

| metric | FIX ON | FIX OFF |
|---|---|---|
| mean speed | 2.3% | 24.3% |
| max speed | 3.3% | 18.2% |
| path length | 3.4% | 26.4% |
| mean turn rate | 4.8% | 50.0% |
| planing fraction | 0.0% | 10.4% |
| mean submersion | 0.0% | 6.8% |

With the fix, speed / distance / turn-rate / planing are frame-rate independent to a few percent; without it a
30 fps device turns half as fast and cruises ~24% slower. Exact endpoint still differs (~10-14%: residual
chaos), as expected. Net: consistent feel across the device performance range, at zero added compute.

## Status

- [x] Spec written
- [x] CVars added (`FramerateIndependentDamping`, `DampingRefDt`)
- [x] `FrDampMul` decay applied to angular (X/Y/Z incl. `yPitchDamping`) and linear (`DampingLocalX/Y/Z`,
      `DampingZUp/ZDown`) damping
- [x] Redirect / carve-grip audited — already dt-correct
- [x] Chaos compiles + editor launches
- [x] Statistical 60-vs-30 fps consistency verified (few-% on the feel metrics)
- [x] Re-calibrated the damping defaults after the fix. On-device the board went too loose (yawed to the far
      side during the high-fps pop-up) because the old values were tuned against per-frame over-damping at the
      phone's ~120-140fps peaks. Bumped angular + linear damping defaults ~2x-effective (`D' = 1-(1-D)^2`) in
      `USurfTuningSubsystem` to restore that feel; now frame-rate independent. Starting point for live tuning.
- [ ] Confirm the re-calibrated feel on-device; fine-tune live in the HUD
- [ ] Confirm on real devices (high vs mid/low phone) once deployed
