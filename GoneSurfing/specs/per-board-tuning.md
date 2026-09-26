# Spec: Per-board tuning — session handoff

## Status

**Branch `tune-the-boards`, 16 commits, not pushed to any remote.** Working tree clean.
Nothing here has been validated on device — every measurement below is PC.

The original task: only the two extremes (foamie, shortboard) had ever been tuned; the middle three
were interpolations. Tuning them turned into a physics investigation, because the thing that made
boards feel different was masking a bug.

- [x] Root cause of "the board keeps speed on flat water" found and fixed (three separate causes,
      below)
- [x] Wave-normal noise traced to the exporter; **the user fixed it and re-exported** — jitter gone
- [x] Per-board glide, yaw damping and carve values set for all five boards
- [ ] **Player validation on device** — none of this has been ridden on Android
- [ ] The open decisions in "What is still open"

## The physics chain (why board tuning became this)

Each fix exposed the next problem. Read in order:

1. **`bottomHydrofoilYaw` was the board's engine.** Its forward component had no slope gate, no
   front-face gate, unsigned `absSinSlip` and no orientation term, so it drove the board on flat
   water (~99% of net forward force), behind the crest (100%), and while inverted. The coefficient
   had been raised 20× on 2026-08-21 for *handling*; the propulsion was a side effect.
   → Gated it on slope + front face, as a **floor** so a board can keep some (`408b1d0f6`).
2. **That made every remaining force slope-proportional**, which exposed a wave-slope signal that
   was 6× noisier than the wave geometry allows.
3. **The noise was in the datatable, not the game.** `export_waves_display.py` took
   `mesh_obj.ray_cast()`'s **flat face normal** off a FLIP surface that is re-meshed every frame.
   Heights were clean (`location.z` is continuous); normals were not. Spec written in the exporter
   repo; **user fixed and re-exported** (`d64391706`).
4. **Remaining surge/brake**: the bottom forward drag scaled with `effectiveWaterHeight`, which
   deliberately keeps a non-zero floor when *not* submerged — so a v² drag kept braking a board that
   had left the water (30% of the hardest braking ticks were airborne at ~1074 cm/s).
   → `forwardDragWettingGate` (`a49b76e49`).

Full detail and every measurement: **[flat-water-propulsion-audit.md](flat-water-propulsion-audit.md)**.
The exporter fix: `E:\windowsgrejor\git\GoneSurfingSimulation\scripts\SPEC_wave_normal_noise.md`.

## Current per-board values

Every board overrides exactly these eight; nothing else.

| coefficient | default | foamie | funboard | fish | hybrid | shortboard |
|---|---|---|---|---|---|---|
| `SurfboardForwardsDamping` | 0.030 | 0.015 | 0.020 | 0.024 | 0.040 | 0.048 |
| `yawFwdOffFaceFloor` | 0.000 | 0.350 | 0.250 | 0.180 | 0.120 | 0.000 |
| `AngularDampingX` | 0.400 | 0.550 | 0.475 | 0.475 | 0.438 | 0.400 |
| `AngularDampingY` | 0.400 | 0.550 | 0.475 | 0.475 | 0.438 | 0.400 |
| `AngularDampingZ` | 0.150 | 0.200 | 0.180 | 0.150 | 0.120 | 0.100 |
| `lateralTurnCoefficient` | 4000 | 2800 | 4100 | 4500 | 4750 | 5400 |
| `lateralTurnHardCarveBoost` | 0.500 | 0.300 | 0.525 | 0.525 | 0.637 | 0.750 |
| `slopeThrustCoefficient` | 40000 | 29897 | 34000 | 42207 | 38103 | 34000 |

Measured flat-water glide (one replayed trace, fixed 60 fps, `slopeSin < 0.10`):

| board | flat mean \|v\| | flat coast τ |
|---|---|---|
| foamie | 356 | 3.01 s |
| funboard | 188 | 1.95 s |
| fish | 184 | 2.53 s |
| hybrid | 133 | 1.38 s |
| shortboard | 124 | 1.15 s |

The dial lands where intended — fish and shortboard have the same *overall* mean speed (231) but
184 vs 124 on flat water, so they differ off the face without changing how they ride on it.

## What is still open

1. **The two glide dials overlap.** `SurfboardForwardsDamping` controls coasting (keeping speed);
   `yawFwdOffFaceFloor` grants *drive* off the face (gaining speed). The foamie currently gets both.
   If glide should be purely a coasting property the floors want flattening; if the foamie should
   also be forgiving about leaving the pocket, they are right as they are. **User's call, not made.**
2. **Fish and funboard still share `lateralTurnHardCarveBoost` 0.525.** They now differ on every
   other handling axis. Given the fish is "long drawn-out arcs, not tight pocket turns", lowering it
   below the funboard's may be more characterful than raising it.
3. **`slopeThrustCoefficient` is not monotonic** — shortboard and funboard are both 34000, and fish
   (42207) is the highest. Deliberate for the fish; the shortboard matching the funboard may not be.
4. **Within-group glide ordering is not trustworthy.** funboard's τ came out *below* fish's despite
   lower damping, and shortboard's τ rests on n=1. Each board rides the same input differently, so
   this is a comparison of tendencies. A controlled coast-down — every board given the identical
   stamped rails-handoff state, then left to coast with no input — would settle it and is not much
   work.
5. **Glide-through risk, accepted knowingly.** The wetting gate raised time-behind-crest 20.6% →
   35.2% and excursion duration 0.41 s → 1.01 s, with 2 excursions above 300 cm/s where legacy had
   0. Mechanism is *arrival speed*, not weakened penetration drag. Levers and caveats are in the
   auto-memory note; revisit only if the board starts punching through waves in play.
6. **Open questions carried by the audit spec**: the inverted case was never directly measured; is
   the ~3 m rideable face the intended wave shape; and is `CoordinateScale` non-uniform (if so,
   normals may need the inverse-transpose and every slope is distorted by a fixed factor — separate
   from the noise, unverified).
7. **`bShowTuningHUD` is still `true`** in `SurfboardPawn.h` — flipped on 2026-08-19 for a one-off
   check. Revert before shipping.

## Traps that cost time this session

- **The `surfing-down-the-line` autopilot rides at ~190 cm/s on the new wave data, where it did
  ~600 on the old.** Its scripted line no longer suits the wave, so it is **not currently a valid
  benchmark**. A/B against a recorded player trace instead, or retune the autopilot — it is the only
  repeatable test vehicle in the project and it is currently broken.
- **Every A/B needs `-usefixedtimestep -fps=60` and identical `surf.debug.flags` on both sides.**
  Logging volume changes frame rate, and frame rate changes the physics: the same configuration
  measured 95%/626 with `torque` logging on and 79%/507 with it off. That confound produced a wrong
  conclusion (M10 in the audit spec) that had to be retracted.
- **Replays diverge** from the ride that produced the trace — sometimes wildly (mean 294 vs 601
  cm/s). Use them for force budgets and paired A/B, never to reproduce a specific moment.
- **Overlays in `Saved/BoardTuning/<id>.json` outrank the board profiles.** Three stale ones were
  archived as `*.superseded-20260907-*.json`; rename back to restore. Anything changed in the tuning
  HUD writes a new one.
- **Never screen-capture the game with gdigrab** — it films whatever is on top and Windows silently
  refuses `SetForegroundWindow` to a background process. Use `-DUMPMOVIE -BENCHMARK -FPS=30`. See
  the `ab-video-comparison` and `record-game-video` skills.
- **`LaunchUnrealEditor.bat` needs the `1` argument** or it blocks on an interactive menu while
  `Start-Process` returns success. Verify with `Get-Process UnrealEditor`.

## Tools added this session

- `RecordVideo.ps1` (repo root) — record a clip of the running game.
- `.claude/skills/record-game-video/` — single clip, shared as an Artifact.
- `.claude/skills/ab-video-comparison/` — the before/after workflow the user called "gold": two
  clips of the same input one tunable apart, synced side by side with the measured table.
- Debug flags `wavedump` (raw datatable dump) and `tilejump` (tile/frame-offset selection);
  `tileFrame` added to the `CROSSING` line — the frame actually sampled, which is **not** `wcFrame`.
- `GoneSurfingSimulation/scripts/analyze_wave_normal_noise.py` — measures exported-normal noise
  without Blender or Unreal. **Written but never run** (no numpy on the dev box, no exports there).
