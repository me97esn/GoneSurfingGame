# Spec: Trace trajectory comparison (measure PC-vs-phone divergence)

> **STATUS (2026-07-16): implemented (capture + compare tooling), awaiting first phone recording.**
> Enables measuring how differently the board behaves on PC vs phone for the *same* input, instead of
> judging it by feel.

## Overview

The board behaves noticeably differently on PC vs phone when the same input trace drives it (e.g. the phone
surfs down the line for many seconds then hard-turns into the wave; the PC drifts over the back of the wave
after a few seconds). To measure that rather than feel it, the **input trace now records the board's own
trajectory** (position, velocity, rotation) alongside the input, and a script diffs a PC replay against the
phone recording embedded in the trace.

## Motivation

Two runs of the *same* trace diverge, but "feels different" isn't actionable. With the board pose captured in
the recording, a PC replay produces a second trajectory over the same time axis, and the two can be diffed
into concrete numbers (cm of position drift, degrees of attitude drift, and *when* they part ways). The most
likely culprit is that the custom Chaos per-axis damping / velocity-ceiling integration is frame-rate /
substep sensitive, so the phone (different frame rate) and PC integrate the same input to different paths —
this measurement will confirm or refute that.

## Design

### FR1 — Board pose in the input trace
`ASurfboardPawn::SampleInputTrace` appends 9 columns after the existing input columns:
`board_x,board_y,board_z,board_vx,board_vy,board_vz,board_roll,board_pitch,board_yaw`, sampled from
`SurfboardActor->GetStaticMeshComponent()` — the **same accessors** the replay recorder
(`AInputReplayAutoPilot::RecordTrajectorySample`) uses, so recording and replay are directly comparable.

- **Backward compatible**: the input columns stay at indices 0–5. `AInputReplayAutoPilot::LoadTrace` reads
  exactly `Cols[0..5]`, so it ignores the appended pose columns; old 6-column traces still load.

### FR2 — Comparison script
`Tests/CompareTraceReplay.ps1 -Trace <phoneTrace.csv> -Replay <pcReplay.csv> [-AlignStart]`:
- Reads the phone trajectory from the trace's `board_*` columns and the PC trajectory from the replay
  recorder's `x/y/z,roll/pitch/yaw`.
- Both clocks start at 0 at the replay handoff, so samples align on `t` (matched nearest-in-time; phone
  ~60 Hz, replay recorder ~20 Hz).
- Reports mean/max **position drift (cm)** and **yaw/pitch/roll drift (deg)**, plus the time of max
  divergence and the final-sample divergence.
- `-AlignStart` subtracts the `t=0` position offset first, isolating trajectory **shape** from a
  handoff that leaves the two on slightly different starting spots.

## Workflow

1. Build + deploy to phone; record a session (the trace now carries the phone's board trajectory).
2. Pull it: `Tests/PullInputTraces.ps1`.
3. Replay on PC with the recorder on. Headless, without editing the level, via the `-ReplayTrace=<file>`
   command-line arg (read in `AInputReplayAutoPilot::BeginPlay`; it force-enables the placed replay actor,
   points it at the trace, bypasses the `surf.autopilots` filter, and names the output after the trace):
   ```
   $env:TEST_MAP='Surfing_infinite_wave'
   $env:EXTRA_ARGS='-ReplayTrace=<trace>.csv'
   pwsh RunGameAndCollectLogs.ps1 -TimeoutSeconds 180
   ```
   It writes the PC trajectory to `Saved/Tests/latest/<trace-basename>.csv`. (A CVar won't work here —
   `-ExecCmds` CVars aren't applied until the first tick, one frame after `BeginPlay` loads the trace.)
4. `pwsh Tests/CompareTraceReplay.ps1 -Trace Tests/InputTraces/<trace>.csv -Replay Saved/Tests/latest/<trace-basename>.csv`
   → divergence report. Add `-AlignStart` for shape-only.

## Caveats

- **Only NEW traces have the pose columns.** Traces recorded before this change have no `board_*` data and
  the script will say so.
- **Handoff start state**: the pop-up autopilot may leave PC and phone on slightly different spots/attitudes
  at `t=0`, so absolute drift starts non-zero — `-AlignStart` removes that to compare shape.
- **Same-settings prerequisite**: for the comparison to isolate physics/frame-rate effects, PC and phone
  must run identical tuning (see the reset/copy flow for `Saved/TuningOverrides.json`).

## Findings (2026-07-17)

First run of the loop on `phone-2026-07-17-08-39-13.csv` (18.9 s, both PC and phone at default settings):

- **PC replay vs phone recording:** position drift grew to **~78 m**, yaw to a near-opposite **177°**, starting
  together (41 cm at t=0) and fanning out from ~t=4 s. Yaw (heading) is the dominant divergence axis.
- **PC vs PC, same trace, variable timestep (default headless):** three identical runs diverged from *each
  other* by **6.8–22.8 m** and up to **180° yaw**. So the physics is **non-deterministic run-to-run** — you
  cannot get a stable PC reference this way, and "feels different every time" is real.
- **PC vs PC, fixed timestep (`-usefixedtimestep -fps=60`):** run-to-run divergence collapses to **2–13 cm /
  2–18° yaw** (~99% reduction).

**Root cause:** the custom Chaos per-axis angular damping integrates `W *= (1 - damping)` **per frame**, so a
jittery/variable frame rate integrates each run (and each device) differently. Fixing the timestep makes it
near-deterministic. The phone (different, variable frame rate than PC) diverges for the same reason.

**Fix direction:** run the surfing physics at a **fixed substep** so integration is frame-rate independent —
project physics settings (`bSubstepping` + `MaxSubstepDeltaTime` / `MaxSubsteps`), or key the per-axis damping
decay to a fixed dt rather than the frame delta. A small residual non-determinism remains under fixed timestep
(~tens of cm / ~15° yaw by trace end, one of three runs) — likely pop-up handoff landing a frame off, or async
float ordering; secondary, revisit after the substep fix.

## Status

- [x] FR1 — board pose columns in the input trace
- [x] FR2 — `CompareTraceReplay.ps1`
- [x] Compiles
- [x] Recorded a phone trace with the new build, replayed on PC, ran the comparison
- [x] Root cause found: variable-timestep integration of the per-axis Chaos damping (not settings)
- [ ] Make the surfing physics fixed-substep; re-run the loop to confirm PC↔phone convergence
- [ ] Chase the residual fixed-timestep non-determinism (handoff-frame / async)
