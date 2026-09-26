# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A surfing physics sandbox built on a custom Unreal Engine 5.4 fork at `E:\windowsgrejor\git\UnrealEngine`. The single C++ module is `GoneSurfing` under `GoneSurfing/Source/GoneSurfing/`. Blueprints in `GoneSurfing/Content/` wire C++ actors together; the test level is `Boards_on_flat_water` (override via `TEST_MAP` env var).

**Design principle (owner, 2026-09-18): realistic surfing is the goal.** The physics follows what a real board would do — including leaving the water (airs) when the speed and the lip allow it — and scoring decides separately what it rewards (airtime scores nothing today). Never defer or suppress a realistic behaviour in the physics because the scoring is not ready for it.

The engine fork is not stock UE5.4 — it has custom per-axis Chaos damping and per-axis velocity ceilings that the surfing physics is tuned against. Don't assume vanilla UE physics behavior.

## Build, run, test

All scripts at the repo root, paths configured in `_paths.bat`:

- **`BuildAndLaunch.bat`** — kill editor, build `GoneSurfingEditor Win64 Development` via UBT. Does NOT launch the editor (the caller — typically the `Stop` hook — handles relaunch). Exit 0 ok, 1 build failed, 2 setup error.
- **`RunGameAndCollectLogs.bat [flags[:actors]]`** — the headless test runner. Kills editor, rotates `Saved/Logs/GoneSurfing.log` → `.prev`, builds, launches `UnrealEditor.exe -game -log -stdout -unattended` with the test map, waits for autopilot to call Quit (600 s safety timeout), then runs snapshot-test compare on any CSVs that landed in `Saved/Tests/latest/`. Argument forms:
  - `RunGameAndCollectLogs.bat thrust` — `surf.debug.flags=thrust`
  - `RunGameAndCollectLogs.bat thrust,buoyancy:tail,left_middle` — flags before colon, actor-label substrings after
- **Trace replay + tick-exact A/B** — `EXTRA_ARGS="-ReplayTrace=<trace>.csv -usefixedtimestep -fps=60 -BoardInTests -Board=<id>"` replays a recorded ride headlessly (deterministic at fixed step). Add `-ReplayOverridesAt=<trace t> -ReplayOverrides=name=value,...` to flip coefficients in memory on exactly that tick, so an A/B pair rides identical physics up to the event under test. PC mouse traces need `AssistDisable=1` in `Saved/TuningOverrides.json`. See `specs/replay-scheduled-overrides.md`.
- **`GoneSurfing/Tests/Compare.ps1 -TestName <name>`** — diffs `Saved/Tests/latest/<name>.csv` vs `Tests/baselines/<name>.csv`. Exit 0 OK / 1 WARN / 2 REGRESSION / 3 missing. Thresholds: pos drift cm, vel cm/s, pitch deg.
- **`GoneSurfing/Tests/Approve.ps1 -TestName <name>`** — promote latest CSV to baseline. Run after an intentional physics change re-validates the trajectory.
- **`Screenshot.ps1`** — launch the game windowed and save a PNG of it, for eyeballing UI/overlay/camera work without an Android deploy. `-Phone` uses the phone's post-DPI-scale logical size (1200×540), which is where layout and copy-length calls should be made. See the `screenshot-game` skill.
- **MCP servers** (`GoneSurfing/Tools/mcp/`, registered in `GoneSurfing/.mcp.json`) — two, sharing `mcp_lite.py`. **`surf-telemetry`**: query ride trajectory CSVs as structured data instead of parsing them by hand (`list_runs`, `run_summary`, `window`, `compare`, `find_events`); prefer it over ad-hoc CSV parsing, and note it reports run staleness, which `Saved/Tests/latest/` badly needs. **`surf-tuning`**: resolve any of the ~200 coefficients through the four-layer stack (compiled default → board profile → `Saved/TuningOverrides.json` → `Saved/BoardTuning/<id>.json`), search them by the *reasoning in their header comments*, diff two boards, and write the two Saved layers (`list_boards`, `search_tunables`, `resolve`, `set_tunable`, `revert`, `diff`). **`surf-live`**: talks to a RUNNING game over Unreal's RemoteControl HTTP API (:30010) — read/set/reset any coefficient with effect on the next physics tick, mid-ride, no relaunch (`status`, `find_subsystem`, `get_live`, `set_live`, `reset_live`, `list_live`, `call_function`); needs the editor target rebuilt once for the plugin, and the web server started (`WebControl.StartServer`, plus `-RCWebControlEnable` in `-game`). See `Tools/mcp/README.md`.

The build/relaunch hooks (`PreToolUse`, `Stop` in `GoneSurfing/.claude/settings.local.json`) auto-kill the editor when Claude edits `.cpp`/`.h` files and auto-rebuild + relaunch after the turn ends. **UE Live Coding locks UBT builds while the editor is running, so any in-turn build attempt fails if the editor is open** — the PreToolUse hook is what makes mid-turn builds possible. Both hooks no-op on non-MSYS shells. See `HOOKS.md` for the full setup.

## Architecture

### Actor topology (per surfboard, ~20+ actors)

A single surfboard has ~20 `AFluidDynamics` surface samplers (bottom × 5 front-to-back × 2 sides, plus rails, fins, nose, tail), 4 `ABuoyancy`, 1 `ASharedCalculations`, 1 `AWeightDistribution`, 1 `ASurfboardUtils`. Each FluidDynamics actor computes lift/thrust/drag for its surface and applies the result via `applyForceAsImpulse` (impulse = force × DeltaTime) on the surfboard's `UPrimitiveComponent` mesh. There is no central force loop — each actor pushes its own impulse per tick.

`ASharedCalculations` owns the planing state machine and relative-water-velocity computation that the FluidDynamics actors read. `AWeightDistribution` exposes `amountInFront` / `amountToTheRight` (0..1, 0.5=centered), driven by either player input (`ASurfboardPawn`) or by an autopilot — they fight if both are active.

### Forces, surfaces, and what "lift" means

The naming is non-obvious and load-bearing:

- **"lift"** in this codebase means Bernoulli suction — *downward* on the bottom of the board, *sideways* on the rails. It does not mean "upward force." Upward forces are called **thrust** or **upthrust**.
- The bottom hydrofoil produces both upthrust (perpendicular to flow) and a forward push (`forwardsThrustCoefficient`). Decoupling these lets you tune forward-thrust independently from upthrust — useful e.g. when a sharp turn would otherwise bleed all forward speed.
- `waveSlopeGravityCoefficient` (on bottom actors only) supplies the missing "gravity along wave slope" the reduced engine gravity can't provide on its own. It's the propulsion that drives planing — terminal speed scales ~linearly with it.
- `effectiveWaterHeight = baseHeight + waterColumnAbove + slopeSin × slopeHeight` is what every per-surface formula multiplies by. The baseline + slope terms keep trough/non-submerged forces non-zero so the board doesn't strand itself.

### Forces vs damping vs redirects — the split that shapes every board movement

The board's motion is produced by two systems that must not do each other's job:

| job | tool | where it lives |
|---|---|---|
| add energy | **force** (impulse) | this project — `AFluidDynamics`, `ABuoyancy`, `AWeightDistribution`, each pushing its own impulse per tick |
| resist / remove energy | **damping** toward a target velocity | the engine fork's Chaos solver |
| change direction without changing speed | **velocity redirect** (rotate the velocity vector) | the engine fork's Chaos solver |

The fork exists to provide the second and third tools. A *force* used to resist or to turn overshoots and reverses — measured: `wavePenetrationDrag`, whose only job was to resist crossing the face, went from −2,177 (resisting) to +12,507 (driving); a force-level governor on the summed drive moved the net surge force 12% because the ungoverned forces grew to compensate. Damping asymptotes; a redirect conserves speed. So: when a board does something wrong, first ask which job is failing, then reach for that tool. Never add a force to slow or steer the board, and never run a damping and a force for the same job (both on measured +80% RMS acceleration).

`ASurfboardUtils` feeds the fork every tick as `p.Chaos.Solver.*` CVars: per-axis local linear damping (`DampingLocalX/Y/Z`, `DampingZUp/Down`, minimal-damping multipliers), per-axis angular damping with asymmetric pitch (`AngularDampingX/Y/Z`, `…YAwayExtra`, `…YTowardReduction`), per-axis velocity ceilings (`MaxVelocityX/Y/ZUp/ZDown`), the **planing redirect** (rotates velocity toward the wave's up-slope — rocker lift; `PlaningRedirect*`), the **carve-grip redirect** (toward the commanded heading; `CarveGrip*`, `CarveHeading*`), the **wave-carry redirect** (keep-up; `WaveCarry*`), **wave-normal damping** toward the *water's* velocity along the cross-shore axis (`WaveNormal*`, `WaterVel*`), and **pitch righting** (`PitchRightingRate`, `PitchMisalignment`). Specs: `wave-interaction-damping-and-redirect.md` (the principle and the current default configuration), `planing-redirect.md`, `carve-grip-via-redirect.md`, `wave-carry-redirect.md`, `velocity-ceiling-damping.md`, `pitch-righting-and-redirect-escape.md`.

Consequences for analysis: the `torque` force budget (`TORQUE … cat=… Fin=`) lists *forces only*. A velocity change with no matching force in the budget is the fork (redirect, ceiling, damping toward a moving target) — look there before hunting for a missing force. The old wave-mass force family was retired to 0 by default (d4dbb60c1) when the wave interaction moved to damping + redirects; `wavePenetrationCoefficient` is still 0, but `waveMassThrustCoefficient` and `waveMassFlowDragCoefficient` were brought back at **0.0001** (owner, 2026-09-23) for the lip's punch — three and a half decades below the 0.003 they were retired from, so they are a lip-impact term now and not the old drive; the header block "RESTORING THE PRE-DAMPING BUILD" in `SurfTuningSubsystem.h` is the A/B control set and must be changed as a set.

### Surfboard mesh orientation

The surfboard StaticMesh is rotated 90° — **`board.forwards` is local +Y, not +X**. Pitch/roll/yaw conventions in `AStateTriggerAutoPilot` use `Roll` for what visually looks like pitch because of this. Never hardcode board axes; read them from the actor.

### Autopilots and snapshot tests

`AStateTriggerAutoPilot` is a state-machine autopilot whose steps advance based on observed pitch/yaw/roll/velocity/planing thresholds (plus timeouts). It is used for:
1. **Cinematic intros** — drive the board through pop-up before handing control to the player (`ASurfboardPawn.AutoPilotDuration`, `ControlDelayAfterAutoPilot`).

   **The takeoff is kinematic, not physics.** In the playable level the intro plays a recorded trace on rails: `SurfRails::AreForcesSuppressed()` gates every impulse/torque site while the board is moved kinematically, and physics resumes with a stamped position/velocity at handoff (`specs/deterministic-ride-handoff.md`). No propulsion term, gate or coefficient affects whether or when the board catches the wave — do not protect "takeoff timing" when changing a force, and do not read older specs/memories about wave-catch propulsion budgets as current (they predate 2026-08-25).
2. **Snapshot tests** — set `TestName` on the autopilot and it records a position/velocity/attitude trajectory CSV to `Saved/Tests/latest/<TestName>.csv` on the last step. `RunGameAndCollectLogs.ps1` then runs `Compare.ps1` on every CSV present.

In playable levels, autopilots must be `enabled=false` or absent or they fight player weight-shift input. Multiple `AStateTriggerAutoPilot` actors can co-exist in one level; tests pick which one runs via the `surf.autopilots '<TestName>'` CVar filter. `bAutoQuitOnComplete` only fires `QuitGame` when both `WorldType == EWorldType::Game` AND the filter CVar is non-empty (test runs). Interactive Android play has no filter, so the autopilot finishes its scripted sequence and hands off to the player without quitting. (`AAutoPilotSuite` was the old chaining mechanism; deleted.)

### Runtime debug CVars

Editor `bDebug*` UPROPERTY checkboxes are preserved, but two CVars override them at runtime:
- `surf.debug.actors "<substr1>,<substr2>"` — match actor labels via substring (e.g. `left_middle` hits all `bottom_left_middle_*` actors).
- `surf.debug.flags "thrust,buoyancy,..."` — category names.

Every debug site reads `if (bDebugX || SurfDebug::ShouldDebug(this, "x"))`. Helper is gated by `WITH_EDITOR` (true even in `-game` mode launched from the editor binary; the project is not configured for Shipping builds). Set via `-ExecCmds=` at launch — see `RunGameAndCollectLogs.bat` and `specs/debug-logging.md`.

### Player controls (`ASurfboardPawn`)

FPV camera pawn (Behind / Beside positions) plus Enhanced Input handlers for:
- `IA_PaddleForward` — analog forward thrust (W on PC binary, left stick Y on Android).
- `IA_Turn` — binary turn ±1 (A/D, left stick X past `AndroidTurnThreshold`).
- `IA_Weight` — 2D weight shift (right stick on Android; PC: RMB-gated mouse-delta integration via `IA_WeightGate`). Auto-centers on release over `WeightAutoCenterDuration`.

Drives the level's `AWeightDistribution`. See `specs/surfing-controls.md`.

### Infinite wave tiling

`AGridLODActor` + `AInfiniteWaveManager` reposition manually-placed grid actors as the board moves (no spawn/destroy). Frame indices into the precomputed wave-frequencies data come from `AWaveHeight`; `WaterController` BP exposes `CurrentFrame` for autopilot timing.

## Workflow: spec-driven dev

Non-trivial features start as a spec under `GoneSurfing/specs/<name>.md` before code lands. See `specs/README.md` for the template. Active specs include `surfing-controls.md`, `debug-logging.md`, `run-game-and-collect-logs.md`, `bottom-hydrofoil-thrust.md`, `barrel-glide-through-bug.md`, and more. When fixing a bug or adding a feature, check whether a spec already describes the intended behavior.

## Gotchas

- **Editor must be killed before any UBT build.** The `Stop` hook handles this on turn end; if you need to build mid-turn yourself, stop `UnrealEditor.exe`, `LiveCodingConsole.exe`, `zenserver.exe`, and `TraceServer.exe` first.
- **`Build.bat -FromMsBuild` returns non-zero on a no-op "up-to-date" build.** The wrappers treat non-zero as success if the editor DLL exists.
- **CVar argument quoting in `-ExecCmds`**: comma is the command separator; CVar values must be wrapped in single quotes (`surf.debug.flags 'thrust,buoyancy'`) so the parser preserves the commas inside.
- **`GetActorLabel()` is `WITH_EDITOR`-only.** Fine for `-game` mode launched from the editor binary; would break in a packaged Shipping build (not currently a target).
- **DDC/TEMP overrides** in `BuildAndLaunch.bat` keep build cache I/O on the E: drive. Reconfigure or delete on a different machine.
