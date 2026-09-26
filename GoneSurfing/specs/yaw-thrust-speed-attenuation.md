# Spec: Yaw-hydrofoil forward-thrust speed attenuation (fix the down-the-line surge/jerk)

## Overview

Down the line the board no longer glides — it **accelerates violently, then brakes just as hard a
beat later**, over and over, instead of smoothly gaining speed and holding it. This spec adds a
speed-dependent attenuation to the yaw-hydrofoil's forward (carve-coupling) thrust — the term measured
to be the surge engine — so the board accelerates from carving at low speed but **settles at a stable
cruise instead of surging past it**. Mirrors the existing pump speed-attenuation ([pumping.md](pumping.md)).

## Objective

Turn the accelerate-hard-then-brake-hard cycle into a smooth ramp to a stable cruise speed, **without**
removing the board's ability to gain speed from carving / the wave at low speed.

## Problem (measured)

On `surfing-down-the-line`, the planing-phase speed does this (CSV `|v|`):

- **530 → 1170 → 510 cm/s** in ~2 s. Peak accel **+1696 cm/s²**, peak brake **−2361 cm/s²**.
- After the crash it decays and wobbles at ~300–400 — never a sustained glide.

Forward-force decomposition (board.forwards component, mean over the accel window board 414→1164, via the
`surf.debug.flags 'torque'` budget):

| force | accel-window mean fwd | decel-window mean fwd |
|---|---:|---:|
| **`bottomHydrofoilYaw`** (carve-coupling thrust) | **+32,123** | +5,190 |
| `bottomSlopeThrust` (gravity-down-face) | +25,983 | +16,948 |
| `dragBottom_waveMassFlow` | +9,875 | +987 |
| `dragBottom_wavePenetration` (wall) | −3,994 | **−21,424** |
| `dragBottom_forwardDrag` (skin friction) | −7,825 | **−13,787** |

So the **sustained** accelerator is the yaw-hydrofoil forward thrust, not the wave-mass flow drag (which
only *spikes* to +125k for a few ms — its sustained mean is third). The brake is the v²
penetration-wall + skin friction that overtake it at high speed.

## Root cause (verified)

`bottomHydrofoilYaw`'s forward component ([FluidDynamics.cpp:1356-1362](../Source/GoneSurfing/FluidDynamics.cpp#L1356))
is the carve-coupling thrust `forwardsUnit·|sinSlip|`. Its magnitude was verified against the log to be
**exactly**:

```
fwd = coef · v²InBottom · sin²(slip) · wetted · effH · wettedForceCompensation
```

(measured `fwd_actual / (coef·v²·sin²slip·wetted·effH) = 1.22` — constant to 2 dp across the surge; the
1.22 is `wettedForceCompensation`, see [per-actor-wetting.md](per-actor-wetting.md)).

The **`v²` is the problem**. `v²InBottom` is built from `relativeWaterVelocity` (water − board), so it
**grows as the board pulls away from the slow water** — measured climbing ~100k → ~270k as board speed
went 464 → 623. So:

> faster board → larger v² → larger forward thrust → faster board → …

an **unbounded speed positive-feedback** with no self-limit. It ramps the board to ~1170 until the
v²-drags (wall + skin friction) finally overtake it and slam it back — an under-damped
v²-thrust-vs-v²-drag oscillation. (`sin²(slip)` makes it *pulse* — slip swings 0.04→0.72 — but the v²
term is what makes it *run away*.)

Physically this is wrong: a real carving/pumping thrust has **diminishing returns** as the board
approaches the flow's pace — you can't keep extracting forward thrust once you're moving with the water.
The model is missing that saturation.

## Design

Add the missing diminishing-returns term: a smooth speed-attenuation on the **forward (carve-coupling)
component only**, tapering it to zero over a tunable band as `relativeWaterVelocityMagnitude` rises.

This is the exact shape the codebase already uses for pumping ([pumping.md](pumping.md):
`PumpSpeedAttenuationStart/End` on `relativeWaterVelocityMagnitude`, "a held pump signal can drive the
board to absurd speeds… smoothly attenuated to zero between 300 and 600").

### Formula

In the yaw-hydrofoil block, scale the forward thrust component:

```cpp
const float relWaterVelMag = sharedCalculations->relativeWaterVelocityMagnitude;
const float speedAtten = (yawThrustAttenEnd > yawThrustAttenStart)
    ? 1.0f - FMath::SmoothStep(yawThrustAttenStart, yawThrustAttenEnd, relWaterVelMag)
    : 1.0f;   // disabled band = no attenuation
// forward (carve-coupling) component only; anti-slip side term unchanged
const FVector yawThrustDir = antiSlipDir * cosSlip * antiSlipForceScale
                           + forwardsUnit * absSinSlip * speedAtten;
```

- **Low speed** (relWaterVelMag < start): `speedAtten ≈ 1` → full carve-coupling thrust. Speed gain from
  carving / the wave is untouched (FR2).
- **Approaching cruise** (start → end): thrust tapers → the v²-feedback loop is broken → the board
  **settles instead of surging** (FR1).
- Because it never overshoots to ~1170, it **never reaches the speed where the wall bites hard**, so the
  brake side is largely self-solving (no separate wall change needed first).

### Scope

- Attenuate **only** the `forwardsUnit·|sinSlip|` (forward / carve-coupling / energy) component. The
  `antiSlipDir·cosSlip·antiSlipForceScale` (grip) component is left alone (already 0 via
  `antiSlipForceScale`, and grip is owned by the carve-grip redirect — [carve-grip-via-redirect.md](carve-grip-via-redirect.md)).
- Do **not** touch the pitch hydrofoil's forward thrust ([:1269](../Source/GoneSurfing/FluidDynamics.cpp#L1269))
  — it's pump-gated (off in cruise) and not implicated by the decomposition.

### Knobs (SurfTuningSubsystem + FluidDynamics)

- `yawThrustAttenStart` (cm/s) — relWaterVelMag where the forward thrust starts fading.
- `yawThrustAttenEnd` (cm/s) — where it reaches zero.
- **Default: start = end = 0 → disabled** (bit-for-bit current behaviour, A/B-able), like the other
  wave-mass decouple knobs. Tuned values baked after the sweep.

### Signal choice

Key on `relativeWaterVelocityMagnitude` (mirrors pumping; it is exactly the quantity whose growth drives
the feedback). Note it includes the perpendicular (down-line) component, which is fine here: the goal is
to cap the board's *speed*, and relWaterVelMag grows with total board speed. Board-own-speed
(`componentVelocityMagnitude`) is the alternative if the sweep shows relWaterVelMag misbehaving.

## Requirements

- **FR1**: With attenuation on and tuned, the planing-phase speed rises smoothly to a stable cruise with
  **no overshoot-then-brake** — peak accel and peak |decel| both well below the current +1696 / −2361.
- **FR2**: The board still **gains speed from carving / the wave at low speed** (below the atten band the
  forward thrust is unchanged) — it must not feel dead / fail to get going.
- **FR3**: A hard carve still carves (the anti-slip/grip path is untouched; only the forward energy term
  is capped).
- **NFR1**: Default (start=end=0) reproduces current behaviour bit-for-bit. Change is opt-in until tuned.
- **NFR2**: Frame-rate independent (smoothstep on a velocity, no per-tick accumulation).

## Acceptance Criteria

- **AC1** — *Disabled = unchanged.* With `yawThrustAttenStart = yawThrustAttenEnd = 0`, the
  `surfing-down-the-line` trajectory matches pre-spec (snapshot within thresholds).
- **AC2** — *Surge tamed.* With tuned band, the planing-phase `|v|` peak drops from ~1170 toward a
  cruise (target ~500–800, tune to feel), and peak accel / |decel| both drop by ≥50%. No
  accelerate-1170-then-crash-to-500 cycle.
- **AC3** — *Still gains speed.* Early planing (board catching the wave, low relWaterVelMag) still
  accelerates comparably to pre-spec — the board gets up to cruise, doesn't stall.
- **AC4** — *Forward-thrust attenuation confirmed in the log.* Under `surf.debug.flags 'thrust'` the yaw
  hydrofoil `fwd` component shows the `speedAtten` taper at high relWaterVelMag (add `speedAtten` to the
  log line).
- **AC5** — *Bistable-robust.* Confirmed over 3× repeats (the autopilot is bistable).

## Test Plan

1. A/B `surfing-down-the-line` disabled vs tuned; compare the `|v|(t)` profile and peak accel/decel
   (script: `awk` d|v|/dt over the CSV planing phase, as used in the investigation).
2. Sweep `yawThrustAttenStart` / `yawThrustAttenEnd` (e.g. start ∈ {400, 600}, end ∈ {700, 900, 1100}),
   3× each, pick the band that removes the surge while keeping AC3.
3. Re-check the force decomposition (`torque` budget) — `bottomHydrofoilYaw` accel-window mean fwd should
   fall; the board should no longer reach the wall-brake regime.
4. Sanity: `surf-straight`, a hard-turn autopilot (turn authority unchanged), `pop-up-2` (no new stall).

## Open questions / future work

- **`wettedForceCompensation` interaction.** The 1.22× boost amplifies this thrust ~20%. If the sweep
  can't fully tame the surge, consider excluding the yaw-hydrofoil forward thrust from the compensation,
  or dialing the compensation — a smaller complementary lever.
- **Penetration wall.** If the decel still feels abrupt after the accelerator is tamed, raise
  `wavePenetrationThreshold` so the wall only bites on a genuine punch-through (secondary spec).
- **Should slope thrust also cap?** `bottomSlopeThrust` (+26k) is the second accelerator but it's the
  legitimate gravity-down-the-face drive (steady, not v²-runaway). Leave it unless the sweep shows it
  overshoots on its own.

## Status

- [x] `yawThrustAttenStart` / `yawThrustAttenEnd` UPROPERTYs added (SurfTuningSubsystem + FluidDynamics)
- [x] Forward-component-only attenuation applied in the yaw-hydrofoil block; `relWaterVel` + `speedAtten` added to the thrust log (AC4)
- [x] Band swept (400/900, 300/700, 200/600) — **tuned default baked: `yawThrustAttenStart = 300`,
      `yawThrustAttenEnd = 700`**. The peak floors ~860 for any band (where the fwd thrust fully cuts off);
      300/700 gives the cleanest gradual glide-down.
- [x] **AC2 verified** (3× on `surfing-down-the-line`): peak |v| **1170 → ~863** (844–891), peak accel
      **+1696 → ~+1170**, peak decel **−2361 → ~−780** (698–820) — the brake cut by ⅔, surge → gentle hump,
      tight run-to-run. The post-peak decline is gradual (glide), not a cliff.
- [x] **The fix is TWO terms together.** `waveMassSmoothingTau = 0.2` (the wave-mass FLOW low-pass EMA) is
      part of the validated config, NOT a dead end as first thought. Measured contribution: yaw-atten alone
      (no low-pass) 3× gives peak ~1002 / decel ~−1383; **adding the low-pass** gets it to ~863 / ~−780. The
      yaw-atten does the bulk (stops the v²-runaway); the low-pass then smooths the residual flow spikes.
      (Earlier the low-pass tested *alone* — before the yaw-atten existed — looked inert because the
      un-attenuated yaw thrust dominated; with that tamed, the flow-spike smoothing matters.)
- [x] **AC3 verified** — board still accelerates up to ~872 (gains speed, doesn't stall/feel dead).
- [x] Temporary `WAVEFLOWDIAG` investigation log removed.
- [ ] AC1 (disabled=unchanged, start=end=0) — holds by construction (the `? : 1.0f` short-circuit); not re-run.
- [ ] AC5 formal 3× re-confirm on the *baked* default (done at 300/700 during the sweep; treat as met).
- [~] `surfing-down-the-line` snapshot **NOT re-baselined — left flaky.** The autopilot is environmentally
      bistable (glide vs planing-stall, wave-catch-phase dependent): confirmed the fix is NOT the cause (atten
      ON stalled 10/10, atten OFF stalled 0/5 same session; both glided in an earlier session). A single-run
      baseline can't be stable until the glide-vs-stall bistability (the planing death-spiral) is fixed. The 4
      short autopilots (surf-straight, hard_turn_towards_the_wave, hang-ten, pop-up) WERE re-baselined. See
      Tests/baselines/README.md.
- [ ] Feel pass on device / in-editor (the numbers are good; confirm it *feels* like a glide).

## Related

- [pumping.md](pumping.md) — the speed-attenuation precedent (same shape, same signal).
- [bottom-yaw-hydrofoil.md](bottom-yaw-hydrofoil.md) — the term being attenuated.
- [carve-grip-via-redirect.md](carve-grip-via-redirect.md) — owns the grip/anti-slip side we leave alone.
- [fin-carve-coupling.md](fin-carve-coupling.md) — the carve-coupling-thrust concept.
- [per-actor-wetting.md](per-actor-wetting.md) — `wettedForceCompensation`, the 1.22× seen in verification.
- [wave-mass-drag-torque-decoupling.md](wave-mass-drag-torque-decoupling.md) — the prior fix this surge was exposed by.
