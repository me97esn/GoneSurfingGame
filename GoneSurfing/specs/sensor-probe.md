# Spec: On-Device Sensor Capture Wizard

## Overview

A scripted, on-screen wizard that walks the player through a list of phone poses and
motions, showing live sensor readouts, and writes every motion value to a CSV for offline
analysis.

## Objective

Replace guess-deploy-retest with measurement. The gyro-yaw fusion
([tilt-yaw-fusion.md](tilt-yaw-fusion.md)) measured **inert** on device — the term reads
exactly zero and is immune to both its gain and its sign — and every remaining hypothesis
(sensor not delivering, frame mismatch, units, tuning-subsystem plumbing) needs ground
truth that no amount of source reading can supply. Each blind iteration costs a full
Android build and deploy.

## Requirements

### FR1 — Scripted steps
Static poses pin the device frame against gravity; motion steps are the ones that matter
for the yaw bug, since they are the only way to observe `RotationRate` at all.

| # | Label | What it establishes |
|---|---|---|
| 1 | `flat` | Device frame vs gravity at a known reference (expect `g ≈ (0,0,±1)`) |
| 2 | `upright` | θ = 90°, the blind-axis crossover |
| 3 | `reclined` | The actual failing pose |
| 4 | `swing_left` | Gyro presence + **sign** against an intended lean |
| 5 | `swing_right` | Sign symmetry |
| 6 | `twist_left` | The gesture that should still work at vertical |
| 7 | `pitch_down` | Confirms the pitch axis is unaffected |

### FR2 — Capture the motion, not the aftermath
Each OK press flushes a **rolling ~2 s ring buffer** (140 samples at 60 Hz), not a single
instant. A motion step is performed *then* confirmed, so the gesture must already be in
the buffer when the button is pressed. The sample at the press is flagged `is_mark=1`.

### FR3 — Live on-screen readouts
Gravity, gyro, `g.z`, `gyro · ĝ`, the world's **paused** flag, and a sample counter that
must visibly climb.

The paused flag and the counter are there because a stalled sampler is otherwise
indistinguishable from a working one until the file is pulled — which is exactly how the
v1 failure went unnoticed through six full sessions.

This is the safety net, not decoration. A frozen or all-zero sensor is then obvious on the
phone *before* any file exists — the alternative is a CSV full of identical rows that
looks like a successful measurement. This exact failure mode is why the feature exists.

### FR4 — Sampling must not depend on the world ticking
Sampling runs on the overlay's own **Slate active timer**, which keeps firing while the
world is paused.

**This was got wrong first time and produced a silent total failure.** v1 pushed samples
from `ASurfboardPawn::Tick`. But the start screen calls `SetGamePaused(World, true)`, and
this overlay sits above it at ZOrder 400 — so the entire probe session runs on a paused
world where the pawn never ticks. Six sessions produced six header-only 421-byte files and
a readout that never updated. The reasoning error was checking "does the probe pause the
world?" (it doesn't) instead of "is the world *already* paused when the probe is used?"
(it is).

The probe still reads through `APlayerController::GetInputMotionState` — the same call the
tilt path uses — so it measures what the gameplay code sees, just driven by a clock that
does not stop.

The pawn's calibrated tilt basis is supplied best-effort via `SetTiltBasis` when it does
tick; it only fills the derived pitch/roll columns and the raw vectors do not depend on it.

### FR5 — Self-reporting output path
The wizard displays the **resolved absolute** CSV path on completion and logs it at
`Display`. The on-device `Saved/` location varies by packaging; a confidently-printed
wrong path costs more than no path.

### NFR1 — Never the occluded thing
ZOrder 400, above the tuning HUD (300) and every gameplay overlay. Input mode is forced to
`GameAndUI` while active (see `ASurfboardPawn::ApplyGameplayInputMode`), or the OK button
is unclickable — the same defect that made the tuning HUD dead.

## CSV Format

Metadata block, then one row per buffered sample:

```
# session, platform, device_orientation, unify_motion_space, ring_capacity
step,label,t,tilt_x..z,gyro_x..z,grav_x..z,acc_x..z,gmag,gz,pitch_deg,roll_deg,yaw_rate,is_mark
```

`device_orientation` and `unify_motion_space` are in the header because the engine remaps
all four motion vectors by screen orientation, and `Android.UnifyMotionSpace` decides
whether they share a frame at all (see
`AndroidInputInterface.cpp::QueueMotionData`). Neither can be recovered from the rows.

`yaw_rate = gyro · ĝ` is precomputed — it is the single number the whole bug turns on.

## Acceptance Criteria

### AC1: Gyro liveness is unambiguous
During any motion step, the on-screen `gyro` line changes visibly. If it holds `0.000`
while the phone rotates, the sensor is not reaching the game and the yaw fusion cannot
work as designed — that is the finding, and it redirects the whole investigation.

### AC2: Motion is captured
`swing_left` rows show a non-zero `yaw_rate` excursion in the samples *before* `is_mark=1`.

### AC3: Sign is readable
`swing_left` and `swing_right` produce `yaw_rate` excursions of opposite sign. Their sign
against the intended lean is what sets `TiltYawInvert`.

### AC4: Pose maths confirmed
`flat` gives `|gz| ≈ 1`; `upright` gives `|gz| ≈ 0`; `reclined` gives `gz` of opposite
sign to `flat` — the cos θ crossover, measured rather than derived.

### AC5: No interference
With `bRunSensorProbe = false` nothing is installed and no samples are pushed.

## Status
- [x] Overlay + ring buffer + CSV writer implemented
- [x] Pawn wiring (install, tilt-basis feed, uninstall, input mode)
- [x] Editor target builds clean
- [x] Run on device, CSV pulled — `Tests/SensorProbe/probe-2026-08-20-16-08-57.csv`
- [x] **Answered the question it was built for**: the fusion's blend weight was inverted
      (device Z is the in-plane vertical axis, not the screen normal). See
      [tilt-yaw-fusion.md](tilt-yaw-fusion.md).
- [x] `bRunSensorProbe` back to `false`
- [x] Wizard dismisses itself (second OK on the done screen) — it previously sat over the
      game at ZOrder 400 for the rest of the session

### Defects found in the probe itself
1. **v1 sampled from the pawn's Tick** → nothing captured at all on a paused world. See FR4.
2. **Ring buffer not cleared between steps** → a step pressed within ~2 s of the previous
   one inherited the tail of that gesture. Visible in the 2026-08-20 capture as an
   identical peak in both swing steps. Did not affect the conclusion (dominant signs
   separated cleanly) but it is a trap. Fixed.
3. **No way to dismiss it.** Fixed.

## Related Files
- [SensorProbeOverlay.h](../Source/GoneSurfing/SensorProbeOverlay.h) / [.cpp](../Source/GoneSurfing/SensorProbeOverlay.cpp)
- [SurfboardPawn.cpp](../Source/GoneSurfing/SurfboardPawn.cpp) — `bRunSensorProbe`, the Tick push, `ApplyGameplayInputMode`
- [tilt-yaw-fusion.md](tilt-yaw-fusion.md) — the defect this exists to diagnose
- [input-trace-replay.md](input-trace-replay.md) — the other on-device recorder; different
  purpose (gameplay replay, not sensor forensics)
