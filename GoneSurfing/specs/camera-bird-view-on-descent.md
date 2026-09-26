# Spec: Behind-camera bird-view blend when aiming down the wave

## Overview

The Behind (chase) camera follows the board's yaw, which gives intuitive left/right control —
tilting the board to carve reads naturally because the camera is over the board's shoulder. But
when the board points **down the wave** and drops down the slope, the wave peak rises between the
camera and the board and **occludes the board**.

Switching to the Beside camera fixes occlusion but breaks control intuition (with the camera off to
the side, left/right tilt no longer maps to on-screen left/right). So we keep the Behind camera and
instead **raise it into a bird-view** only while the board is aimed down the wave, blending smoothly
back to the low chase view when the board trims down the line.

## Objective

Eliminate wave-peak occlusion of the board while descending the slope, without leaving the Behind
camera mode and without moving the camera during ordinary pumping.

## Problem / History

- Attempt 1: keep Behind camera always low → board hidden behind the wave peak on descent.
- Attempt 2: force Beside camera always on → control becomes unintuitive (tilt no longer maps to
  screen-space left/right).
- Rejected driver: blend on board **nose-down pitch**. Pumping is a transient pitch oscillation, so a
  pitch-driven blend bounces the camera up and down on every pump.

## Requirements

**FR1** — While in Behind mode, the camera offset/pitch blends between a *chase* config (low, over the
board, for down-the-line trim) and a *bird* config (high, further back, looking down) based on a
selectable trigger signal (`BirdTriggerSource`).

**FR2** — The trigger must measure the *actual problem* — the board being hidden by the wave between
it and the camera — not a proxy at the board. The default **wave-occlusion** source does a line-of-sight
test: it samples the water surface height at points along the chase-camera→board segment and reports the
max height any crest rises above that sightline. Slope/water-above (sampled only AT the board) and the
legacy heading dot product remain as cheaper proxies but miss a crest one metre back. None of them
twitch with board pitch (no pumping response).

**FR3** — A missing signal (no `SharedCalculations`/`AWaveHeight`, no occlusion) must resolve to chase.

**NFR1** — No camera popping: the blend rides the existing position/rotation smoothing.

**NFR2** — All endpoints and thresholds are editor-tunable (live).

## Implementation

`ASurfboardPawn::UpdateCameraTransform` ([SurfboardPawn.cpp](../Source/GoneSurfing/SurfboardPawn.cpp)),
inside the `bUseBehindCamera` branch:

```
// Pick the trigger signal:
switch BirdTriggerSource:
  WaveOcclusion:      // line of sight from the CHASE camera to a rider-height point above the board
    chaseCam   = SurfboardLocation + YawOnly(SurfboardYaw).Rotate(CameraOffset)  // chase pos avoids feedback
    boardTarget = SurfboardLocation + (0,0,OcclusionTargetHeight)               // aim at the rider, not the deck
    for i in 1..OcclusionSampleCount-1:
      P = Lerp(boardTarget, chaseCam, i/N)               // point along the sightline
      waterZ = waveVelocity.calculateWaveLocationAndNormalAuto(P)[0].Z
      // The sampled surface is only the FLOOR of the occluder. The breaking lip (animated mesh) and
      // white water (particles) sit on top of it and aren't in the height data — and only where the
      // wave towers over the rider. Add a virtual lip that fades in with crest-height above the board:
      lip = OcclusionLipHeight * smoothstep(0, OcclusionLipWaterHeight, waterZ - SurfboardLocation.Z)
      occlusion = max(occlusion, (waterZ + lip) - P.Z)   // occluder above the sightline (cm)
    trigger = occlusion
  WaveSlopeSteepness: trigger = SharedCalculationsForCamera.boardWideSlopeSin       // 0 flat .. ~1 vertical
  WaterColumnAbove:   trigger = SharedCalculationsForCamera.boardWideWaterColumnAbove // cm of water above board
  HeadingAlignment:   trigger = FRotator(0, SurfboardYaw+90, 0).Vector() · normalize(WaveDownhillWorldDir.XY0)
// (wave sources read 0 when SharedCalculationsForCamera / its AWaveHeight is unset -> stays chase)

// Low-pass the raw trigger first — hysteresis alone only rejects noise smaller than the deadband,
// but white water spikes the signal across BOTH thresholds. Smoothing rejects transient spikes:
smoothed = FInterpTo(smoothed, trigger, dt, BirdTriggerSmoothingSpeed)

// Hysteresis (Schmitt trigger) over the two thresholds on the SMOOTHED value, NOT a ramp:
if smoothed >= BirdTriggerHigh:  latched = true   (commit to bird)
if smoothed <= BirdTriggerLow:   latched = false  (return to chase)
TargetBirdBlend = latched ? 1 : 0
TargetBirdBlend = override by BehindCameraConfig (ForceChase -> 0, ForceBird -> 1)
BirdBlend     = FInterpTo(BirdBlend, TargetBirdBlend, dt, BirdBlendSpeed)  // smooth ease between endpoints
TargetOffset  = Lerp(CameraOffset,          CameraOffsetBird, BirdBlend)         // position
TargetPitch   = Lerp(CameraRotationOffset.Pitch, CameraRotationBird.Pitch, ...)  // attitude
TargetRoll    = Lerp(CameraRotationOffset.Roll,  CameraRotationBird.Roll,  ...)
TargetYaw     = SmoothBehindYaw + 90 + Lerp(CameraRotationOffset.Yaw, CameraRotationBird.Yaw, ...)
```

The `+ 90` matches the camera's existing look-yaw convention (the surfboard mesh is rotated 90°, so
`board.forwards` is local +Y); the config's `Yaw` is an extra offset *on top* of the board-following
yaw. `TargetOffset` is then rotated by yaw only and applied exactly as before, and the assembled
`FRotator(TargetPitch, TargetYaw, TargetRoll)` flows through the existing `VInterpTo`/`RInterpTo`
smoothing.

### Tunables (UPROPERTY, `Camera|Position 1 - Behind`)

| Property | Default | Meaning |
|---|---|---|
| `BehindCameraConfig` | `AutoBlend` | `AutoBlend` blends by heading; `ForceChase`/`ForceBird` lock one endpoint for tuning |
| `CameraOffset` | `(0,0,180)` | chase offset (down the line); `-Y` = behind, `+Z` = up |
| `CameraRotationOffset` | `(-30,0,0)` | chase rotation offset (Pitch/Yaw/Roll) vs the board-following look |
| `CameraOffsetBird` | `(0,-400,450)` | bird offset (down the wave) |
| `CameraRotationBird` | `(-55,0,0)` | bird rotation offset (Pitch/Yaw/Roll) vs the board-following look |
| `BirdTriggerSource` | `WaveOcclusion` | what drives chase->bird: occlusion / slope / water-above / heading |
| `SharedCalculationsForCamera` | *(unset)* | SC actor to read the wave from; required for all wave sources |
| `OcclusionSampleCount` | `8` | points sampled along camera->board for the occlusion source |
| `OcclusionTargetHeight` | `150` | cm above the board the sightline aims at (rider/sail point to keep visible) |
| `OcclusionLipHeight` | `0` | virtual cm added atop the water for the unmodeled breaking lip / white water (0 = off) |
| `OcclusionLipWaterHeight` | `200` | crest-above-board (cm) at which the virtual lip reaches full height |
| `WaveDownhillWorldDir` | `(1,0,0)` | fall-line direction, only for the Heading source |
| `BirdTriggerLow` | `20` | hysteresis LOW (source units; cm for occlusion): drop back to chase below this |
| `BirdTriggerHigh` | `50` | hysteresis HIGH (source units; cm for occlusion): commit to bird above this (gap = deadband) |
| `BirdTriggerSmoothingSpeed` | `2.0` | low-pass speed on the trigger before hysteresis (lower = rejects more white-water spikes) |
| `BirdBlendSpeed` | `3.0` | how fast the camera eases chase<->bird once the latch flips |
| `bDebugBirdTrigger` | `false` | per-tick `BirdCam:` log line (value/thresholds/latch/blend) for tuning |

### Tuning workflow

Each endpoint is positioned by locking to it, not by guessing numbers: set `BehindCameraConfig` to
`ForceChase`, play, and adjust `CameraOffset`/`CameraRotationOffset` until the down-the-line view looks
right; then switch to `ForceBird` and adjust `CameraOffsetBird`/`CameraRotationBird` for the descent
view. Return to `AutoBlend` for normal play. (Properties are `BlueprintReadWrite`, so they can be
edited live in the Details panel during PIE.)

Tuning the **trigger thresholds**: assign `SharedCalculationsForCamera`, enable `bDebugBirdTrigger`,
and watch the `BirdCam:` log lines (in `GoneSurfing.log` / console) while surfing. Note the value when
down the slope vs trimming the line,
then set `BirdTriggerHigh` just below the down-the-slope value and `BirdTriggerLow` above the
trimming value, leaving a deadband between them.

### Trigger source

Default is `WaveOcclusion` — the only source that measures the actual occlusion (wave crest between the
chase camera and the board rising above the line of sight), in cm above the sightline. It reads the
`AWaveHeight` via `SharedCalculationsForCamera.waveVelocity` and samples `OcclusionSampleCount` points
along the camera→board segment. Tune `BirdTriggerHigh` to ~the cm of crest-above-sightline at which the
board becomes hidden, and `BirdTriggerLow` a bit under it.

The sampled water surface omits the breaking **lip** (animated mesh) and **white water** (particles),
which occlude the board but only where the wave is breaking/towering. `OcclusionLipHeight` adds a virtual
occluder on top of the water, faded in by `OcclusionLipWaterHeight` (crest height above the board) so it
only counts where the wave is high — letting bird engage before the bare water height alone would.

The cheaper proxies remain selectable but were each rejected in practice: `WaveSlopeSteepness`
(`boardWideSlopeSin`) and `WaterColumnAbove` (`boardWideWaterColumnAbove`) only sample AT the board and
miss a crest set back from it; `HeadingAlignment` (fall-line dot product) flips on every heading swing
while carving.

## Acceptance Criteria

- **Given** the board trims down the line, **then** the camera sits in the low chase view and
  left/right tilt maps to on-screen left/right.
- **Given** the board turns to point down the wave and drops down the slope, **then** the camera
  rises to the bird view and the board stays visible above the wave peak.
- **Given** the board pumps (rhythmic nose-down/up) while trimming down the line, **then** the camera
  does **not** rise or bob.
- **Given** flat water, **then** the camera stays in the chase view.

## Tuning notes

- Verify the sign of `CameraOffsetBird.Y` in-editor: `-Y` should move the camera **behind** the board.
  If it moves in front, flip the sign (the local-frame mapping depends on the 90° mesh rotation).
- Set `WaveDownhillWorldDir` to the actual world direction the wave breaks toward; the blend keys off
  its horizontal component only.
- Widen `[BirdBlendAlignStart, BirdBlendAlignFull]` for a lazier transition, narrow it for a snappier
  one. Raise `BirdBlendAlignStart` if the camera lifts too eagerly on shallow angles.

## Status

- [x] Tunables added to `ASurfboardPawn` (offsets, full-rotation offsets, `BehindCameraConfig` selector)
- [x] Heading-aligned blend implemented in `UpdateCameraTransform`
- [x] Force-endpoint modes for live in-editor positioning of each config
- [ ] Defaults tuned in-editor (offset sign, fall-line direction, thresholds)
- [ ] Validated on a descent + pumping playthrough
