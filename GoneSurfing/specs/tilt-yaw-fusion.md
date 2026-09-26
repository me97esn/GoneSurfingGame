# Spec: Gyro Yaw Fusion for Sideways Weight (Near-Vertical Phone Poses)

## Bug Description

When the player holds the phone close to vertical — lying back, phone held up so the
screen angles down toward the face — the sideways weight input (`amountToTheRight`)
becomes weak or unresponsive, and past vertical it becomes **inverted**. Forward/back
weight (`amountInFront`) is unaffected.

Reported from playtest: *"when a user is lying down, the phone is held vertically, or
might even be slightly pitched back so it is rotated more than 90 degrees. During this
position it seems like the sideways motions of the phone isn't rolling the board
correctly."*

## Current Behavior

[SurfboardPawn.cpp](../Source/GoneSurfing/SurfboardPawn.cpp) `UpdateTiltWeight()` computes

```cpp
RollSin = dot(G, TiltRightAxis);      // TiltRightAxis ≈ the phone's long (screen left↔right) axis
RollDeg = asin(RollSin);
```

i.e. it measures *"is one end of the phone lower than the other."* This is a pure
gravity measurement.

## Root Cause: gravity cannot see rotation about the gravity axis

Let **θ** be how far the phone is tipped up from flat:

| θ | pose |
|---|---|
| 0° | flat on a table, screen up |
| 90° | screen vertical, facing the player |
| >90° | screen angled down toward a player lying on their back |

Put gravity in device coordinates and rotate by each candidate gesture (φ = gesture angle):

| gesture | pivot axis | resulting `RollSin` |
|---|---|---|
| **swing** — pivot the phone about the screen's up↔down axis (what a player does when the phone is near vertical; reads as "yaw") | Y_dev | `−cos θ · sin φ` |
| **twist** — rotate the phone in its own plane, like a steering wheel | Z_dev | `+sin θ · sin φ` |

The swing gesture's sensitivity is **cos θ**:

| θ | swing sensitivity | note |
|---|---|---|
| 0° (flat) | 100% | |
| 60° | 50% | |
| 90° (vertical) | **0%** | the pivot axis *is* the gravity axis — physically unmeasurable |
| 110° (lying back) | 34% | **and sign-inverted** (cos goes negative past 90°) |

At θ = 110°, with `TiltRollDegreesForFullDeflection = 25` and
`TiltAngleDeadzoneDegrees = 2`, a swing needs ≈74° to reach full deflection, and steers
the wrong way. That is the reported symptom.

Two consequences worth stating explicitly:

- **Recalibrating does not help.** Calibration sets the neutral pose and the axes; the
  blind axis is always the world vertical, so the cos θ falloff is unchanged.
- **The pitch axis is provably unaffected.** For a calibration pose θ₀ and current pose
  θ, `dot(G, TiltForwardAxis) = sin(θ − θ₀)` exactly — the pitch readout is the true
  angle delta at every pose. Consistent with only sideways being reported broken.

The twist gesture *does* work at θ = 110° (94% sensitivity, correct sign). It is
presumably unintuitive there because twisting rotates the image relative to the
player's eyes.

## Expected Behavior

Sideways weight responds to the player's natural sideways gesture at every hold pose
from flat through past-vertical, with consistent sign, without regressing the poses
that work today.

## Fix: add a gyro-derived yaw term, weighted by pose

Rotation about the gravity vector is *exactly* the null space of gravity sensing. So a
yaw term adds information the gravity term structurally cannot contain — the two are
orthogonal by construction and can be **summed without double-counting**.

```cpp
const FVector G = Gravity.GetSafeNormal();

// --- gravity channel (unchanged) ---
const float GravityRollDeg = RadToDeg(Asin(dot(G, TiltRightAxis)));

// --- gyro channel: rotation about the axis gravity is blind to ---
const float YawRate = dot(RotationRate, G) - TiltYawRateBias;   // rad/s
TiltYawAngleDeg += RadToDeg(YawRate) * dt;
TiltYawAngleDeg -= TiltYawAngleDeg * min(dt / LeakSeconds, 1);  // leak toward neutral

// --- pose-weighted sum ---
const float SwingSensitivity = Abs(G.Z);   // = |cos θ|; 1 = flat, 0 = vertical
const float YawWeight        = 1 - SwingSensitivity;
RollDeg = GravityRollDeg + YawWeight * YawGain * TiltYawAngleDeg;
```

`G.Z` is the gravity component along the screen normal, which is exactly `cos θ` — the
blend weight falls straight out of the same number that describes the problem. No extra
pose estimation needed.

### Why sum rather than lerp

An earlier sketch lerped between the two channels. That is wrong at θ = 90°, where the
lerp weight reaches 1.0 and would discard the gravity channel entirely — killing the
**twist** gesture, which is at *full* sensitivity in exactly that pose. Summing keeps
the gravity channel at full authority everywhere (twist keeps working at every pose) and
adds swing authority only where gravity loses it.

### Why the past-vertical inversion stops mattering

At θ = 110° the sum is 0.66 × (correctly-signed gyro swing response) + 0.34 × (gravity
channel, whose swing response is itself only 0.34 and wrong-signed → 0.12 effective).
The correct term dominates ~5:1. Flipping the gravity term's sign by `sign(G.Z)` was
considered and rejected: it would invert the twist gesture, which is the *strong,
correct* channel in that pose.

### Drift management

Integrated gyro drifts. Two mitigations, both required:

1. **Bias estimate at calibration.** Average `dot(RotationRate, G)` over the first
   `TiltYawBiasSampleSeconds` (1.0 s) after handoff and subtract it thereafter. The yaw
   channel contributes 0 during that window. Phone MEMS gyro bias is typically
   0.5–2 °/s; unremoved, a 2 °/s bias against a 2 s leak parks the input at a permanent
   ~4° lean — above the 2° deadzone, i.e. a stuck turn.
2. **Leak toward neutral** (`TiltYawLeakSeconds`, default 2.0 s). Bounds any residual
   bias and also discards slow whole-body rotation (player turning on the couch).

The leak is affordable here specifically because centered *is* the resting semantics of
this input — the stick/mouse paths already auto-center on release via
`WeightAutoCenterDuration`. The constraint is that the leak must outlast a carve; 2 s
against ~1–2 s carves is the intended margin.

## Sign Convention — MUST BE VERIFIED ON DEVICE

The gyro sign cannot be derived from the gravity path (that is what "orthogonal" means),
and it depends on three things not determinable by reading source: whether Android's
`Gravity` points up (specific force) or down, the gyro's handedness after UE's
`AndroidUnifyMotionSpace` remap, and which swing direction a player means by "lean left."

Direction is therefore its own knob, **`TiltYawInvert`** on `USurfTuningSubsystem`:
`>= 0.5` reverses, default `0.0`. `TiltYawGain` carries magnitude only (default `1.0`;
`0` disables fusion entirely, restoring exact pre-fix behaviour).

Splitting sign from magnitude is what makes the check a two-second job rather than a
redeploy. `SurfTuningHUD` builds one slider row per `FFloatProperty` on the subsystem, so
all of these appear in the on-device gear panel automatically — but that slider maps
`0 .. 4 × captured default` and **can never cross zero**. A signed gain would have been
unflippable from the phone. A float defaulting to `0.0` instead takes the HUD's
zero-default branch, which gives a raw `0..1` slider — usable as an on-device toggle. The
HUD pauses the world while open, so the loop is: ride in the failing pose, open the gear,
drag `TiltYawInvert`, close, ride again.

`bShowTuningHUD` on the pawn must be on in the level for the gear button to appear.

## Input-Trace / Replay Compatibility

`LatestTiltRollDeg` (trace column 2, `tilt_roll_deg`) now carries the **fused** sideways
angle, still pre-deadzone and pre-scale. This is deliberate:

- [InputReplayAutoPilot.cpp](../Source/GoneSurfing/InputReplayAutoPilot.cpp)
  `OffsetFromTilt()` re-runs the deadzone/scale pipeline on that column, so replay stays
  correct with **zero changes** and deadzone/full-deflection remain tunable post-record.
- Traces recorded before this change hold pure gravity roll and replay exactly as they
  did before.

Three provenance keys are added to the trace metadata block (`tilt_yaw_gain`,
`tilt_yaw_leak`, `tilt_yaw_bias_rads`). `TryParseMetadataLine` ignores unknown keys, so
this is backward- and forward-compatible in both directions.

## Acceptance Criteria

### AC1: Sideways works near vertical
Calibrate holding the phone at ≈90–110° (screen angled down toward a reclining player).
Swing the phone ±20° about the vertical axis. `amountToTheRight` moves off 0.5 in the
direction of the swing, reaching ≳0.8 / ≲0.2 at the extremes.

### AC2: No inversion through vertical
Repeat AC1 at θ ≈ 70°, 90°, and 110°. The sign of `amountToTheRight` for a
swing-left gesture is the same at all three.

### AC3: Flat pose unregressed
Calibrate flat-ish (θ ≈ 20–40°). Bank the phone left/right. Behaviour is
indistinguishable from pre-fix — `YawWeight` is ≤0.06 at θ ≤ 20°, so the gyro term is
effectively absent.

### AC4: Twist gesture still works at vertical
At θ = 90°, twist the phone in its own plane. `amountToTheRight` responds at full
sensitivity (this is the sum-not-lerp requirement).

### AC5: No drift-induced stuck turn
Calibrate, then hold the phone still for 30 s at θ = 90°. `amountToTheRight` stays
within 0.5 ± 0.05 (the fused angle stays inside the 2° deadzone).

### AC6: Pitch axis untouched
Pure pitch motion moves `amountInFront` and leaves `amountToTheRight` within deadzone of
0.5, at every pose in AC2.

### AC7: Kill switch
`TiltYawGain = 0` (gear panel or `Saved/TuningOverrides.json`) reproduces pre-fix
behaviour exactly.

### AC8: Sign flip is reachable on device
`TiltYawInvert` appears as a slider in the gear panel under the Tilt group, spans 0..1,
and reverses the sideways direction mid-session without a rebuild.

## Test Cases

### TC1: On-device sign check
No logs required. Ride in the reclining pose (θ ≈ 100–110°) and swing the phone toward a
lean. If the board goes the other way, open the gear panel and drag `TiltYawInvert` to 1.
The verdict is unambiguous by feel at that pose: if the sign is wrong, the gyro term
*and* the residual gravity term both push the wrong way, so it steers plainly backwards
rather than merely weakly.

For numbers rather than feel, launch with `surf.debug.flags tilt` — the log line prints
`gravRoll`, `yaw`, `yawW` and `bias` alongside the fused `RollDeg`, and marks the bias
window with `CALIBRATING`. This is the one thing that cannot be settled without hardware.

### TC2: Bias check
In the same log, after the 1 s calibration window, hold the phone still and confirm
`yaw` stays within ~±1° over 30 s.

### TC3: Existing trace replays unchanged
Replay any trace under [Tests/InputTraces/](../Tests/InputTraces/) recorded before this
change. Trajectory must be identical to pre-fix — those rows contain pure gravity roll
and the replay path is untouched.

### TC4: Snapshot tests
`RunGameAndCollectLogs.bat` — all baselines unchanged. The entire fusion path is inside
`#if PLATFORM_ANDROID`, so headless PC tests cannot reach it.

## Status

> ⚠️ **REVERT BEFORE SHIPPING (2):** `bDebugTilt` in
> [SurfboardPawn.h](../Source/GoneSurfing/SurfboardPawn.h) was flipped to `true` to get the
> per-tick tilt numbers on device (an APK can't easily take `-ExecCmds`, so the
> `surf.debug.flags` route isn't available there). Set back to `false` with the flag below.
>
> ⚠️ **REVERT BEFORE SHIPPING:** `bShowTuningHUD` in
> [SurfboardPawn.h](../Source/GoneSurfing/SurfboardPawn.h) was flipped from `false` to
> `true` so the `TiltYawInvert` slider is reachable on device. Set it back to `false` once
> the sign is confirmed — otherwise players get a gear button. No Blueprint or level asset
> overrides this flag, so the C++ default is the only place it lives.

- [x] Root cause identified (cos θ falloff + sign inversion past 90°)
- [x] Implemented in [SurfboardPawn.cpp](../Source/GoneSurfing/SurfboardPawn.cpp)
- [x] Tunables on [SurfTuningSubsystem.h](../Source/GoneSurfing/SurfTuningSubsystem.h)
- [ ] **Sign verified on device (TC1)** — blocking; default is a coin-flip guess
- [ ] Playtested at θ ≈ 90–110° (AC1, AC2)
- [ ] Flat pose regression checked (AC3)
- [ ] Drift checked over 30 s (AC5)
- [ ] New trace recorded post-fix for regression baseline
- [ ] **`bShowTuningHUD` reverted to `false`** (see warning above)
- [ ] **`bDebugTilt` reverted to `false`** (see warning above)
- [x] **`bRunSensorProbe` reverted to `false`** (see [sensor-probe.md](sensor-probe.md))

## RESOLVED (2026-08-20): the blend weight was inverted

Measured with the sensor probe ([sensor-probe.md](sensor-probe.md)). Gravity at each
static pose, in device coordinates:

| pose | g (device) | \|g.z\| |
|---|---|---|
| flat, screen up | (-1.004, -0.018, 0.001) | **0.00** |
| upright / vertical | (0.043, 0.022, 1.004) | **1.00** |
| reclined (the failing pose) | (0.393, -0.028, 0.930) | **0.92** |

**Device Z is not the screen normal — device X is.** Z is the phone's *in-plane vertical*
axis (screen top-to-bottom), which is precisely the swing gesture's pivot. The motion steps
confirm all three axes independently, by which gyro component dominates: swing pivots about
Z, board-pitch about Y (the long axis), twist about X.

The code assumed Z was the screen normal and computed `YawWeight = 1 - |g.z|`. Correct is
`YawWeight = |g.z|` — gravity is blind to rotation about an axis parallel to itself, so
|g.z| *is* the blindness and therefore the gyro's weight directly.

The inversion scaled the gyro to **0.08 in the reclined pose** — the exact pose the fusion
exists to fix — which is why the term read as inert and was immune to both its gain and its
sign. `TiltYawGain = 4` moved it to ~0.3 of intended, still below notice.

**Measured polarity:** swing-left produces a **positive** yaw rate (mean +0.61 across the
gesture; swing-right gave −1.16), while leaning left requires a **negative** sideways
angle. The base mapping is therefore negative, baked in as `MeasuredYawPolarity = -1`.
`TiltYawInvert` remains as the escape hatch for a different device or grip.

`|g.z|` is robust across both landscape orientations: the engine's reorient gives
`out.Z = ±in.X`, so the absolute value picks the same physical axis either way.

### Superseded analysis (2026-08-19): "yaw term measured inert on device"

Playtest at θ > 90°: board rolls the wrong way for **both** `TiltYawInvert = 0` and `= 1`,
for both the yaw and the roll gesture. Below vertical, both settings steer correctly.

A sign flip that changes nothing means the term it multiplies is ~0 — so `TiltYawAngleDeg`
is not accumulating, and what is being felt is the unfixed gravity-only behaviour
(correct below vertical, sign-inverted above it, exactly as the cos θ analysis predicts).

That both gestures invert together is consistent rather than a second defect: above
vertical the phone's screen-up axis is near-vertical, so "yaw the phone" and "bank the
phone" converge on the same rotation about the near-gravity axis, and both are governed by
cos θ.

Candidates, in order:
1. `RotationRate` arriving as zero (would show as `rawRate≈0` in the tilt log while the
   phone is rotating). Engine side looks fine — `GameActivity.java.template` registers the
   gyro unconditionally when present, and falls back to a tilt-delta rate when absent.
2. The bias window never completing, leaving `bTiltYawBiasReady` false forever, which pins
   `TiltYawAngleDeg` at exactly 0. Would show as a permanent `CALIBRATING` marker.
3. Magnitude far smaller than expected (units or frame mismatch on `RotationRate`).

The debug log now prints `rawRate` and the whole `gyro` vector to separate (1) from (2).

## Related Files
- [SurfboardPawn.cpp](../Source/GoneSurfing/SurfboardPawn.cpp) — `UpdateTiltWeight()`,
  calibration block in `OnPlayerControlsEnabled()`, trace header in `StartInputTrace()`.
- [SurfboardPawn.h](../Source/GoneSurfing/SurfboardPawn.h) — yaw state + fallback UPROPERTYs.
- [SurfTuningSubsystem.h](../Source/GoneSurfing/SurfTuningSubsystem.h) — `TiltYawGain`,
  `TiltYawInvert`, `TiltYawLeakSeconds`, `TiltYawBiasSampleSeconds`.
- [SurfTuningHUD.cpp](../Source/GoneSurfing/SurfTuningHUD.cpp) — the on-device gear panel
  these knobs surface in; its `ValueToSliderUnits` 0..4×default mapping is why the sign
  had to be split out of the gain.
- [tilt-axis-swap-bug.md](tilt-axis-swap-bug.md) — the previous tilt-axis fix; establishes
  the `Seed = +Z` screen-normal convention this spec's `G.Z` relies on.
- [surfing-controls.md](surfing-controls.md) — parent spec, Phase 6 (Android tilt).
- [input-trace-replay.md](input-trace-replay.md) — recording/replay infrastructure.
