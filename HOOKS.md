# Build-and-relaunch hooks

This repo ships with two Claude Code hooks that automate the C++ build/relaunch loop:

- `PreToolUse` — closes the running Unreal Editor *before* Claude edits a `.cpp` or `.h` file. Without this, any `Build.bat` Claude tries to run during its turn fails because UE's Live Coding system locks builds while the editor is open.
- `Stop` — once Claude finishes a turn that left a `.cpp`/`.h` dirty, this kills any remaining editor instance, rebuilds the project, and relaunches the editor.

The implementation lives in:
- [BuildAndLaunch.bat](BuildAndLaunch.bat) — the kill + build script (at the repo root).
- [GoneSurfing/.claude/settings.local.json](GoneSurfing/.claude/settings.local.json) — the Claude Code hook entries (`PreToolUse` and `Stop`).

Both files are committed. After cloning to another Windows machine, four absolute paths and (optionally) the DDC/TEMP overrides must be edited to match the new machine.

## Prerequisites

- Windows + Git Bash (MINGW64). The hook detects MSYS via `uname -s` and silently no-ops on other systems.
- Claude Code CLI v2.x or later, signed in via `claude auth login`.
- An Unreal Engine 5.4 source build *or* a launcher install — anything that provides `Engine\Build\BatchFiles\Build.bat`.

## Edit `BuildAndLaunch.bat`

Lines 28-38 hardcode this machine's layout:

```
set UE_ROOT=E:\windowsgrejor\git\UnrealEngine
set PROJECT_PATH=E:\windowsgrejor\git\GoneSurfingUE5\GoneSurfing\GoneSurfing.uproject
set UE-LocalDataCachePath=E:\windowsgrejor\git\GoneSurfingUE5\GoneSurfing\Saved\DDC
set UE-SharedDataCachePath=E:\windowsgrejor\git\GoneSurfingUE5\GoneSurfing\Saved\DDC
set TEMP=E:\windowsgrejor\git\GoneSurfingUE5\GoneSurfing\Saved\Temp
set TMP=E:\windowsgrejor\git\GoneSurfingUE5\GoneSurfing\Saved\Temp
```

- `UE_ROOT` — directory containing `Engine\Build\BatchFiles\Build.bat`. For a launcher install this is typically `C:\Program Files\Epic Games\UE_5.4`.
- `PROJECT_PATH` — absolute path to `GoneSurfing.uproject` on this machine.
- The DDC/TEMP overrides exist only to keep cache I/O on a specific drive. Delete them or repoint as desired.

## Edit `GoneSurfing/.claude/settings.local.json`

Inside `hooks.Stop[0].hooks[0].command`, the final line invokes PowerShell with two absolute paths baked in:

```
powershell -NoProfile -Command "Start-Process 'E:\windowsgrejor\git\UnrealEngine\Engine\Binaries\Win64\UnrealEditor.exe' 'E:\windowsgrejor\git\GoneSurfingUE5\GoneSurfing\GoneSurfing.uproject'"
```

Replace both paths with the matching locations on the new machine. The editor path is `<UE_ROOT>\Engine\Binaries\Win64\UnrealEditor.exe`.

## Activate the hooks

Claude Code only loads hooks at session startup — editing settings mid-session has no effect on the running session.

1. Fully restart Claude Code (or restart VS Code, which restarts the embedded session).
2. In the new session, run `/hooks` to confirm both `PreToolUse` and `Stop` are listed.
3. Make a trivial `.cpp` or `.h` edit and end your turn. You should see the editor close immediately when the edit is applied (PreToolUse), and after the turn ends the status bar shows `Stopping editor, building, relaunching...` and the editor reappears.

## Why a `PreToolUse` hook is needed

UE's **Live Coding** system locks out regular UBT builds while the editor is running:

> Unable to build while Live Coding is active. Exit the editor and game, or press Ctrl+Alt+F11 if iterating on code in the editor or game.

This means *any* in-turn `Build.bat` invocation by Claude (e.g., to verify a change compiles) fails as long as the editor is open. The `PreToolUse` hook closes the editor the first time Claude touches a `.cpp` or `.h` file in a turn, so subsequent builds in the same turn succeed. The post-turn `Stop` hook then handles the final rebuild and relaunch.

Filter logic in the hook:
- Matches the `Edit`, `Write`, and `MultiEdit` tools.
- Reads the tool's `file_path` from stdin JSON via `python3`.
- Only runs `taskkill` when the path ends in `.cpp` or `.h` — edits to markdown, JSON, blueprints, etc. do not touch the editor.

## Why the `Stop` hook is split between a .bat and a bash launch

- The .bat handles only the kill and build steps. Putting the launch step inside the .bat (via `cmd start` or `powershell Start-Process` invoked from inside it) caused the spawned editor to die or never surface — the launcher chain `bash → cmd → bat → launcher → editor` left the editor too deep in the hook process tree.
- Doing the launch directly from the hook's bash script (one process layer shallower) detaches reliably.
- UE's `Build.bat` with `-FromMsBuild` returns a non-zero exit code on no-op (already-up-to-date) builds. The hook intentionally ignores the .bat's exit code and always attempts the relaunch — on a real compile error you'll just see the editor come up with stale binaries, which is a clear "my change didn't take effect" signal.
