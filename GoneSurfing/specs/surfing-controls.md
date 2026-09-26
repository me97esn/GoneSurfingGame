# Spec: Surfing Controls (Paddle, Turn, Weight Shift)

## Overview
Replace the proof-of-concept input scheme in `ASurfboardPawn` with a playable control surface for actual surfing. Adds analog weight-shift control (forward/back and left/right on the board) for steering while riding a wave, alongside refined paddle and turn controls for getting into position. Supports both Android (touch joysticks) and PC (keyboard + mouse).

## Objective
Expose the inputs a real surfer cares about:
- **Paddle forward** with variable intensity to build speed.
- **Turn the board** while paddling or stationary to change heading.
- **Shift body weight** on the board to steer, trim, and engage rails while surfing.

## Background
The current [SurfboardPawn](../Source/GoneSurfing/SurfboardPawn.cpp) is a PoC:
- Paddle is a 2D camera-relative force vector that also steers via an off-center application point.
- Weight distribution (`amountInFront` / `amountToTheRight` in [WeightDistribution.h](../Source/GoneSurfing/WeightDistribution.h)) is controlled exclusively by [AutoPilot](../Source/GoneSurfing/AutoPilot.h), which is a dev-only test harness.
- Player cannot control weight at all.

Subsequent playtesting of the v1 input scheme (right thumbstick for weight on Android, RMB+mouse on PC) showed the right thumbstick-on-glass to be too imprecise for surfing-style weight shifts, and the mouse equally awkward on PC. Android is the target platform; PC is dev-only. The current iteration of this spec adds **device tilt** as the primary Android weight input — the player physically leans the phone to shift the surfer's weight, which thematically mirrors what a real surfer does. The thumbstick path is retained behind a UPROPERTY toggle for A/B comparison.

## Scope
This spec defines the player control surface. It does **not** change:
- Camera system (Behind / Beside cameras, toggle via C) — unchanged.
- Restart action — unchanged.
- Physics of the weight distribution system itself — we only drive the existing `amountInFront` / `amountToTheRight` properties.
- Autopilots remain in the codebase for dev use; they must be disabled in playable levels.

## Control Model

### Paddle forward (analog)
- 0..1 magnitude scales a forward force applied at the surfboard's center.
- PC: initially **binary** (W = full paddle). Two-speed (Shift+W = hard) deferred.
- Android: analog via left stick Y-axis.

### Turn (binary)
- Fixed-magnitude turn force (tunable UPROPERTY), either on-left, on-right, or off.
- **Independent of paddle**: turning works with or without forward paddle input.
- Implemented as a sideways force at a forward offset in surfboard local space (matches the existing mechanism, minus the 2D decomposition).
- PC: A / D keys.
- Android: left stick X-axis, thresholded (e.g., |X| > 0.5 triggers turn on that side).

### Weight shift (analog, 2D)

> **Being replaced on Android (2026-09-09).** A virtual joystick takes over weight shift and a held
> button takes over pumping — see `specs/pump-button-and-virtual-stick.md`. Tilt stays behind the
> `WeightInputSource` tuning switch for A/B. The PC paths below are unaffected.

- Drives `AWeightDistribution::amountInFront` and `amountToTheRight` directly.
- Full deflection = 0.0 or 1.0 on that axis. Center = 0.5.
- **Sensitivity configurable** via UPROPERTY, with sensible defaults.
- **Independent of paddle and turn** — any combination can be active. No interlock.
- **Forward weight is capped** at `WeightMaxInFront` (live-tunable, `Tuning|Weight`, default 0.45).
  Player input only — autopilots and the pop-up intro write `amountInFront` directly and are not
  capped. Rationale: a nose-heavy board stops answering the roll axis entirely, and players read
  that as the game ignoring input, not as physics. Keeping a little weight on the tail at all times
  means the board always turns *some* amount. Below the cap the mapping is untouched, so response
  stays proportional to tilt; at 0.45 the forward half of the tilt range is spent. Raise toward
  0.6–0.7 to hand forward authority back, 1.0 to remove the limit. 0.45 is a first guess, not a
  tuned value.
- The **assist is exempt** from the cap. Its trim targets sit above the ceiling, so capping its
  output discarded every forward correction it made — the ceiling governs what the player can
  hold, not what the assist may correct to.

**Android — device tilt (default):**
- Phone gravity vector (from `APlayerController::GetInputMotionState`) is decomposed into pitch (forward/back lean) and roll (left/right lean) relative to a calibrated neutral.
- Pitch delta drives `amountInFront`; roll delta drives `amountToTheRight`. Sign per axis configurable (`bInvertTiltPitch` / `bInvertTiltRoll`).
- Per-axis "full deflection" angles configurable (`TiltPitchDegreesForFullDeflection`, `TiltRollDegreesForFullDeflection`, default 18°).
- Small dead-band around neutral (`TiltAngleDeadzoneDegrees`, default 2°) to suppress sensor noise.
- **No auto-center.** Tilt is intrinsically live — the phone's resting pose *is* the neutral. When the player stops leaning, weight returns naturally because the sensor reading does.
- **Calibration**: neutral gravity vector is snapshotted on the tick `bPlayerControlsEnabled` transitions to true (the "SURF!" handoff moment). No mid-ride recenter in v1; revisit if drift bites in playtesting.

**Android — right stick (fallback / A/B):**
- Right stick deflection maps directly to weight offset.
- **Auto-centers on release** over `WeightAutoCenterDuration` (default ~0.3 s) since the stick has no intrinsic neutral once released.
- Selected via `AndroidWeightInputSource` UPROPERTY (`Tilt` default, `Stick` for comparison).

**PC — right mouse + mouse movement:**
- Right mouse button held + mouse movement. Cursor is hidden and locked (FPS-style relative mode); raw mouse deltas integrate into a virtual offset from center. On RMB release, cursor is restored and weight interpolates back to (0.5, 0.5) over `WeightAutoCenterDuration`.
- PC path unchanged by this iteration.

### Camera toggle / Restart
Unchanged from current implementation.

## Input Mapping Summary

| Action | Android | PC |
|---|---|---|
| Paddle forward (analog) | Left stick Y (+) | W (binary for now) |
| Turn left | Left stick X past `-Threshold` | A |
| Turn right | Left stick X past `+Threshold` | D |
| Weight shift (2D analog) | **Device tilt (default)** / right stick (fallback) | RMB held + mouse XY |
| Toggle camera | On-screen button | C |
| Restart | On-screen button | R |

The right stick is currently unused on Android; this spec claims it. Device tilt is the default Android weight input; the right stick is retained behind a UPROPERTY toggle. The current single `IA_Paddle` Vector2D action is replaced by two dedicated actions (see below).

Note: in the current implementation the player does not actually paddle — `AStateTriggerAutoPilot` handles the catch-the-wave phase and control hands off to the player once already surfing. The left-stick paddle/turn bindings remain wired up but are unused in typical play. Tilt weight control therefore stands alone as the player's primary input.

## Architecture

### Enhanced Input Actions

**Replace**:
- `IA_Paddle` (Vector2D) — superseded.

**Add**:
- `IA_PaddleForward` (float, 0..1): forward paddle intensity.
- `IA_Turn` (float, -1..+1): turn direction. On Android, bound through left stick X with a threshold (or thresholded in C++). On PC, A and D each contribute ±1.
- `IA_Weight` (Vector2D): weight shift input. Android = right stick. PC = mouse XY (raw axis, integrated in C++).
- `IA_WeightGate` (bool, PC-only): right mouse button. While held, weight action is "live" and cursor is locked. On release, cursor is freed.

**Unchanged**:
- `IA_ToggleCamera`, `IA_Restart`.

### `ASurfboardPawn` additions

New UPROPERTYs (names illustrative):

```cpp
// Turn
float TurnForceStrength          = 10000.0f;   // magnitude applied when turning
float TurnForceForwardOffset     = 100.0f;     // replaces SidewaysForceForwardOffset
float AndroidTurnThreshold       = 0.5f;       // |left stick X| past this = turn

// Weight shift (general)
TObjectPtr<AWeightDistribution> WeightDistribution;
float WeightAutoCenterDuration   = 0.3f;       // stick + PC mouse paths only

// Weight shift (PC mouse)
float MouseWeightSensitivityX    = 400.0f;     // pixels of mouse travel per full deflection
float MouseWeightSensitivityY    = 400.0f;

// Weight shift (Android stick fallback)
float AndroidWeightDeadzone      = 0.1f;

// Weight shift (Android tilt — new)
UENUM() enum class EAndroidWeightInputSource : uint8 { Tilt, Stick };
EAndroidWeightInputSource AndroidWeightInputSource = EAndroidWeightInputSource::Tilt;
float TiltPitchDegreesForFullDeflection = 18.0f;
float TiltRollDegreesForFullDeflection  = 18.0f;
float TiltAngleDeadzoneDegrees          = 2.0f;
bool  bInvertTiltPitch                  = false;
bool  bInvertTiltRoll                   = false;
```

Internal state:
- `FVector2D CurrentWeightOffset` in [-1, +1] per axis (0 = centered).
- `bool bWeightInputActive` — true while Android right stick is past deadzone, PC `IA_WeightGate` is held, or Android tilt mode is enabled and calibrated.
- `bool bTiltCalibrated` — false until the handoff snapshot is taken.
- `FVector NeutralGravity` — gravity vector captured at handoff; used as the zero reference for subsequent tilt readings.
- On stick/mouse release, `CurrentWeightOffset` interpolates toward zero over `WeightAutoCenterDuration`. **Tilt mode bypasses this**; the sensor reading is the source of truth.

Each tick:
1. If Android + Tilt source + calibrated: read live gravity via `GetInputMotionState`, decompose pitch/roll delta from `NeutralGravity`, apply deadzone, scale by per-axis degrees-for-full-deflection, clamp to [-1, +1], write to `CurrentWeightOffset`.
2. Else if `bWeightInputActive` (stick or mouse): update `CurrentWeightOffset` from the latest input sample.
3. Else interpolate `CurrentWeightOffset` toward zero.
4. Clamp each axis to [-1, +1].
5. Write to the referenced `AWeightDistribution`:
   - `amountToTheRight = 0.5 + CurrentWeightOffset.X * 0.5`
   - `amountInFront    = 0.5 + CurrentWeightOffset.Y * 0.5`

Calibration:
- On the tick `bPlayerControlsEnabled` transitions false → true (the "SURF!" handoff in `UpdatePlayerControlState`), if Android + Tilt source: read current gravity vector, store as `NeutralGravity`, set `bTiltCalibrated = true`. Log the captured vector at `Display` level so a posture issue can be diagnosed from logs.
- No mid-ride recenter in v1.

Input handlers:
- `PaddleForward(const FInputActionValue&)` — applies forward force scaled by the float input at the surfboard's center.
- `Turn(const FInputActionValue&)` — applies sideways force at the forward offset, signed by input; no-op when input is zero.
- `WeightInput(const FInputActionValue&)` — Android stick mode: sets target offset directly from stick; PC: integrates mouse delta scaled by `MouseWeightSensitivityX/Y`. Tilt mode bypasses this handler entirely; tilt is polled in `Tick()`.
- `WeightGateStarted` / `WeightGateCompleted` — PC only. Started: hide cursor, record cursor position. Completed: restore cursor position and visibility, mark input inactive (weight begins interpolating home).

### `AWeightDistribution` reference
`ASurfboardPawn` gains an EditAnywhere UPROPERTY pointing to the level's `AWeightDistribution` actor, analogous to the existing `SurfboardActor` reference. Warn at `BeginPlay` if unset.

### Autopilots in playable levels
Autopilots write `amountInFront` / `amountToTheRight` every tick and would fight player input. They must be disabled (`enabled=false`) or absent in any level the player actually plays. This is a level/content audit, not a code change.

## Non-Functional Requirements

- **Platform detection**: reuse existing `PLATFORM_ANDROID` pattern. PC-specific code (cursor lock, RMB gate) no-ops on Android; Android-specific code (threshold on stick X) no-ops on PC.
- **Performance**: per-tick arithmetic and force application only.
- **Configurability**: all magnitudes, thresholds, deadzones, sensitivities, and durations exposed as UPROPERTY EditAnywhere.
- **Debug**: preserve existing paddle debug visualization; add an optional debug draw for current weight offset (e.g., a small 2D indicator in world space near the board).

## Acceptance Criteria

### AC1: Analog paddle (Android)
Left stick Y at 0.3 → ~30% of `PaddleForceStrength` forward. Left stick Y at 1.0 → full force.

### AC2: Binary turn
- PC A/D or Android left stick X past threshold applies fixed torque, regardless of deflection magnitude beyond threshold.
- Turning produces the same torque whether or not paddle is also held.

### AC3: Weight shift range
- Right stick fully forward → `amountInFront = 1.0`.
- Right stick fully back → `amountInFront = 0.0`.
- Right stick fully left → `amountToTheRight = 0.0`.
- Right stick fully right → `amountToTheRight = 1.0`.
- Diagonal produces proportional offsets on both axes.

### AC4: Weight auto-center
On stick release (Android) or RMB release (PC), weight interpolates from current offset to (0.5, 0.5) over `WeightAutoCenterDuration`.

### AC5: PC mouse weight
- Holding RMB hides and locks the cursor.
- Mouse delta accumulates into `CurrentWeightOffset`, scaled by `MouseWeightSensitivity`.
- Offset clamps to ±1.0 per axis.
- On RMB release, cursor is restored to its pre-lock screen position and weight begins auto-centering.

### AC6: Input independence
- Player can paddle and shift weight simultaneously.
- Player can turn without paddling.
- Player can paddle without turning.
- Player can shift weight without paddling or turning.

### AC7: No autopilot interference
In playable levels, `AAutoPilot::enabled` is false or no `AAutoPilot` actor is present. Player weight input is the sole driver of `amountInFront` / `amountToTheRight`.

### AC8: Android tilt — calibration at handoff
On the tick `bPlayerControlsEnabled` transitions to true, the current gravity vector is captured as `NeutralGravity` and `bTiltCalibrated` is set. The captured vector is logged.

### AC9: Android tilt — proportional response
With the phone held at neutral pose (whatever it was at handoff), `WeightDistribution->amountInFront == 0.5` and `WeightDistribution->amountToTheRight == 0.5`. Tilting forward by `TiltPitchDegreesForFullDeflection` drives `amountInFront` to 1.0 (or 0.0 if `bInvertTiltPitch`). Tilting right by `TiltRollDegreesForFullDeflection` drives `amountToTheRight` to 1.0 (or 0.0 if `bInvertTiltRoll`). Mid-range tilts produce proportional values between 0.0 and 1.0 on each axis.

### AC10: Android tilt — deadzone
Tilt magnitudes within `TiltAngleDeadzoneDegrees` of neutral on a given axis leave that axis at 0.5 on `WeightDistribution` (suppresses jitter from sensor noise).

### AC11: Android tilt — no auto-center fight
In tilt mode, the auto-center interpolation in Tick is bypassed. `amountInFront` and `amountToTheRight` are written from the live sensor reading each tick, with no interpolation toward 0.5 on release (there is no "release" — the player simply returns the phone to neutral).

### AC12: A/B source toggle
Setting `AndroidWeightInputSource = Stick` restores the v1 behavior (right stick drives `amountInFront` / `amountToTheRight`, with auto-center toward 0.5 on release; no tilt polling). Setting it to `Tilt` (default) activates the tilt path and ignores the right stick.

## Open Questions / Future Iterations

1. Analog paddle on PC (Shift+W = hard, W = slow) — deferred until binary default is tested.
2. HUD indicator for current weight offset — planned for next iteration.
3. Auto-center vs. stay-put on release — revisit after playtesting (stick/mouse modes only; not applicable to tilt).
4. Turn implementation: torque impulse (`AddTorqueInRadians`) vs. off-center linear force. Start with the latter to stay close to current behavior, tune from there.
5. Should turn force scale with forward speed? Out of scope here.
6. Should mouse sensitivity have separate horizontal/vertical tuning?
7. **Tilt sensitivity defaults** — 18° for full deflection on both axes is a guess. Will be retuned after first playtest.
8. **Tilt axis signs** — `bInvertTiltPitch` / `bInvertTiltRoll` defaults are guesses too; the empirically correct signs will be locked in after first playtest. Logging raw gravity values on first frame post-handoff will make this fast to diagnose.
9. **Tilt axis decomposition** — initial landscape implementation swapped pitch and roll because the seed-picker chose device-frame +Y (screen long axis) as "forward" instead of +Z (screen normal). Resolved by [tilt-axis-swap-bug.md](tilt-axis-swap-bug.md): hardcode seed = +Z.
10. **Tilt recenter button** — not in v1. If posture drift during a ride turns out to be annoying, add a tap-to-recenter button (could reuse the camera-toggle button as a long-press, or add a dedicated on-screen button).
11. **Motion controls enablement on Android** — `APlayerController` may need motion explicitly enabled for `GetInputMotionState` to return live data. To be verified during implementation; if so, enable in `BeginPlay`.

## Implementation Phases

- **Phase 1** — Split `IA_Paddle` into `IA_PaddleForward` and `IA_Turn`. New handlers. Visually similar behavior to today. ✅
- **Phase 2** — Add `IA_Weight` + Android right-stick binding. Drive `AWeightDistribution`. Audit playable levels for enabled autopilots. ✅
- **Phase 3** — PC mouse gate: RMB lock/unlock + mouse-delta integration + cursor restore. ✅
- **Phase 4** — Auto-center interpolation. Tune defaults (force magnitudes, sensitivities, durations). ✅
- **Phase 5** — Playtest and iterate. Result: right-stick weight on Android is too imprecise; right-mouse-on-PC equally awkward. Motivates Phase 6.
- **Phase 6** — Android device tilt as weight input (this iteration):
  - Add `AndroidWeightInputSource` UPROPERTY (Tilt default, Stick fallback) + tilt-related UPROPERTYs.
  - Verify/enable motion controls on `APlayerController` in `BeginPlay`.
  - On handoff (`bPlayerControlsEnabled` false → true): snapshot `NeutralGravity`, set `bTiltCalibrated`, log captured vector.
  - In `Tick` (Android + Tilt + calibrated): read gravity, decompose to pitch/roll delta, deadzone + scale + clamp, write `CurrentWeightOffset`. Skip auto-center in this mode.
  - Add a debug log of raw gravity decomposition gated on `surf.debug.flags tilt` so axis signs and sensitivity can be tuned from log output without an editor session.
  - Playtest on device. Lock axis signs and sensitivity defaults. Update this spec with the chosen defaults.

## Related Files
- [SurfboardPawn.h](../Source/GoneSurfing/SurfboardPawn.h) / [SurfboardPawn.cpp](../Source/GoneSurfing/SurfboardPawn.cpp)
- [WeightDistribution.h](../Source/GoneSurfing/WeightDistribution.h) / [WeightDistribution.cpp](../Source/GoneSurfing/WeightDistribution.cpp)
- [AutoPilot.h](../Source/GoneSurfing/AutoPilot.h) / [AutoPilot.cpp](../Source/GoneSurfing/AutoPilot.cpp)
- [fpv-camera-and-paddling.md](fpv-camera-and-paddling.md) — superseded for paddle semantics; camera portion still applies.
- [camera_system_spec.md](../Docs/camera_system_spec.md) — camera behavior unchanged.
