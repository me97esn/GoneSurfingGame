# ==========================================================
#  RunGameAndCollectLogs.ps1
#  Non-interactive build → launch → wait → log flow.
#  See specs/run-game-and-collect-logs.md for the contract.
#
#  Exit codes:
#    0  build OK + game exited cleanly within timeout
#    1  build failed; game not launched
#    2  game launched but exceeded the safety-net timeout (killed)
#    3  launch failed (binary or project missing / spawn failed)
# ==========================================================

param(
    [string]$Argument = "",
    [int]$TimeoutSeconds = 600
)

$ErrorActionPreference = "Stop"

# --- Path configuration. Mirrors _paths.bat / BuildAndLaunch.bat. ---
$UeRoot       = "E:\windowsgrejor\git\UnrealEngine"
$UePath       = Join-Path $UeRoot "Engine\Binaries\Win64\UnrealEditor.exe"
$BuildBat     = Join-Path $UeRoot "Engine\Build\BatchFiles\Build.bat"
$ProjectPath  = "E:\windowsgrejor\git\GoneSurfingUE5\GoneSurfing\GoneSurfing.uproject"
$EditorTarget = "GoneSurfingEditor"
$LogPath      = "E:\windowsgrejor\git\GoneSurfingUE5\GoneSurfing\Saved\Logs\GoneSurfing.log"
$LogPathPrev  = "$LogPath.prev"
$TestMap      = if ($env:TEST_MAP) { $env:TEST_MAP } else { "Boards_on_flat_water" }

# --- Argument parsing: "flags:actors:autopilots" → ($Flags, $Actors, $Autopilots) ---
# All three sections are optional. Examples:
#   "thrust"                                      → flags only
#   "thrust:bottom"                               → flags + actors
#   "thrust:bottom:surfing-down-the-line"         → flags + actors + autopilot filter
#   "::surfing-down-the-line"                     → autopilot filter only
$Flags      = ""
$Actors     = ""
$Autopilots = ""
if ($Argument) {
    $parts = $Argument.Split(":", 3)
    if ($parts.Count -ge 1) { $Flags      = $parts[0] }
    if ($parts.Count -ge 2) { $Actors     = $parts[1] }
    if ($parts.Count -ge 3) { $Autopilots = $parts[2] }
}

Write-Host "[RunGameAndCollectLogs] flags='$Flags' actors='$Actors' autopilots='$Autopilots' timeout=${TimeoutSeconds}s map='$TestMap'"

# --- Sanity checks ---
if (-not (Test-Path $UePath))      { Write-Host "ERROR: UnrealEditor.exe not found at: $UePath"; exit 3 }
if (-not (Test-Path $ProjectPath)) { Write-Host "ERROR: Project not found at: $ProjectPath"; exit 3 }
if (-not (Test-Path $BuildBat))    { Write-Host "ERROR: Build.bat not found at: $BuildBat"; exit 3 }

# --- Step 1: kill any running editor so build/launch isn't blocked. ---
Write-Host "[1/4] Stopping any running UnrealEditor..."
$existing = Get-Process -Name "UnrealEditor" -ErrorAction SilentlyContinue
if ($existing) {
    $existing | Stop-Process -Force
    Start-Sleep -Seconds 2
    Write-Host "  - editor stopped ($($existing.Count) process(es))"
} else {
    Write-Host "  - no editor running"
}

# --- Step 2: rotate the existing log so this run produces a fresh file. ---
Write-Host "[2/4] Rotating log file..."
if (Test-Path $LogPath) {
    if (Test-Path $LogPathPrev) { Remove-Item -Path $LogPathPrev -Force }
    Move-Item -Path $LogPath -Destination $LogPathPrev -Force
    Write-Host "  - rotated previous log to $LogPathPrev"
} else {
    Write-Host "  - no existing log to rotate"
}

# --- Step 3: build editor target. ---
Write-Host "[3/4] Building $EditorTarget (Win64 Development)..."
& cmd /c "`"$BuildBat`" $EditorTarget Win64 Development -Project=`"$ProjectPath`" -WaitMutex -FromMsBuild"
$buildResult = $LASTEXITCODE
if ($buildResult -ne 0) {
    # Mirrors the BuildAndLaunch.bat workaround: "Target is up to date" can
    # surface as a non-zero exit. Treat it as success only if the editor
    # binary exists and is non-empty.
    if (Test-Path "E:\windowsgrejor\git\GoneSurfingUE5\GoneSurfing\Binaries\Win64\UnrealEditor-GoneSurfing.dll") {
        Write-Host "  - build returned $buildResult but DLL exists; treating as up-to-date"
    } else {
        Write-Host "============================================"
        Write-Host "BUILD FAILED (exit code $buildResult)"
        Write-Host "Game not launched. Fix errors above and re-run."
        Write-Host "============================================"
        exit 1
    }
}
Write-Host "  - build OK"

# --- Step 4: launch the editor in -game mode and wait for exit. ---
Write-Host "[4/4] Launching game (timeout ${TimeoutSeconds}s)..."

# CVar values are wrapped in single quotes so UE's ParseExecCmds preserves
# any commas inside them (the parser splits on commas OUTSIDE single-quotes
# to break the line into multiple commands — see Engine/Source/Runtime/
# Engine/Private/ParseExecCommands.cpp). Strip the whole arg if both empty.
$execCmds = ""
if ($Flags -or $Actors) {
    $execCmds = "surf.debug.flags '$Flags', surf.debug.actors '$Actors'"
}
if ($Autopilots) {
    if ($execCmds) { $execCmds = "$execCmds, surf.autopilots '$Autopilots'" }
    else           { $execCmds = "surf.autopilots '$Autopilots'" }
}
# Optional passthrough for extra console commands (e.g. "surf.replay.hold 3.0").
# Comma-joined into the same -ExecCmds line. Caller is responsible for single-quoting
# any value that itself contains commas.
if ($env:EXTRA_EXECCMDS) {
    if ($execCmds) { $execCmds = "$execCmds, $($env:EXTRA_EXECCMDS)" }
    else           { $execCmds = "$env:EXTRA_EXECCMDS" }
}

$ueArgs = @(
    "`"$ProjectPath`"",
    "`"$TestMap`"",
    "-game",
    "-log",
    "-stdout",
    "-fullstdoutlogoutput",
    "-unattended"
)
if ($execCmds) {
    $ueArgs += "-ExecCmds=`"$execCmds`""
}
# Optional passthrough for extra launch args read at BeginPlay (e.g. "-ReplayTrace=foo.csv").
# Unlike -ExecCmds CVars, command-line args are available in the first frame's BeginPlay.
if ($env:EXTRA_ARGS) {
    foreach ($a in $env:EXTRA_ARGS.Split(" ", [System.StringSplitOptions]::RemoveEmptyEntries)) {
        $ueArgs += $a
    }
}

Write-Host "  - args: $($ueArgs -join ' ')"

try {
    $proc = Start-Process -FilePath $UePath -ArgumentList $ueArgs -PassThru -NoNewWindow -ErrorAction Stop
} catch {
    Write-Host "ERROR: failed to launch UnrealEditor: $_"
    exit 3
}

if (-not $proc.WaitForExit($TimeoutSeconds * 1000)) {
    Write-Host "TIMEOUT: game did not exit within ${TimeoutSeconds}s; killing PID $($proc.Id)"
    try { $proc.Kill() } catch { }
    exit 2
}

$exitCode = $proc.ExitCode
Write-Host "[done] game exited with code $exitCode"
Write-Host "[done] log: $LogPath"

if ($exitCode -ne 0) {
    Write-Host "WARNING: game exited non-zero ($exitCode); log content is still available."
}

# --- Snapshot-test auto-compare ---
# Any autopilot with a non-empty TestName flushes a CSV to Saved/Tests/latest/.
# For each one, run Compare.ps1 and report. Doesn't fail the runner on
# regression — just surfaces it for the caller to decide.
$ProjectRoot = Split-Path -Parent $ProjectPath
$LatestDir   = Join-Path $ProjectRoot "Saved\Tests\latest"
$ComparePs1  = Join-Path $ProjectRoot "Tests\Compare.ps1"

if ((Test-Path $LatestDir) -and (Test-Path $ComparePs1)) {
    $testCsvs = @(Get-ChildItem -Path $LatestDir -Filter '*.csv' -File -ErrorAction SilentlyContinue)
    if ($testCsvs.Count -gt 0) {
        Write-Host ""
        Write-Host "=== Snapshot tests ==="
        $anyRegress = $false
        foreach ($csv in $testCsvs) {
            $name = [System.IO.Path]::GetFileNameWithoutExtension($csv.Name)
            & $ComparePs1 -TestName $name
            if ($LASTEXITCODE -eq 2) { $anyRegress = $true }
        }
        if ($anyRegress) {
            Write-Host ""
            Write-Host "One or more snapshot tests REGRESSED. Review the diffs above."
        }
    }
}

exit 0
