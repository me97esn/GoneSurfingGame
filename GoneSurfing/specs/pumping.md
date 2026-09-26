# Spec: Pumping (Accelerate by Pushing the Phone Down)

## Overview

Let the player gain forward speed by **physically gesturing the phone up-and-down**. The accelerometer detects the loading phase of each gesture and produces a transient downward impulse on the surfboard mesh; the existing bottom-hydrofoil physics converts the resulting downward board motion into forward thrust as a byproduct of the `sinAOA × v²` term in [calcThrustForce](../Source/GoneSurfing/FluidDynamics.cpp). No new force model — pumping rides entirely on the existing hydrodynamics.

Replaces the stale pre-refactor design that targeted the deprecated `alongThrustAmount` formula.

## Motivation

- Real surfers regain speed between waves by pumping (compressing and extending their legs).
- Today the board only decelerates between wave-thrust events — there's no rider-side energy injection.
- Phone-controlled tilting freed up touchscreen real estate; the accelerometer can detect a real-world pump gesture with no extra UI.

## Physics Model (Approach C — downward impulse)

A pump applies a transient downward impulse to the surfboard mesh at its center of mass each tick the player is in the loading phase. **The existing physics does the rest:**

- Downward impulse → board's vertical velocity drops → relative water velocity tilts upward.
- `flowDir` (in [FluidDynamics.cpp:1029](../Source/GoneSurfing/FluidDynamics.cpp#L1029)) tilts toward `board.up`.
- `sinAOA = dot(flowDir, board.up)` increases.
- Bottom hydrofoil thrust ∝ `v² × sinAOA × amountWetted × effH × forwardsThrustCoefficient` increases.
- Net: forward velocity rises during the loading phase, falls back during recovery — but the asymmetry of "loading-only" input means the cycle averages to positive forward acceleration.

**Energy is conserved** inside the simulation: the rider's leg muscles inject energy via the downward impulse, and the hydrodynamic redirect converts it. No free thrust, no synthesized energy.

> **Not true as built, measured 2026-09-12.** The forward term the pump gate opens is `v² × sinAOA × effH × forwardsThrustCoefficient(4)`, and on every pump it ran 5–300× over the per-actor `maxHydrofoilForceAmount` (20 000). So every bottom actor pinned at the cap and the launch was cap × actor count ≈ 2 g regardless of how hard the pump was — the forward thrust was bounded by the cap, not by the pump's work. See "Stationary-pirouette exploit" below for the fix.

### Bottom-hydrofoil turn-gate dependency

The bottom hydrofoil's forward-thrust components (`boardFwdForce` and `actorFwdForce` in [FluidDynamics.cpp:1119-1120](../Source/GoneSurfing/FluidDynamics.cpp#L1119-L1120)) are gated by `turnGate`, originally `smoothstep(0.05, 0.20, lateralShift)`. The gate's purpose is to prevent the bottom hydrofoil from producing dominant cruise propulsion in straight-line surfing — it should only fire when the surfer explicitly commands forward thrust.

That commands set is now `{ carve, pump }`. The gate is `max(intentGate, pumpGate)` where `pumpGate = smoothstep(0, 5000, MaxPumpForce × PumpInput)`. With this:

- Cruise / wave-catch: neither lateral shift nor pump force → gate closed → no forward thrust → board decelerates naturally.
- Carving (without pumping): `intentGate` opens → forward thrust → speed preserved through the carve.
- Pumping straight (without carving): `pumpGate` opens proportionally to `MaxPumpForce × PumpInput` → forward thrust → board accelerates from pumping.
- Both: gate fully open.

The gate must depend on actual force (not just the input signal) — otherwise setting `MaxPumpForce = 0` would still open the gate and let cruise-killing forward thrust through, breaking the rider's intuition that "no pump force = no pump effect". Lowering `MaxPumpForce` proportionally tapers both the impulse magnitude and the gate's openness in lockstep.

**Without this combined gate, pumping straight does nothing** — the downward impulse boosts `sinAOA` but only the `upForce` component (not gated) fires, pushing the board vertically without forward acceleration. Phase 0 worked because the surfing-straight autopilot has weight-shift steps that opened the lateral gate during the run.

### Why apply behind COM (resolved by Phase 0)
Phase 0 (2026-06-08) tested at-COM, +20 cm (ahead), and −20 cm (behind) variants against a baseline. The behind-COM variant outperformed all others by a wide margin — see "Phase 0 Results" below. The downward torque from the back-applied force keeps the board's nose-up rotation, which sustains `sinAOA` on the bottom hydrofoil, which keeps the board planing, which keeps forward damping low. The mechanism is self-reinforcing in a way the at-COM variant is not. Default `PumpForwardOffset = -20`.

### Why slopeSin attenuation
A surfer never pumps on a steep wave face — the wave does its own work via slope gravity + the bottom hydrofoil's wave-tilt-induced AOA. Attenuating pump by `boardWideSlopeSin` keeps pumping useful on flat sections and prevents "pump-spam on every face" exploits.

### Why speed attenuation
Pumping in real surfing has natural diminishing returns: drag scales with v² **through the water**, and the rider's legs can't move water fast enough relative to the board to keep adding meaningful acceleration. Without a model of this, a held pump signal can drive the board to absurd speeds before the engine's quadratic velocity-damping wall catches up. `PumpInput` is smoothly attenuated to zero between `PumpSpeedAttenuationStart` (300 cm/s default) and `PumpSpeedAttenuationEnd` (600 cm/s default), keyed on `sharedCalculations->relativeWaterVelocityMagnitude` (not the board's world-frame speed — a board carried by fast-moving wave water still has headroom to pump). Applied **in place on `PumpInput`** in `AWeightDistribution::Tick` so both the pump impulse AND the bottom-hydrofoil's `pumpGate` (which reads `PumpInput` via `sharedCalculations->weightDistribution`) see the same tapered value.

```
slopeAttenuation = 1 - smoothstep(PumpSlopeAttenuationStart, PumpSlopeAttenuationEnd, boardWideSlopeSin)
                  // 1.0 on flat water, → 0.0 as the wave gets steep
```

### Stationary-pirouette exploit (2026-09-12) and the three fixes

PC trace `phone-2026-09-12-20-22-04.csv`: with the board leaned fully onto a rail and a pump every ~2 s, the board sat in place for 30 s — 96 m of path, 8 m of net travel. One cycle: speed 120 → 520 cm/s in 0.3 s (≈1.4 g) on the pump release, then the full-rail carve (~120 °/s at 120 cm/s) swung the heading ~180° and bled the speed back to 120 in about a second, ready for the next pump. Launches in alternating directions cancel; the trick scorer logged one `sweep=3108deg`.

Three things stacked, each fixed by a tunable:

1. **The gate was a switch.** `pumpGate = smoothstep(0, 5000, MaxPumpForce × PumpInput)` was tuned when `MaxPumpForce` was 20 000; it was later raised 10× to 200 000 without rescaling the gate, so the gate was fully open at `PumpInput = 0.025`. Now `smoothstep(0, MaxPumpForce, proxy)` — the gate *is* the pump input (still a force proxy so `MaxPumpForce = 0` closes it).
2. **The gated forward thrust was pinned at the general cap** (see the note under "Energy is conserved"). `PumpForwardForceCap` (6 000 per bottom actor, × gate) bounds the forward pair so a half pump is a half launch and a full pump is ~0.6 g across ~10 bottom actors instead of ~2 g.
3. **The speed taper handed a stalled board the strongest pump.** `PumpSpeedAttenuationStart/End` (300/600) only fades the pump *out*; below 300 it was full. `PumpSpeedRampInStart/End` (100/250) fades it *in*, keyed on the same relative water velocity, so a board the carve has stalled at ~120 cm/s gets ~5 % of a pump and the loop cannot restart. Below ~100 cm/s the rider has to wait for the wave — which is what a real pump feels like.

The carve's ~120 °/s yaw rate at walking speed is the other half of the pirouette and was left alone; re-check whether it still matters with the pump fixed.

## Phase 0 Results (2026-06-08)

Four-trial verification run with `MaxPumpForce=50000`, surfing-straight autopilot, single run per trial. CSVs at [Tests/phase0/](../Tests/phase0/).

| Trial | Setup | Peak speed | End speed | Forward vy end | Δy covered | End planing |
|---|---|---|---|---|---|---|
| A baseline | no pump | 559 cm/s | 99 cm/s | 63 | 581 cm | 0.127 |
| B at-COM | offset=0 | 598 (+7%) | 73 (−26%) | 52 | 669 | 0.028 |
| C ahead | offset=+20 | 602 (+8%) | 136 (+37%) | 65 | 436 | 0.000 |
| D behind | offset=−20 | **730 (+31%)** | **305 (+208%)** | **142** | **1281 (2.2×)** | **0.771** |

**Mechanism confirmed.** Approach C works — a downward impulse on the surfboard mesh produces forward thrust via the bottom hydrofoil's `sinAOA × v²` term as predicted. Behind-COM is strongly self-reinforcing because the nose-up rotation sustains AOA → sustains planing → keeps damping low. At-COM is the cleanest test of the velocity-induced-AOA mechanism but the constant-force equilibrium kills planing; a pulsed input shouldn't have this problem.

**Caveats:**
- Single run per trial; [snapshot variance](../../../C:/Users/esand/.claude/projects/e--windowsgrejor-git-GoneSurfingUE5/memory/reference_snapshot_run_variance.md) is large. D's gain is far above noise floor; B's marginal +7% may not be.
- Trajectories diverge — faster trials hit autopilot state transitions earlier and end up at different points in the level. Late-game comparison isn't strictly apples-to-apples.
- Pitch readout is wave-slope-dominated in the late game; D's −16° final pitch is mostly the wave face, not the pump torque.

## Phase 0 — Physics Verification (before wiring controls)

Validate the downward-impulse → forward-thrust mechanism with hard-coded test values **before** building the input pipeline. If the mechanism doesn't add forward velocity in a controlled setup, the controls work is wasted.

### Verification hook

Two new fields on [USurfTuningSubsystem](../Source/GoneSurfing/SurfTuningSubsystem.h) under category `Tuning|Pump`:

```cpp
UPROPERTY(...) float TestPumpInput = 0.0f;            // 0..1; non-zero overrides player input
UPROPERTY(...) float PumpForwardOffset = 0.0f;        // cm along board.forwards from COM; default 0
UPROPERTY(...) float MaxPumpForce = /* see AC1 */;    // N; tuned during Phase 0
```

And ~15 lines in `AWeightDistribution::Tick` (the initial Phase 0 form — the live code adds `PumpLateralOffset` application, the `PumpSpeedAttenuation*` taper, and the `pumpGate` feed described above):

```cpp
const float pumpInput = (Tuning && Tuning->TestPumpInput > 0.0f)
    ? Tuning->TestPumpInput
    : Pawn ? Pawn->PumpInput : 0.0f;                  // Phase 1 hook

if (pumpInput > 0.0f && Surfboard && sharedCalculations)
{
    const float slopeAtt = 1.0f - FMath::SmoothStep(
        Tuning->PumpSlopeAttenuationStart,
        Tuning->PumpSlopeAttenuationEnd,
        sharedCalculations->boardWideSlopeSin);
    const float forceMag = Tuning->MaxPumpForce * pumpInput * slopeAtt;
    if (forceMag > 0.0f)
    {
        UPrimitiveComponent* mesh = Cast<UPrimitiveComponent>(Surfboard->GetRootComponent());
        if (mesh && mesh->IsSimulatingPhysics())
        {
            FVector applicationPoint = mesh->GetCenterOfMass();
            if (!FMath::IsNearlyZero(Tuning->PumpForwardOffset))
            {
                const FVector boardFwd = Surfboard->GetTransform().TransformVector(FVector(0, 1, 0)).GetSafeNormal();
                applicationPoint += boardFwd * Tuning->PumpForwardOffset;
            }
            const FVector downImpulse = FVector(0, 0, -forceMag * DeltaTime);
            mesh->AddImpulseAtLocation(downImpulse, applicationPoint);
        }
    }
}
```

### Verification trials

Use the **surfing-straight autopilot** (`AStateTriggerAutoPilot` with `TestName` set so a CSV lands in `Saved/Tests/latest/`). Edit values via the in-game TUNE panel between runs:

| Run | TestPumpInput | PumpForwardOffset | What it tests |
|---|---|---|---|
| **A. Baseline** | 0 | — | Reference trajectory, no pump. |
| **B. At COM** | 1.0 | 0 | Pure downward force, no induced pitch. Tests the velocity-induced-AOA mechanism in isolation. |
| **C. Ahead of COM** | 1.0 | +20 | Forces pitch-down. Tests the hypothesis that the AOA boost from downward velocity dominates the AOA loss from pitch rotation. |
| **D. Behind COM** | 1.0 | -20 | Control: both effects (downward velocity + nose-up rotation) add to sinAOA. Should be the strongest gain. |

`MaxPumpForce` should be aggressive enough that the signal clears single-run noise (see [reference_snapshot_run_variance](../../../C:/Users/esand/.claude/projects/e--windowsgrejor-git-GoneSurfingUE5/memory/reference_snapshot_run_variance.md) — 60× drift between identical runs). Start at 50 kN; raise until B vs A shows a clear gain.

### Comparison metric

The autopilot recorder writes columns `t, x, y, z, vx, vy, vz, roll, pitch, yaw, step, slopeSin, planing, underwater`. Compare runs at the **final timestep** of the autopilot:

- **Forward velocity:** the larger of `|vx|, |vy|` (board's forward axis is local +Y, so usually `vy`).
- **Pitch:** the `pitch` column. Negative = nose-down.

## Phase 1 — Player Controls (Android phone accelerometer)

### Signal extraction

`APlayerController::GetInputMotionState(Tilt, RotationRate, Gravity, Acceleration)` ([SurfboardPawn.cpp:895](../Source/GoneSurfing/SurfboardPawn.cpp#L895)) provides `Acceleration` with gravity already subtracted — the player's induced motion.

Pipeline (each frame, in `ASurfboardPawn::Tick` or a new `UpdatePumpInput` method called alongside `UpdateTiltWeight`):

```cpp
const FVector gUnit = Gravity.GetSafeNormal();
const float A_alongGravity = FVector::DotProduct(Acceleration, gUnit);   // m/s², + = downward
LowPassA = FMath::Lerp(LowPassA, A_alongGravity, dt * PumpLowPassHz);   // ~10 Hz cutoff
const float loadOnly = FMath::Max(0.0f, LowPassA - PumpDeadzone);      // loading phase only
PumpInput = FMath::Clamp(loadOnly / PumpAccelForFull, 0.0f, 1.0f);     // 0..1, public
```

- **Gravity-projected, not world-Z.** Uses live `Gravity` (not `NeutralGravity` from the tilt calibration) so the projection follows the player's current pose.
- **Loading-only** matches real surfing: compress phase generates thrust, extend phase is recovery.
- **Low-pass** smooths the high-frequency accelerometer noise (~30–50 Hz raw → ~10 Hz post-filter).
- **Deadzone** (~1.5 m/s²) swallows the natural 0.3–1 m/s² hand-jitter so the board doesn't pump from holding still.
- The accelerometer's signal type already matches the physics need (transient, returns to zero) — no differentiation step required.

### Wiring to the impulse

`PumpInput` (a public `float` on `ASurfboardPawn`) is read by `AWeightDistribution::Tick` per the snippet above. The pawn writes, WeightDistribution reads — same shape as the existing tilt → `amountInFront` / `amountToTheRight` flow.

### PC fallback (dev-only)

PC is dev-only. Pumping is polled directly via `APlayerController::IsInputKeyDown(EKeys::SpaceBar)` in `UpdatePumpInput`'s `#else` branch — no `IA_Pump` Enhanced Input asset needed. Hold space → `PumpInput` interpolates to 1.0 over ~0.1 s; release → decays over ~0.2 s. Skips the `bExternalWeightOverride` (replay autopilot) case the same way the Android path does.

## UPROPERTYs

Add to `USurfTuningSubsystem` under category `Tuning|Pump`, and register in `kCategoryTable` per [project_tuning_subsystem_two_edits](../../../C:/Users/esand/.claude/projects/e--windowsgrejor-git-GoneSurfingUE5/memory/project_tuning_subsystem_two_edits.md):

| Field | Default | Meaning |
|---|---|---|
| `MaxPumpForce` | 100000 | Newtons. Full-strength downward impulse magnitude (dialed in on Android 2026-06-11). |
| `PumpForwardOffset` | −20.0 | cm along `board.forwards` from COM. Negative = behind COM (toward tail). Phase 0 found this dramatically outperforms at-COM or ahead-of-COM. |
| `PumpLateralOffset` | 0.0 | cm along board's local +X (lateral). Tune to compensate for an asymmetric `centerOfMassOffset` on the surfboard mesh that would otherwise bias roll when pumping. |
| `PumpAccelForFull` | 7.0 | m/s². Phone vertical accel that maps to `PumpInput = 1.0`. |
| `PumpDeadzone` | 0.1 | m/s². Below this, ignore as hand-jitter (dialed in on Android 2026-06-11; the original 1.5 was too aggressive — typical pump gestures barely cleared it). |
| `PumpLowPassHz` | 10.0 | Low-pass cutoff for accelerometer noise. |
| `PumpSlopeAttenuationStart` | 0.05 | `boardWideSlopeSin` at which attenuation begins. |
| `PumpSlopeAttenuationEnd` | 0.50 | `boardWideSlopeSin` at which pump is fully suppressed. |
| `PumpSpeedAttenuationStart` | 300 | cm/s; **relative water velocity** at which pump effectiveness starts to fade. Real pumping saturates because drag scales with v² through the water (not through the world). |
| `PumpSpeedAttenuationEnd` | 600 | cm/s; relative water velocity at which pump produces no force. |
| `PumpSpeedRampInStart` | 100 | cm/s; relative water velocity below which the pump does nothing. A pump converts speed the board already has; a stalled board has no flow to push against. |
| `PumpSpeedRampInEnd` | 250 | cm/s; relative water velocity at which the ramp-in reaches full. `End <= 0` disables. |
| `PumpForwardForceCap` | 6000 | Force units per bottom actor, at a full pump; scales down with the pump gate. Ceiling on the pump-driven forward pair of the bottom pitch hydrofoil. 0 = only `maxHydrofoilForceAmount` applies. |
| `TestPumpInput` | 0.0 | Phase 0 override (0..1). Non-zero bypasses player input. |

## Acceptance Criteria

### AC1: Phase 0 — at-COM beats baseline — **PARTIAL PASS**
With surfing-straight autopilot, `TestPumpInput=1.0, PumpForwardOffset=0, MaxPumpForce=50000`: peak forward velocity 598 cm/s vs 559 cm/s baseline = **+7%** (under the 1.10× target). End-of-run speed actually lower than baseline (73 vs 99) because the constant downward force kills planing → forward damping bites. Constant force at COM produces a sustained equilibrium that's counterproductive in the late game. For a player-driven pulsed input this should be much better; for constant Phase 0 force it's not a clean win.

### AC2: Phase 0 — ahead-of-COM still gains — **PASS**
`PumpForwardOffset=+20`: final pitch −7.5° (clearly nose-down) AND final forward speed 136 vs 99 baseline = **+37%**. The user-predicted effect holds — downward velocity → sinAOA boost dominates the AOA loss from pitch rotation.

### AC3: Phase 0 — behind-of-COM is strongest — **PASS BY HUGE MARGIN**
`PumpForwardOffset=-20`: peak speed 730 vs 559 = **+31%**, final speed 305 vs 99 = **+208%**, distance covered 2.2× baseline. The board sustains `planing = 0.77` through the entire run vs the baseline's collapse to 0.13 — the back-applied force keeps `sinAOA` high → bottom hydrofoil keeps producing forward thrust → planing self-reinforces → forward damping stays low. Decisive.

### AC4: Phase 1 — accelerometer maps to PumpInput
Holding the phone steady: `PumpInput < 0.05` (deadzone swallows hand-jitter). Vigorous pumping at ~1 Hz with peaks ~7 m/s² downward: `PumpInput` reaches ≥ 0.7 on each loading peak.

### AC5: Phase 1 — flat-water pumping accelerates
On flat water (`boardWideSlopeSin < 0.05`) with rhythmic pumping at ~1 Hz: forward velocity reaches **≥ 1.5× the no-pump baseline speed within 5 s**. Numbers placeholder until Phase 0 fixes `MaxPumpForce`; revisit after AC1 lands.

### AC6: Phase 1 — steep-wave attenuation
With `boardWideSlopeSin > 0.5` and `PumpInput = 1.0`, the applied impulse magnitude is `< 0.1 × MaxPumpForce`. Pumping does not noticeably affect speed on the wave face.

### AC7: Wave-thrust unaffected
With pump enabled but `PumpInput = 0`, baseline wave-surf speed is identical to the pre-pump implementation. Verify by running the surfing-straight autopilot pre- and post-pump-merge.

### AC8: Tunable
All fields listed above are EditAnywhere and visible in the in-game TUNE panel under `Tuning|Pump`.

### AC9: Pump-gesture pitch decoupling
Rapid phone up-down motion at ~1 Hz (the pump gesture) — *without intentional weight-shift steering* — produces less than **2°** of board pitch oscillation amplitude. Slow sustained phone-tilt (1+ second hold) still produces the full pitch response per the surfing-controls spec. Verified by holding the phone at the deadband neutral and pumping vertically: the accelerometer's induced incidental tilt from the gesture should not appear as a sustained board pitch change. If it does, tune `AngularDampingX` upward until the criterion passes. See Open Question #1.

## Open Questions / Future Work

1. **Pump-gesture / tilt-axis coupling — likely diminishes max gain.** When the player pushes the phone *down* with the hand, the natural wrist motion also tips the phone *forward* (top edge away from the face). By design, phone-tilt-forward pitches the board nose-down. So the natural pump gesture couples the downward impulse with a nose-down pitch torque — which is the **trial-C scenario** in Phase 0, not trial D.
   - Phase 0 results show C still gains (+37% end speed) — so pumping will likely still work, just not at the theoretical D-scenario maximum (+208%).
   - The default `PumpForwardOffset = -20` applies a nose-up torque from the pump force. The gesture coupling applies a nose-down torque from the weight shift. They partially cancel; the net effect is likely between trial B and trial C in magnitude.
   - **Primary mitigation: tune `AngularDampingX` (pitch-axis angular damping).** Pump gestures are fast (~1 Hz); intentional weight-shift steering is slow (sustained tilt). Higher pitch-axis angular damping makes the board's pitch response sluggish — fast wrist motions don't have time to translate into meaningful pitch rotation, while slow sustained tilts still do (the torque overcomes the damping over time). This is a frequency separation that targets exactly the problem. See AC9.
   - **Alternative mitigations** if damping-tune alone isn't enough:
     - Push `PumpForwardOffset` further back (e.g. −40) so the pump's nose-up torque overpowers the gesture's nose-down torque.
     - Add `PumpForwardOffsetGain` so the offset becomes more negative as `PumpInput` rises.
     - Temporarily damp `amountInFront` updates while `PumpInput` is high.
2. **Frequency cap.** Should oscillation faster than ~3 Hz be attenuated to prevent exploit? Currently the low-pass at 10 Hz already softens this; revisit if playtest finds an exploit.
3. **HUD pump indicator.** Visual feedback for `PumpInput` (bar near the velocity readout) would help players learn the rhythm. Deferred.
4. **Rotation-rate gating.** If steering (phone-tilt for weight shift) produces false-positive pump signal via the off-axis-pivot acceleration, gate by `RotationRate.Size() > threshold → attenuate PumpInput`. Hold until playtest surfaces the issue.
5. **Pump-during-turn composition.** Pump is purely longitudinal; lateral turn is independent. They should compose without interlock, but watch for surprises in playtest.

## Implementation Phases

- **Phase 0**: Subsystem fields (`TestPumpInput`, `PumpForwardOffset`, `MaxPumpForce`, `PumpSlopeAttenuationStart/End`) + 15-line hook in `AWeightDistribution::Tick`. Ran A/B/C/D autopilot trials.
- **Phase 1**: Pawn-side `UpdatePumpInput` (accelerometer pipeline) + `PumpInput` field + remaining UPROPERTYs (`PumpAccelForFull`, `PumpDeadzone`, `PumpLowPassHz`).
- **Phase 1 add-ons (post-PC-playtest)**: `PumpLateralOffset` for COM-asymmetry compensation; bottom-hydrofoil `turnGate` extended to open on `MaxPumpForce × PumpInput` (fixing "pumping straight on PC produced no acceleration"); `PumpSpeedAttenuationStart/End` keyed on `relativeWaterVelocityMagnitude` (fixing "held pump accelerates to absurd speeds"); debug draw of impulse application point.
- **Phase 2**: PC dev fallback (hold space-bar via `IsInputKeyDown`). HUD indicator + rotation-rate gate still deferred until playtest reveals need.

## Status

- [x] Spec reviewed
- [x] Phase 0: `TestPumpInput` + `PumpForwardOffset` + `MaxPumpForce` + slope-attenuation fields added to subsystem
- [x] Phase 0: `AWeightDistribution::Tick` hook lands
- [x] Phase 0: surfing-straight autopilot CSVs collected for A / B / C / D
- [x] Phase 0: AC1 (partial) / AC2 / AC3 verified; default `PumpForwardOffset = -20`, `MaxPumpForce = 20000` (lowered from Phase 0's 50 000 trial value after live PC playtest)
- [x] Phase 1: `ASurfboardPawn::UpdatePumpInput` + accelerometer pipeline
- [x] Phase 1: `PumpInput` public field, read by `AWeightDistribution::Tick`
- [x] Phase 1: `PumpAccelForFull`, `PumpDeadzone`, `PumpLowPassHz` UPROPERTYs added
- [x] Phase 1: slope attenuation via `boardWideSlopeSin` (SC wired into WD from SurfboardUtils::BeginPlay)
- [x] Phase 1: `PumpLateralOffset` for compensating an asymmetric `centerOfMassOffset`
- [x] Phase 1: bottom-hydrofoil `turnGate` now opens on either lateral shift or `MaxPumpForce × PumpInput`
- [x] Phase 1: speed attenuation (relative water velocity) — `PumpSpeedAttenuationStart/End` taper `PumpInput` so held-pump can't drive the board to absurd speeds
- [x] Phase 2: PC dev fallback (hold space-bar via `IsInputKeyDown(EKeys::SpaceBar)`)
- [x] Phase 2: debug draw (cyan arrow + sphere at application point, yellow sphere at COM)
- [x] 2026-09-12: proportional gate + `PumpForwardForceCap` + `PumpSpeedRampInStart/End` (stationary-pirouette exploit); player-validated on PC 2026-09-12
- [ ] AC4 / AC5 / AC6 / AC7 / AC8 / AC9 verified on device
- [ ] Memory: pumping mechanism + gate-coupling pattern recorded once stable

## Related Files

- [FluidDynamics.cpp:1020-1110](../Source/GoneSurfing/FluidDynamics.cpp#L1020-L1110) — `calcThrustForce`, the bottom hydrofoil that converts downward motion to forward thrust via `sinAOA × v²`.
- [SharedCalculations.h](../Source/GoneSurfing/SharedCalculations.h) — `boardWideSlopeSin` exposed for the wave-face attenuation gate.
- [SurfTuningSubsystem.h](../Source/GoneSurfing/SurfTuningSubsystem.h) — destination for new `Tuning|Pump` UPROPERTYs. See [[project_tuning_subsystem_two_edits]] for the two-edit rule (UPROPERTY + `kCategoryTable` row).
- [WeightDistribution.cpp](../Source/GoneSurfing/WeightDistribution.cpp) — site of the pump impulse application in `Tick`.
- [SurfboardPawn.cpp:885-948](../Source/GoneSurfing/SurfboardPawn.cpp#L885-L948) — `UpdateTiltWeight` pattern that the pump-input pipeline mirrors.
- [tilt-axis-swap-bug.md](tilt-axis-swap-bug.md) — same orientation-invariant projection math (gravity unit vector) that the pump's `A_alongGravity` computation reuses.
- [runtime-tuning.md](runtime-tuning.md) — subsystem framework; pump fields land in its `Tuning|Pump` category.
- Engine fork [`PBDRigidsEvolutionGBF.cpp` damping block](../../UnrealEngine/Engine/Source/Runtime/Experimental/Chaos/Private/Chaos/PBDRigidsEvolutionGBF.cpp) — per-axis damping + velocity ceilings; naturally bound pump-induced speeds at the high-velocity wall.
