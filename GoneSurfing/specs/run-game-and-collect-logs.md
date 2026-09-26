# Spec: RunGameAndCollectLogs.bat

## Overview
A non-interactive Windows script that builds the editor target, launches the project in `-game` mode with autopilot enabled and configurable debug flags, waits for the game to exit, then surfaces the relevant log content. Designed to be invoked by Claude Code or by hand for fast build → play → read iteration without manual editor interaction.

Companion to:
- [BuildAndLaunch.bat](../../BuildAndLaunch.bat) — for "I want to play it manually myself."
- [debug-logging.md](debug-logging.md) — defines the CVars this script populates.

## Background
Manual debug iteration is: open editor → click Play → observe → stop → read log. This script replaces that with a single command. The autopilot infrastructure (`AAutoPilot`) already simulates inputs as a kind of unit-test harness. The level used must contain an autopilot whose final step calls `Quit` (added as part of broader work; outside this script's responsibility).

## Requirements

### Functional

- **Argument**: a single optional string mapping to the debug-logging CVars:
  ```
  RunGameAndCollectLogs.bat thrust,buoyancy:left_middle,right_middle,tail
  ```
  - Before the colon: comma-separated `surf.debug.flags` values.
  - After the colon: comma-separated `surf.debug.actors` values.
  - If colon omitted, the whole arg is treated as flags only.
  - If no argument, both CVars are left empty (no debug overrides; just the autopilot).

- **Steps** in order:
  1. Kill any running `UnrealEditor.exe` (force).
  2. Rotate the existing log: rename `Saved\Logs\GoneSurfing.log` → `GoneSurfing.log.prev` (if it exists) so the run produces a fresh file.
  3. Build the editor target (`GoneSurfingEditor Win64 Development`). On failure, exit 1 without launching.
  4. Launch the project in `-game` mode with the configured `-ExecCmds`.
  5. Wait for the game process to exit naturally (autopilot calls `Quit`). Apply a hard timeout (default 600 s) as a safety net — kill if exceeded.
  6. After exit, surface the relevant log content (see "Log delivery" below).

- **Exit codes**:
  - `0` — successful build + game exited cleanly within timeout.
  - `1` — build failed; game not launched.
  - `2` — game launched but exceeded the safety-net timeout; was force-killed.
  - `3` — launch failed (binary or project not found).

- **Working directory**: repo root, same as `BuildAndLaunch.bat`.

- **Shared configuration**: engine path, project path, editor target name. Refactor the shared values into a tiny included `_paths.bat` (or `.ps1`) sourced by both scripts. Acceptable alternative: duplicate the lines, accept the drift risk.

### Non-functional

- **Non-interactive**: no `pause`, no menus, no human input.
- **Bounded**: every wait has a hard timeout. Never hangs.
- **Idempotent**: safe to run repeatedly. Each run produces a fresh log slice.
- **Safe**: kills only `UnrealEditor.exe`; doesn't touch unrelated processes.

## Architecture

### Argument parsing
```
INPUT  = %~1                                  # e.g. "thrust,buoyancy:left_middle,right_middle,tail"
FLAGS  = part before ":" (or whole INPUT)
ACTORS = part after ":" (or empty)
```

### Launch invocation
```
UnrealEditor.exe "%PROJECT_PATH%" "%TEST_MAP_NAME%" ^
  -game ^
  -log -stdout -fullstdoutlogoutput ^
  -unattended ^
  -ExecCmds="surf.debug.flags '%FLAGS%', surf.debug.actors '%ACTORS%'"
```

Notes:
- `-log -stdout -fullstdoutlogoutput` mirrors log output to the script's stdout (in addition to the file).
- `-unattended` suppresses interactive dialogs so a missing dependency fails fast instead of hanging.
- `-NullRHI` (skip rendering) is a future option for ~2-3× speedup; not in v1 since some bugs may be rendering-coupled.

### Wait + timeout
`.bat` doesn't have a clean "wait with timeout" primitive. Recommended: implement the script as a thin `.bat` wrapper that delegates to a `.ps1` for the actual logic. PowerShell makes this clean:
```powershell
$proc = Start-Process -FilePath $UEPath -ArgumentList $args -PassThru -NoNewWindow
if (-not $proc.WaitForExit(600 * 1000)) {
    $proc.Kill()
    exit 2
}
exit $proc.ExitCode
```

### Log delivery

After the game exits, the log file `<repo>\GoneSurfing\Saved\Logs\GoneSurfing.log` is final.

Strategy: **Rotate before launch, then expose the fresh file.** Step 2 above already renames the prior log to `.prev`, so after exit, `GoneSurfing.log` contains exactly this run's output.

Two outputs available:
- **stdout**: the launch flags include `-stdout -fullstdoutlogoutput`, so logs stream to the script's stdout in real time.
- **File**: `Saved\Logs\GoneSurfing.log` is the durable copy. Caller can `Read` it directly.

Both work; the script does not need to do any further extraction. The caller (Claude or human) reads what they need.

## Acceptance Criteria

### AC1: Exit codes
- Build OK + clean exit → 0.
- Build fails → 1, no launch attempted.
- Hard timeout exceeded → 2, process killed.
- Binary or project missing → 3.

### AC2: Argument parsing
| Invocation | flags | actors |
| --- | --- | --- |
| `RunGameAndCollectLogs.bat thrust:tail` | `thrust` | `tail` |
| `RunGameAndCollectLogs.bat thrust,buoyancy` | `thrust,buoyancy` | (empty) |
| `RunGameAndCollectLogs.bat` | (empty) | (empty) |
| `RunGameAndCollectLogs.bat thrust,buoyancy:left_middle,tail` | `thrust,buoyancy` | `left_middle,tail` |

### AC3: Autopilot terminates the run
The game exits because the autopilot calls `Quit`, not because of the safety-net timeout. The timeout fires only if the autopilot misbehaves.

### AC4: Log content available
After a successful run, `Saved\Logs\GoneSurfing.log` contains exactly this run's output (the previous log was rotated to `.prev` before launch). Stdout also contains the same content streamed live.

### AC5: Editor was killed before launch
If an editor was running when the script started, it is gone before the build step begins. No file-lock conflicts during build.

### AC6: CVars receive the parsed values
After launch, in-game `~` console shows `surf.debug.flags` and `surf.debug.actors` populated with the values parsed from the argument.

### AC7: Composes with BuildAndLaunch.bat
Both scripts share the same engine, project, and target paths. Updating one location updates both (via `_paths.bat` or equivalent shared include).

## Open Questions

1. **Test map**: which level does the script load? Hard-coded vs. second positional argument vs. environment variable? Lean toward an env var (`TEST_MAP`) with a sensible default.
2. **Quit step in autopilot**: outside this spec, but the script depends on it. Coordinated with the level-and-autopilot setup work.
3. **`-NullRHI`**: skip rendering for faster runs. Default off (some bugs are rendering-coupled), opt-in via a future flag.
4. **Stdout vs. file as primary delivery**: both are produced; document which one the caller is expected to use. Likely the file (durable, easier for Claude to `Read`).
5. **Multiple autopilots in the level**: if more than one is present, do all run, or is one selected? Out of scope here — autopilot configuration is its own concern.
6. **Profile shortcuts**: future `--profile flip-investigation` syntax that maps a name to a saved flag/actor combo. Not in v1.

## Related Files
- New: `RunGameAndCollectLogs.bat` (+ likely `RunGameAndCollectLogs.ps1`) at repo root.
- New (optional, refactor): `_paths.bat` or `_paths.ps1` at repo root, sourced by both scripts.
- Existing sibling: [BuildAndLaunch.bat](../../BuildAndLaunch.bat) — shares paths.
- Companion: [debug-logging.md](debug-logging.md) — defines the CVars this script populates.
- Implicit dependency: an `AAutoPilot` configured in the test level whose final step calls `Quit`.
