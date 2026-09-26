# Spec: Schematic wave-radar (break-awareness HUD)

## Overview

The chase camera can't show what's **behind/around** the surfer — which is exactly where the wave
breaks. A surfer reads the breaking section to decide when to speed up, slow down, or turn. This spec
adds a small **top-down schematic HUD** ("wave radar") that shows the surfer, the wave face, and the
**break line** relative to the board — driven entirely by the existing wave data, with **no second
scene render**, so it's cheap on mobile.

It is deliberately *not* a picture-in-picture (`USceneCaptureComponent2D` renders the whole scene a
second time — the heaviest option on a phone GPU). The radar draws 2D shapes from data instead.

## Objective

Give the player continuous, mobile-cheap awareness of where the wave is breaking relative to their
position and heading, enough to make speed/turn decisions.

## Requirements

### Functional

- **FR1** — Display a compact top-down map, screen-anchored (default: a corner), showing a region of
  the wave around the surfboard (configurable extent, e.g. ±1500 cm).
- **FR2** — Mark the **surfer**: position (map center) and **heading** (an arrow/triangle).
- **FR3** — Shade the **wave face** by relative height (trough → crest) so the player can read where
  the wave is, including *behind* them.
- **FR4** — Highlight the **break line / breaking cells** — where the face is steep enough to be
  breaking (white-water-ish), so the player sees the dangerous/fast section.
- **FR5** — Orientation is **heading-up** by default (board forward points up the radar, so on-screen
  left/right matches the player's left/right), with a config flag for world-fixed.
- **FR6** — Toggleable on/off at runtime; off = zero cost.

### Non-functional

- **NFR1 (mobile)** — No render targets, no scene capture. One `GetWaveDataAroundLocation` grid call
  plus 2D Slate draws per update. Update rate throttleable (default ~15 Hz) independent of framerate.
- **NFR2** — Pure C++ Slate (like `SurfTuningHUD`), no UMG assets required.
- **NFR3** — Grid resolution and extent are tunables; default kept small (e.g. 11×11 = 121 samples).

## Data sources (all already present)

- **Wave grid:** `AWaveHeight::GetWaveDataAroundLocation(center, frame, distanceBetweenPoints, gridX, gridY)`
  → `TArray<TTuple<double /*height*/, FVector /*normal*/>>`. One call yields the whole neighborhood.
  Frame comes from the WaterController (the `...Auto` variants already read it; mirror that, or add a
  `GetWaveDataAroundLocationAuto`).
- **Break detection (actual white water):** `AParticleSystemsController` reads the white-water point
  cloud (`WhiteWaterPointsDataTable`, per-frame `FWavePointsDataStruct2::Positions`) and transforms each
  point to world via its per-channel `ComponentWorldTransform`. It now caches those world points
  (`GetWhiteWaterWorldPoints()`); the radar bins them into board-local cells and marks any cell
  containing a point as breaking. This is the *real* foam footprint, not an estimate.
  - **Fallback:** if no `ParticleControllerForRadar` is assigned, the radar falls back to a steepness
    estimate — a cell is breaking when `slopeSin = sqrt(1 - normal.Z^2) > BreakSlopeThreshold`.
- **Surfer transform:** `SurfboardLocation` / heading from `ASurfboardPawn` (the same horizontal
  forward-vector heading the camera uses, robust to pitch).
- **Wave height query reference:** reuse `ASharedCalculations::waveVelocity` (the `AWaveHeight`), the
  pawn already references SharedCalculations for the camera occlusion.

## Implementation

### Widget

A pure-C++ Slate widget `SWaveRadar` (mirrors `SurfTuningHUD`'s install pattern: a free function
`WaveRadar::Install(UWorld*)` / `Uninstall(UWorld*)` called from `ASurfboardPawn::BeginPlay`/`EndPlay`).
It is a separate always-on overlay, NOT part of the tuning HUD (which pauses the game when open).

The widget holds a pointer to the data provider (the pawn or a small struct it pushes each tick) and
paints in `OnPaint` using `FSlateDrawElement` primitives (boxes/lines/triangles) — no textures needed.

### Per-update (throttled to RadarUpdateHz)

```
center = SurfboardLocation
grid   = WaveHeight.GetWaveDataAroundLocation(center, frame, CellSpacing, GridN, GridN)
minH/maxH = range of grid heights         // for the height shading
for each cell:
    heightT  = (h - minH) / (maxH - minH) // 0..1 for color ramp
    slopeSin = sqrt(1 - normal.Z^2)
    breaking = slopeSin > BreakSlopeThreshold
store a compact GridN×GridN of {heightT, breaking} for OnPaint
```

`OnPaint` maps grid (x,y) → radar pixels. For heading-up, rotate the grid offset by `-boardHeadingYaw`
before mapping (so forward is up). Draw order: face cells (height ramp) → breaking cells (foam color)
→ surfer triangle at center pointing up. Optional: a wave-flow arrow from `calculateWaveVelocityAuto`.

### Tunables (UPROPERTY on the pawn or a `Tuning|Radar` block)

| Property | Default | Meaning |
|---|---|---|
| `bShowWaveRadar` | `true` | master on/off (off = no cost) |
| `RadarScreenAnchor` / `RadarSize` | corner / ~200px | placement & size |
| `RadarExtentCm` | `1500` | half-extent of the sampled region around the surfer |
| `RadarGridN` | `11` | samples per side (N×N total) |
| `RadarCellSpacingCm` | derived (`2·Extent/(N-1)`) | spacing passed to `GetWaveDataAroundLocation` |
| `RadarUpdateHz` | `15` | data refresh rate (paint can be every frame) |
| `ParticleControllerForRadar` | *(unset)* | the `AParticleSystemsController`; when set, break cells = actual white-water points |
| `BreakSlopeThreshold` | `0.6` | fallback only (no controller): slopeSin above which a cell is breaking |
| `bRadarHeadingUp` | `false` | heading-up (radar rotates) vs world-fixed (triangle rotates — steadier) |
| `RadarWorldYawOffset` | `90` | world-fixed only: world yaw mapped to radar "up" (each +90 = quarter turn) |

## Acceptance Criteria

- **Given** the surfer is riding, **then** the radar shows the surfer centered with a heading arrow and
  the wave face shaded around them, including the area **behind** the surfer.
- **Given** a section of the wave is breaking behind/beside the surfer, **then** those cells render in
  the break color, and they track the break as it moves.
- **Given** the board turns, **then** (heading-up mode) the wave/break rotate around the fixed surfer
  marker so on-screen left/right matches the player's left/right.
- **Given** a low-end mobile device, **then** enabling the radar costs one grid sample (~121 lookups)
  at `RadarUpdateHz` plus 2D draws — no measurable GPU scene-render cost, no render target.
- **Given** `bShowWaveRadar = false`, **then** nothing is sampled, drawn, or ticked.

## Test cases

- Flat water → all cells same shade, no breaking cells, arrow only.
- Steep/breaking face behind the surfer → break cells appear at the bottom of a heading-up radar.
- Spin the board 360° → break cells orbit the center smoothly (no wrap glitch in the heading rotation;
  reuse the pitch-robust horizontal-forward heading, not Euler yaw).
- Toggle `bShowWaveRadar` off/on → widget hides/shows, profiler shows the per-tick sampling gated out.

## Open questions

- **Break signal fidelity:** RESOLVED — radar now bins the actual white-water world points cached by
  `AParticleSystemsController`, with the slope estimate kept as a no-controller fallback.
- **Orientation default:** heading-up (chosen) vs world-fixed. Heading-up matches the chase camera's
  frame; if players find the rotating wave disorienting, expose `bRadarHeadingUp=false`.
- **Frame source:** add a `GetWaveDataAroundLocationAuto` (reads WaterController frame) to avoid passing
  the frame in, consistent with the other `...Auto` queries.

## Status

**Disabled by default as of 2026-09-09** (`surf.hud.radar 0`). The camera moved further back, and the
wave face and break are now readable in the scene itself — which is what the radar existed to convey.
Reading it also meant looking away from the wave mid-ride, so it cost screen space and attention for
information the player now gets for free. The implementation is intact and the CVar turns it back on
live (`surf.hud.radar 1`); revisit if the camera moves back in.

- [ ] `SWaveRadar` Slate widget + `WaveRadar::Install/Uninstall`
- [ ] Per-tick grid sample + break classification (throttled)
- [ ] `OnPaint` schematic (face shading, break cells, surfer arrow, heading-up rotation)
- [ ] Tunables wired
- [ ] Profiled on target mobile device
