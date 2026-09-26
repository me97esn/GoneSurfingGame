# Spec: On-Device Ride Replay (kinematic playback)

## Overview
Add an in-game **"Replay"** button beside the existing Restart button that plays
back the player's most recent ride on the phone. Because the input-trace recorder
already stores the board's world position, velocity and rotation every tick, the
replay is a **kinematic playback**: physics is switched off on the board and its
transform is driven directly from the recorded trajectory. This reproduces the
ride *exactly* (deterministic), and the existing camera rig follows the board for
free.

The Beside camera is used for v1; a dedicated replay camera is a follow-on.

## How this differs from `input-trace-replay.md` (important)
There is already an `AInputReplayAutoPilot` (see [input-trace-replay.md](input-trace-replay.md)).
It solves a **different** problem and must not be confused with this feature:

| | `input-trace-replay.md` (existing) | this spec (new) |
|---|---|---|
| Purpose | PC-side **force tuning** | On-device **"watch my last ride"** |
| Reads | `tilt/stick` **input** columns | `board_*` **trajectory** columns |
| Method | re-feeds input through **live physics** | **kinematic** transform playback |
| Fidelity | deliberately **diverges** run-to-run | **exact** reproduction |
| Runs on | editor only (`#if !WITH_EDITOR` disables it) | **on device** (packaged Android) |

The existing actor's CSV parsing, path resolution and time-cursor/lerp are useful
*reference*, but this feature is a new on-device path that reads the trajectory
columns the tuning replay ignores.

## Objective
Let a player review the run they just did, on the phone, with one tap — no PC, no
ADB, no rebuild. The value is qualitative feedback ("what did that carve actually
look like from the side?").

## Background — the pieces this builds on
- **Recorder** — `ASurfboardPawn` already records per-tick CSVs to
  `Saved/InputTraces/<prefix>-<timestamp>.csv` when `bRecordInputTrace=true`
  (already enabled in the playable level). Columns today:
  `t, tilt_pitch_deg, tilt_roll_deg, stick_x, stick_y, source, board_x, board_y,
  board_z, board_vx, board_vy, board_vz, board_roll, board_pitch, board_yaw`.
  Recording starts at the autopilot→player handoff (`OnPlayerControlsEnabled`), so
  `t=0` is the moment player control goes live (post pop-up).
- **Board** — `SurfboardActor` (an `AStaticMeshActor`) is a *separate* actor the
  pawn references. The pawn's `CameraRoot`/`CameraComponent` follow its transform
  every tick in `UpdateCameraTransform`. The ~20 `AFluidDynamics` actors apply
  impulses to its mesh. Impulses on a non-simulating (kinematic) body are no-ops,
  so `SetSimulatePhysics(false)` cleanly neutralizes the physics without touching
  the fluid actors.
- **Camera** — `bUseBehindCamera` picks Behind (Position 1) vs Beside (Position 2);
  `SetCameraBehind(bool)` / `ToggleCameraMode()` are `BlueprintCallable`. During
  the intro autopilot the camera is *already* Beside; it flips to Behind at handoff.
- **UI** — `WBP_SurfboardControls` (the on-screen touch HUD, spawned from the
  `Surfing_infinite_wave` level Blueprint) embeds `WBP_RestartButton` and a
  `CameraToggleButton`, each doing `GetPlayerPawn → Cast SurfboardPawn →
  RestartLevel / ToggleCameraMode`.
- **Wave clock** — the wave surface (mesh tiles *and* physics query data) is a pure
  deterministic function of a single int `CurrentFrame` on the
  `WaterController-Datatable-BP` actor. C++ reads it by reflection
  (`ASharedCalculations::SC_ReadWaterControllerFrame`,
  `FindWaterController(World)`); nothing writes it today. The BP's own tick
  auto-increments `CurrentFrame` and wraps `EndFrame→StartFrame`.
  `AInfiniteWaveManager`/`AGridLODActor` tile off the camera/board **world-Y**, not
  a timer.

## Scope

### In scope
- One new CSV column, `wave_frame`, recorded per tick (see "Recording change").
- A `ReplayLastRide()` entry point on `ASurfboardPawn` and a new replay button in
  `WBP_SurfboardControls` beside Restart.
- A kinematic replay mode on the pawn: board physics off, transform driven from the
  trace, wave `CurrentFrame` re-synced, input + recording suppressed, Beside camera.
- Runs on device (packaged Android) as well as in the editor.

### Out of scope (follow-ons)
- A dedicated replay camera (orbit / cinematic). v1 reuses Beside.
- Scrub / pause / slow-mo / loop controls. v1 is play-once then hold on the last
  frame.
- Replaying a ride *other than* the latest (a picker / gallery).
- Editing, trimming or sharing traces.
- Resuming the live physics ride after a replay — replay is a terminal review
  action; the player uses Restart to start a fresh ride.

## Recording change: add `wave_frame`
Append one column, `wave_frame`, to the CSV (column index 15, *after* the existing
`board_*` block). Appending at the end keeps `AInputReplayAutoPilot`'s
index-based parser (reads cols 0..5 and 6..14) working unchanged.

- Value: `WaterController.CurrentFrame` read via the existing reflection helper at
  the same point `SampleInputTrace` reads the board transform.
- Also record the start frame in the metadata block (`# wave_start_frame=<N>`) for
  quick inspection and as a fallback.
- Old traces (no `wave_frame` column) must still replay: fall back to a free-running
  wave (or `StartFrame`) and log once that wave-sync is unavailable for that trace.

## Architecture

### Replay entry — `ASurfboardPawn::ReplayLastRide()`
`UFUNCTION(BlueprintCallable, Category="Level")`, mirroring `RestartLevel()`.

0. **Re-entrancy** — if `bReplayActive` is already true (replay running, or holding
   on the last frame), do **not** re-finalize or re-parse: just reset `ReplayTime=0`,
   `ReplayRowHint=0`, re-teleport the board to row 0 and re-assert the start
   `wave_frame`, and play from the top. This is the "watch it again" path and must be
   cheap and repeatable arbitrarily.
1. **Finalize the current trace** (first entry only) so "latest ride" == "what I just
   did": `StopInputTrace()` (final synchronous flush), then locate the newest `.csv`
   in `Saved/InputTraces/` (by filename timestamp, which is monotonic; mtime as
   tiebreak). On repeat taps recording is already stopped, so this is a no-op and the
   newest file is unchanged — the same ride replays.
2. If no trace found → no-op + log; the button may disable itself.
3. Parse `t`, `board_x/y/z`, `board_roll/pitch/yaw`, and `wave_frame` into an array
   of rows (cache them; repeat taps reuse the parsed rows). Velocity columns are not
   needed for kinematic playback.
4. Enter replay mode (below).

### Replay mode (state on the pawn)
State: `bReplayActive`, `TArray<FReplayRow> ReplayRows`, `float ReplayTime`,
`int32 ReplayRowHint`, plus cached originals to restore.

**On enter:**
- Suppress control: `bPlayerControlsEnabled=false`; ensure recorder is stopped so we
  don't record the replay itself.
- Board → kinematic: cache and set `SurfboardActor` mesh `SetSimulatePhysics(false)`;
  teleport it to row 0's transform.
- Wave → recorded start: set `WaterController.CurrentFrame = rows[0].wave_frame` via
  reflection (`FIntProperty::SetPropertyValue_InContainer`).
- Camera → Beside: `SetCameraBehind(false)`.

**Each tick (while `bReplayActive`):**
- Advance `ReplayTime`; find the row pair straddling it (using `ReplayRowHint` like
  `AInputReplayAutoPilot`).
- Position: `FMath::Lerp` the two rows' `board_x/y/z`.
- Rotation: `FQuat::Slerp` (or `FRotator` shortest-path interp) of the two rows'
  roll/pitch/yaw. Set `SurfboardActor` world transform (via the mesh, since it's
  kinematic).
- Wave: re-assert `CurrentFrame` to the nearest recorded `wave_frame` (int; nearest
  is fine). **Must beat the BP's auto-increment** — see "Wave clock ownership".
- Camera follows automatically through the pawn's normal `UpdateCameraTransform`
  (board must move *continuously* from the start so `AInfiniteWaveManager` tiling
  tracks it; the teleport-to-row-0 at enter is the only jump, done before playback).

**On end (past last row):** hold on the final frame; leave `bReplayActive=true` but
stop advancing. The board stays kinematic (physics is *not* restored — only Restart,
via level reload, does that). Re-enable the Replay + Restart buttons. Tapping Replay
again re-runs the same latest trace from the start (step 0 above); this is repeatable
arbitrarily. Tapping Restart reloads the level for a fresh ride.

### Wave clock ownership (the one ordering subtlety)
The `WaterController-Datatable-BP` tick auto-increments `CurrentFrame` every tick, so
a single write is overwritten. **Implemented approach:** the pawn writes both
`CurrentFrame` *and* a `bManualFrameControl` bool on the controller each replay tick,
by name via reflection (`SetWaveFrame`). The **only** BP change required is:
1. Add a `bManualFrameControl` bool variable (default false) to
   `WaterController-Datatable-BP`.
2. Gate the "Step currentFrame" auto-increment behind `Branch(NOT bManualFrameControl)`
   so the BP stops advancing the frame while replay drives it.

No `SetCurrentFrame` BP event is needed — C++ sets the int directly. The bool is a
plain UPROPERTY the reflection write reaches. `bManualFrameControl` is never cleared
in code because the only exit from replay is Restart, which reloads the level and
resets the BP. Until the bool is added, `SetWaveFrame` still writes `CurrentFrame`
each tick (the write simply loses the race with the BP's own step), so the wave
free-runs from the recorded start — the Phase 2 behaviour.

### UI — new Replay button
Add a button to `WBP_SurfboardControls` beside `WBP_RestartButton`, with the same
handler shape: `GetPlayerPawn → Cast SurfboardPawn → ReplayLastRide`. Reuse
`WBP_RestartButton`'s styling (new `WBP_ReplayButton` or a second instance with a
"Replay" label). Optionally grey it out until at least one trace exists.

### On-device gating
Unlike `AInputReplayAutoPilot`, this path is **not** `#if !WITH_EDITOR`-gated — it is
a player-facing feature and must run in the packaged Android build. It touches no
editor-only API on the hot path (`GetActorLabel` etc.); verify the trace-directory
resolution works under the Android sandbox path
(`.../files/UnrealGame/.../Saved/InputTraces/`), which the recorder already writes to.

## Acceptance Criteria
- **AC1** — With recording on, after a ride the newest `Saved/InputTraces/*.csv`
  contains a `wave_frame` column populated with the live `CurrentFrame` each tick,
  and a `# wave_start_frame=` metadata line.
- **AC2** — Tapping Replay after a ride switches to the Beside camera and plays the
  board through the exact recorded path; the board pose at replay time `t` matches
  the recorded pose at `t` within interpolation tolerance.
- **AC3** — The wave surface under the board during replay matches what it was during
  the ride (same `CurrentFrame` timeline); no visible drift between board and wave.
- **AC4** — No physics fights the playback: with the board kinematic, fluid-actor
  impulses have no effect; the board follows the trace, not the sim.
- **AC5** — Replaying an **old** trace with no `wave_frame` column does not crash;
  it plays the board path with a free-running wave and logs the limitation once.
- **AC6** — At trace end the board holds on the final frame; Replay and Restart are
  usable; Restart reloads the level and restores normal physics/input.
- **AC7** — Runs in a packaged Android build (no editor-only dependency on the replay
  path).
- **AC8** — Recording does not capture the replay itself (no trace-of-a-replay files).
- **AC9** — Replay is repeatable: tapping Replay again (during playback or while
  holding on the last frame), with no Restart in between, replays the **same** latest
  ride from the start. No new trace file is created and the board is not handed back
  to physics between replays.

## Implementation phases
1. **Recorder column** — add `wave_frame` (+ `# wave_start_frame=`) to
   `SampleInputTrace`/`StartInputTrace`. Verify a fresh trace has the column.
2. **Kinematic playback core** — `ReplayLastRide()` + replay state + tick playback of
   transform (board physics off, Beside camera, input suppressed). Wave left
   free-running for this phase. Confirm the board retraces the path on PC.
3. **Wave re-sync** — BP `bManualFrameControl` + `SetCurrentFrame`, pawn asserts
   `CurrentFrame` from the trace each tick. Confirm no board/wave drift.
4. **UI** — Replay button in `WBP_SurfboardControls`; wire to `ReplayLastRide`.
5. **On-device** — deploy, record a ride, replay on the phone, confirm AC2/AC3/AC7.
6. **(Follow-on)** dedicated replay camera; scrub/pause/loop; ride picker.

## Spray in replays (added 2026-08-02)

The board-spray system (specs/board-spray-particles.md) is force-driven, but during replay the
force pipeline is tick-disabled and the board is kinematic — the SprayController's inputs froze
at their pre-replay values, so replays showed either a CONSTANT stale fan or none at all
(depending on the board state at the trigger moment), and the frozen fan kept spawning forever
while holding on the last frame. Both observed on device.

Fix: **record the spray outputs, play them back verbatim** (chosen over kinematic reconstruction,
which would be a second spray model to keep matched with every future tuning change):

- **Columns 16-27** (appended last, index-compat like `wave_frame`): per site (left/right/tail)
  the ejection velocity vector + spawn rate, post gate/wetting/budget —
  `spray_vl_x/y/z, spray_rl, spray_vr_*, spray_rr, spray_vt_*, spray_rt`. Zeros in levels
  without a SprayController.
- **Playback**: `ApplyReplayAtTime` lerps the spray columns and pushes them via
  `ASprayController::SetReplaySpray` — the override substitutes recorded velocity/rate while the
  controller keeps computing emission positions, waterline, and the settle grid LIVE (all
  deterministic under the re-synced wave clock; FD actor positions ride the kinematic board).
  The SprayController is deliberately NOT in `SetForcePipelineTicking`'s disable list.
- **Hold = pause**: while holding on the last frame, zero rates are pushed — emission stops with
  playback; in-flight droplets fade out naturally.
- **Old traces** (< 28 columns): replay runs sprayless — the always-pushed zero override also
  suppresses the stale-accumulator spray that caused the original bug.
- **Columns 28-29** (`weight_front,weight_right`, added 2026-08-05): the weight amounts driving
  the surfer rider's lean/twist, pushed through `USurferAnimInstance::SetReplayWeights` each
  replay tick (the live WeightDistribution is tick-frozen, which held the rider in one constant
  lean — observed). Pre-rider traces replay a neutral (0.5/0.5) rider. The rider's board-roll
  compensation needs no recording: it reads the board transform geometrically, and the replayed
  transform IS the recording. See specs/surfer-rider-lean.md.
- Water velocity for settled-foam drift is the one frozen input left (SC tick-disabled) —
  cosmetically negligible over a foam lifetime; revisit only if visible.

## Open questions
1. **Which ride does "Replay" play?** RESOLVED: the **current** ride — on tap,
   finalize (stop+flush) the in-progress trace, then play it back. "Replay" means
   "watch what I just did", usable mid-ride.
2. **Auto-disable the button** when no trace exists yet (first launch), or let it
   no-op silently? Cosmetic.
3. **Interpolation of `wave_frame`** — nearest recorded int is proposed; if the BP
   step rate differs from the record rate, consider deriving the frame from
   `ReplayTime` against `wave_start_frame` instead. Revisit if drift appears.
4. **End-of-replay UX** — hold-on-last-frame is proposed; loop or auto-Restart are
   alternatives.

## Related files
- Modify: [Source/GoneSurfing/SurfboardPawn.h](../Source/GoneSurfing/SurfboardPawn.h) /
  [.cpp](../Source/GoneSurfing/SurfboardPawn.cpp) — `wave_frame` recording, `ReplayLastRide`, replay mode.
- Modify: `Content/Blueprints/WaterController-Datatable-BP` — `bManualFrameControl` + `SetCurrentFrame`.
- Modify: `Content/Input/WBP_SurfboardControls` (+ new `WBP_ReplayButton`).
- Reference: [Source/GoneSurfing/InputReplayAutoPilot.cpp](../Source/GoneSurfing/InputReplayAutoPilot.cpp) — CSV parse / time-cursor / path-resolution pattern.
- Reference: [Source/GoneSurfing/SharedCalculations.cpp](../Source/GoneSurfing/SharedCalculations.cpp) — `FindWaterController`, `SC_ReadWaterControllerFrame` reflection helpers.
- Related spec: [input-trace-replay.md](input-trace-replay.md) — the *other* (PC tuning) replay; do not conflate.
