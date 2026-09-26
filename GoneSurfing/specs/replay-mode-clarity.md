# Spec: Replay-mode clarity (LIVE vs REPLAY legibility)

## Problem
The on-device ride replay (`specs/on-device-ride-replay.md`) is visually
indistinguishable from live surfing — the player taps Replay and the screen looks
identical, so it isn't obvious the game switched modes. Two follow-on issues:

1. **No mode signal.** Nothing on screen says "this is a replay."
2. **The end looks like a bug.** Playback holds on the final frame with no cue, so
   the freeze reads as a crash/hang rather than "the replay finished."
3. **Radar is wrong context.** The live wave radar keeps drawing/updating during
   replay, which is peripheral-awareness UI for an *interactive* ride.

(Muting the water audio during replay — commit `03b3535cb` — was the first step in
this direction and validated the approach.)

## Solution
A **pure-C++ Slate overlay** (`ReplayOverlayHUD`, mirroring `WaveRadarHUD` /
`SurfTuningHUD` — no UMG asset, no Blueprint edits) shown only while `bReplayActive`:

- **Letterbox bars** (top + bottom) — the primary, instantly-readable "cinematic /
  replay" signal.
- **`● REPLAY` badge** (top-left) with a softly pulsing red dot.
- **Playback progress bar + `m:ss / m:ss` timer** (bottom) — shows how far along the
  replay is, and makes the approaching end visible.
- **`REPLAY ENDED` end-card** — shown when `bReplayHolding`: dims the scene and
  centers "REPLAY ENDED", so the freeze is clearly intentional. No action-hint line:
  the ride list that opens over the end-card is the actual set of choices (the old
  "Replay to watch again • Restart for a new ride" hint was removed 2026-09-12 as stale).

Plus two pawn-side changes:

- **Hide the radar on replay enter** (`WaveRadar::Uninstall`); replay is terminal
  (only Restart, via level reload, exits it) so it is not restored.
- **Quick fade-in from black** on the LIVE→REPLAY seam (and on re-tap "watch again"),
  reusing the same `PlayerCameraManager->StartCameraFade` the Restart flow uses.

The overlay is **HitTestInvisible**, so it never blocks the on-screen touch controls
(Replay / Restart / camera) beneath it, regardless of stacking order.

## Files
- Add: `Source/GoneSurfing/ReplayOverlayHUD.h` / `.cpp` — the Slate overlay
  (`ReplayOverlay::Install/Uninstall/UpdateData`, `FReplayOverlaySnapshot`).
- Modify: `Source/GoneSurfing/SurfboardPawn.cpp` / `.h`
  - `EnterReplayMode()` — uninstall radar, install overlay, fade.
  - `ReplayLastRide()` re-entrant path — fade on "watch again".
  - `Tick()` replay branch — `UpdateReplayOverlay()` in place of `UpdateWaveRadar()`.
  - `EndPlay()` — uninstall overlay.
  - New helpers `UpdateReplayOverlay()`, `PlayReplayTransitionFade()`.

## Design chosen (2026-08-07)
Player picked: **full cinematic** treatment, **end-card + freeze** at the end (not
auto-loop or auto-restart), **quick fade** on the transition.

## Tunables / notes
- Letterbox bar height = `clamp(ScreenH * 0.065, 24, 150)` px. If the bottom bar ever
  visually clips the on-screen buttons, thin it or drop the bottom bar (touches still
  pass through — the overlay is HitTestInvisible; this is a *visual* concern only).
- UI scale is resolution-independent (`clamp(ScreenH / 1080, 0.6, 3.0)`); no
  `SDPIScaler` (the overlay fills the viewport and scales its own metrics).

## Out of scope / follow-ons
- Dedicated orbit/cinematic replay camera (still Beside; see parent spec).
- Scrub / pause / slow-mo controls.
- Auto-loop or auto-restart end behavior (explicitly not chosen).
