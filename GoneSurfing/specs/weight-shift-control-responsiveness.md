# Spec: Weight-shift control responsiveness (tilt mapping + torque in C++)

> **STATUS (2026-07-16): implemented (C++), pending one manual BP edit.** Diagnoses "the board
> is slow / doesn't react quick enough" on the phone, and moves the weight-shift force application
> out of `WeightDistributionBP` into C++ while exposing the two throttling knobs as live HUD tunables.
> The BP tick nodes must still be deleted by hand (see [Manual step](#manual-step)).

## Overview

Player weight-shift (phone tilt → roll/pitch of the board) felt sluggish. The cause was **not** damping
or restoring forces — it was the **drive side** being throttled twice before it reached the physics:

1. the **tilt→weight mapping** was set to require a 45° phone tilt for full deflection, and
2. the **weight torque magnitude** was a hardcoded value with no runtime tuning hook.

This spec records the diagnosis and the fix: apply the weight torque in C++, default the tilt mapping to
18°, and make both `tilt_for_full` and `torqueMagnitude` live-tunable via `USurfTuningSubsystem`.

## Diagnosis

Measured from device input trace `phone-2026-07-16-10-22-58.csv` (replayed through
[input-trace-replay.md](input-trace-replay.md)):

- The trace header recorded **`tilt_pitch_for_full=45`, `tilt_roll_for_full=45`** — i.e. the on-device
  pawn instance overrode the C++ default (18°) up to 45°.
- Turning is almost all **roll**. Over the session, roll tilt averaged **17.3°** (peak 40.9°), which at
  45°-for-full maps to only **~39%** commanded deflection (peak 91%). Pitch averaged 5.5° → ~12%.
- So the strongest signal in the pipeline was being cut to ~a third before the torque calc even ran.

The restoring/damping machinery was ruled out as the primary cause:

- The heavy terms (pitch-align coefficient `10`, directional `AngularDampingYAwayExtra = 0.8`) act on the
  **pitch** axis (fed from `waveRelativePitchSin`, contact-gated) — the axis the player barely uses.
  See [surface-relative-pitch-damping.md](surface-relative-pitch-damping.md).
- The **roll** axis (the turn input) is left deliberately free: rail lift is roll-decoupled, roll damping
  is only `AngularDampingX = 0.1`. Yaw damping is `0.02` and *reduced* further when weight is back.
- Note the physics-body local frame is rotated (`SurfboardUtils.cpp`: *"X is forwards when I debug this"*),
  so engine **X = roll, Y = pitch, Z = yaw** — this is why the "Y" away-extra is a pitch term.

## Changes

### FR1 — Weight torque applied in C++
`AWeightDistribution::Tick` now applies the angular impulse itself, replicating the old BP graph exactly:
`calculateWeightTorque()` → `AddAngularImpulseInRadians(torque, None, bVelChange=true)` on the surfboard's
root primitive, every tick. Keeps the whole weight pipeline in one place so it can't be silently unwired
by a BP edit. (`WeightDistribution.cpp`)

### FR2 — Tilt-for-full tunable, default 18°
`TiltPitchDegreesForFullDeflection` / `TiltRollDegreesForFullDeflection` added to `USurfTuningSubsystem`
(category `Tuning|Tilt`, default `25.0` — down from the 45° that shipped on-device; 18° was tried first
and felt too twitchy). `ASurfboardPawn::GetEffectiveTilt{Pitch,Roll}ForFull()` prefer the
subsystem value over the pawn UPROPERTY, so the tunable **overrides the stale 45° per-instance override** with
no map edit. Used by the tilt input path (`UpdateTiltWeight`), the trace-recorder header, and the replay
autopilot (`InputReplayAutoPilot::OffsetFromTilt`).

### FR3 — Weight torque magnitude tunable
`WeightTorqueMagnitude` added to `USurfTuningSubsystem` (category `Tuning|Weight`, default `2000.0`).
`calculateWeightTorque` reads the subsystem value, falling back to the actor UPROPERTY only when the
subsystem is unavailable.

### NFR
- Both tunables auto-register in the runtime HUD (reflection over the subsystem's float UPROPERTYs, grouped by
  the `kCategoryTable` in `SurfTuningSubsystem.cpp`) and persist to `Saved/TuningOverrides.json`. See
  [runtime-tuning.md](runtime-tuning.md).
- New category-table entries are **required** — property metadata is stripped in non-editor (Android) builds.

## Manual step

`WeightDistributionBP`'s Event Graph still contains the `Event Tick → calculateWeightTorque →
AddAngularImpulseInRadians (bVelChange=true)` chain. **Delete those four nodes** — otherwise the impulse is
applied twice (once from BP, once from C++). This is a `.uasset` edit and must be done in-editor.

## Tuning knobs (dial on-device, in priority order)

1. `Tuning|Tilt → TiltRollDegreesForFullDeflection` — physical tilt for full weight shift. 25° default; lower
   toward ~18° for a twitchier feel, or raise toward the old 45° to calm it down.
2. `Tuning|Weight → WeightTorqueMagnitude` — turn authority per unit of weight shift. 2000 default; raise if
   the board banks too slowly even at full deflection.

## Acceptance criteria

- **Given** the BP nodes are removed, **when** playing, **then** the board still rolls/pitches from weight
  shift (torque comes from C++), with no double-strength impulse.
- **Given** `tilt_for_full` is 25°, **when** the player tilts the phone ~17° to turn, **then** commanded roll
  deflection is ~65% (was ~39% at 45°).
- **Given** either tunable is changed in the HUD, **when** the value updates, **then** turn feel changes live
  and the value survives a relaunch (persisted to `TuningOverrides.json`).
- **Given** a fresh input trace is recorded, **then** its `tilt_*_for_full` header reflects the effective
  (tunable) value, not a stale instance override.

## Status

- [x] FR1 — weight torque applied in C++
- [x] FR2 — tilt-for-full tunable, default 18°
- [x] FR3 — torque magnitude tunable
- [x] Compiles (`GoneSurfingEditor Win64 Development`)
- [ ] Manual: delete `WeightDistributionBP` tick nodes
- [ ] On-device re-tune of the two knobs to taste
