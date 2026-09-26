<#
.SYNOPSIS
    Capture an Unreal Insights trace from a real gameplay session on the connected Android phone,
    pull it, and analyze it.

.DESCRIPTION
    Tracing is started and stopped MID-RIDE through the game's adb console-command receiver
    (ConsoleCmdReceiver.java: `am broadcast -a android.intent.action.RUN -e cmd '<console cmd>'`,
    Development builds only). This script:

      1. verifies a device is attached,
      2. (re)launches the game unless -NoLaunch,
      3. waits -WarmupSeconds so the Start screen can be tapped and the ride is under way,
      4. sends `Trace.File <name>.utrace <channels>` to start recording to a file,
      5. waits -Seconds while you play,
      6. sends `Trace.Stop` (flushes and closes the file cleanly - no SIGKILL, no lost tail),
      7. pulls the .utrace and hands it to AnalyzeTrace.ps1.

    Why not arm it at launch with -trace= in UECommandLine.txt: `-trace=<channels>` on its own traces
    to MEMORY (TraceAuxiliary.cpp: "By default, if any channels are enabled we trace to memory"); a
    file only opens with -tracefile. Starting mid-ride also keeps engine startup and level load out of
    the capture entirely, so no -SkipStartupSeconds is needed and the whole-capture summary is the
    steady-state summary.

    The one thing that still needs a launch flag is -StatNamedEvents (finer scopes): that is applied
    through UECommandLine.txt, which is backed up and restored.

.PARAMETER Seconds
    How long to record. Default 90.

.PARAMETER WarmupSeconds
    Seconds between launch and the start of recording - time for the Start screen to be tapped and
    the intro to play out. Default 40. Ignored with -NoLaunch (recording starts immediately).

.PARAMETER NoLaunch
    Don't relaunch the game; record from whatever is running right now.

.PARAMETER Name
    Base name of the .utrace. Default ride_<yyyyMMdd_HHmmss>.

.PARAMETER Device
    adb device serial (passed as 'adb -s <serial>'). Only needed with several devices attached.

.PARAMETER Channels
    Trace channels. Default 'default' (= cpu,gpu,frame,log,bookmark). 'default,counters' adds stat
    counters; 'memory' is very heavy on device. Note the GPU channel produces no GPU track on the
    Pixel 10 under either RHI - GPU time has to be inferred from RHI-thread waits.

.PARAMETER StatNamedEvents
    Adds -statnamedevents to the launch command line, which emits far finer-grained named scopes
    (individual function-level timers) at a real runtime cost. Implies a relaunch.

.PARAMETER SkipStartupSeconds
    Ignore the first N seconds of the capture when analyzing. Rarely needed now that recording starts
    mid-ride; kept for parity with AnalyzeTrace.ps1.

.PARAMETER NoAnalyze
    Pull the trace but skip the analysis pass.

.EXAMPLE
    ./CaptureTrace.ps1
    Relaunch, give you 40 s to tap Start and get riding, record 90 s, pull + analyze.

.EXAMPLE
    ./CaptureTrace.ps1 -NoLaunch -Seconds 60
    You are already riding: record the next 60 s.
#>
[CmdletBinding()]
param(
    [int]$Seconds = 90,
    [int]$WarmupSeconds = 40,
    [switch]$NoLaunch,
    [string]$Name,
    [string]$Device,
    [string]$Channels = 'default',
    [switch]$StatNamedEvents,
    [double]$SkipStartupSeconds = 0,
    [switch]$NoAnalyze,
    [int]$Top = 25
)

$ErrorActionPreference = 'Stop'
$RepoRoot     = $PSScriptRoot
$ProjectDir   = Join-Path $RepoRoot 'GoneSurfing'
$ProfilingDir = Join-Path $ProjectDir 'Saved\Profiling'

$Pkg        = 'com.dsh.gonesurfing'
$DevBase    = "/sdcard/Android/data/$Pkg/files/UnrealGame/GoneSurfing"
$DevCmdline = "$DevBase/UECommandLine.txt"
$DevProf    = "$DevBase/GoneSurfing/Saved/Profiling"

function Fail($m) { Write-Host "ERROR: $m" -ForegroundColor Red; exit 1 }
function Step($m) { Write-Host $m -ForegroundColor Cyan }

$AdbArgs = @()
if ($Device) { $AdbArgs = @('-s', $Device) }

# Resolve the executable up front and invoke it through the variable. A function named 'Adb' that
# called a bare 'adb' would resolve to itself -- PowerShell command names are case-insensitive --
# and recurse until "call depth overflow".
$AdbExe = (Get-Command 'adb.exe' -ErrorAction SilentlyContinue).Source
if (-not $AdbExe) { $AdbExe = (Get-Command 'adb' -ErrorAction SilentlyContinue).Source }
if (-not $AdbExe) { Fail "adb not found on PATH (expected under %LOCALAPPDATA%\Android\Sdk\platform-tools)." }
function Adb { & $AdbExe @AdbArgs @args }
function ConsoleCmd($cmd) { Adb shell "am broadcast -a android.intent.action.RUN -e cmd '$cmd'" | Out-Null }

if (-not $Name) { $Name = 'ride_' + (Get-Date -Format 'yyyyMMdd_HHmmss') }
$Leaf = "$Name.utrace"

# ---- 1. Device present? -----------------------------------------------------
$dev = @(Adb devices) | Where-Object { $_ -match '^\S+\s+device\b' }
if (-not $dev) {
    Write-Host "ERROR: No adb device attached." -ForegroundColor Red
    Write-Host "  - plug the phone in over USB and unlock it"
    Write-Host "  - accept the 'Allow USB debugging?' prompt if it appears"
    Write-Host "  - 'adb kill-server' is the routine fix if it still does not show up"
    exit 1
}
Step ("Device: {0}" -f (($dev[0] -split '\s+')[0]))

# ---- 2. Launch (optionally with -statnamedevents on the command line) --------
$origCmdline = $null
$tmp = Join-Path $env:TEMP 'UECommandLine.trace.txt'
if ($StatNamedEvents -and $NoLaunch) { Fail "-StatNamedEvents is a launch flag; drop -NoLaunch." }

if (-not $NoLaunch) {
    if ($StatNamedEvents) {
        $probe = @(Adb shell "cat '$DevCmdline' 2>/dev/null") | Where-Object { $_ -and $_.Trim() }
        $origCmdline = if ($probe) { ($probe -join ' ').Trim() } else { '' }
        $base = if ($origCmdline) { $origCmdline } else { '-project="../../../GoneSurfing/GoneSurfing.uproject"' }
        $base = ($base -replace '\s*-statnamedevents', '').Trim()
        # UE reads the first line; keep it a single line with a trailing newline.
        [IO.File]::WriteAllText($tmp, "$base -statnamedevents`n", [Text.Encoding]::ASCII)
        Adb push $tmp $DevCmdline | Out-Null
        Step "Launch command line (temporary): $base -statnamedevents"
    }

    Step "Launching $Pkg ..."
    Adb shell "am force-stop $Pkg" | Out-Null
    Adb shell "monkey -p $Pkg -c android.intent.category.LAUNCHER 1" | Out-Null
    Write-Host ""
    Write-Host "  >>> Tap START on the phone and get riding. Recording begins in $WarmupSeconds s <<<" -ForegroundColor Green
    Write-Host ""
    Start-Sleep -Seconds $WarmupSeconds
}

$topAct = @(Adb shell "dumpsys activity activities 2>/dev/null | grep -m1 topResumedActivity") -join ' '
if ($topAct -and ($topAct -notmatch [regex]::Escape($Pkg))) {
    Write-Host "  WARNING: $Pkg does not look foregrounded. Check the phone screen." -ForegroundColor Yellow
    Write-Host "    $topAct" -ForegroundColor DarkGray
}

# ---- 3. Record --------------------------------------------------------------
Adb shell "rm -f '$DevProf/$Leaf'" | Out-Null
ConsoleCmd "Trace.File $Leaf $Channels"
Start-Sleep -Seconds 3
$probe = @(Adb shell "ls '$DevProf/$Leaf' 2>/dev/null") | Where-Object { $_ -match '\.utrace' }
if (-not $probe) {
    Fail "Trace.File did not create $DevProf/$Leaf. Is the game running, and is this a Development build (the console receiver is compiled out of Shipping)?"
}
Write-Host ""
Write-Host "  >>> RECORDING - ride normally for $Seconds seconds <<<" -ForegroundColor Green
Write-Host ""
$left = $Seconds - 3
while ($left -gt 0) {
    $chunk = [Math]::Min(10, $left)
    Start-Sleep -Seconds $chunk
    $left = $left - $chunk
    if ($left -gt 0) { Write-Host ("      {0,3} s left" -f $left) -ForegroundColor DarkGray }
}

# ---- 4. Stop (flushes and closes the file) ----------------------------------
Step "Trace.Stop ..."
ConsoleCmd "Trace.Stop"
Start-Sleep -Seconds 4

# ---- 5. Restore the launch command line if we touched it --------------------
if ($null -ne $origCmdline) {
    if ($origCmdline) {
        [IO.File]::WriteAllText($tmp, "$origCmdline`n", [Text.Encoding]::ASCII)
        Adb push $tmp $DevCmdline | Out-Null
        Step "Restored the original UECommandLine.txt."
    } else {
        Adb shell "rm -f '$DevCmdline'" | Out-Null
        Step "Removed the temporary UECommandLine.txt."
    }
    Remove-Item $tmp -ErrorAction SilentlyContinue
}

# ---- 6. Pull ----------------------------------------------------------------
if (-not (Test-Path $ProfilingDir)) { New-Item -ItemType Directory -Force -Path $ProfilingDir | Out-Null }
$dest = Join-Path $ProfilingDir $Leaf
Step "Pulling $Leaf ..."
Adb pull "$DevProf/$Leaf" $dest | Out-Null
if (-not (Test-Path $dest)) { Fail "adb pull failed for $DevProf/$Leaf" }
$sizeMb = [Math]::Round((Get-Item $dest).Length / 1MB, 1)
Write-Host ("  -> {0}  ({1} MB)" -f $dest, $sizeMb) -ForegroundColor Green
if ($sizeMb -lt 0.5) {
    Write-Host "  WARNING: that capture is suspiciously small - the session may have been too short." -ForegroundColor Yellow
}

# ---- 7. Analyze -------------------------------------------------------------
if ($NoAnalyze) { exit 0 }
$analyze = Join-Path $RepoRoot 'AnalyzeTrace.ps1'
if (-not (Test-Path $analyze)) { Fail "AnalyzeTrace.ps1 not found next to this script." }
$aArgs = @{ Trace = $dest; Top = $Top }
if ($SkipStartupSeconds -gt 0) { $aArgs['SkipStartupSeconds'] = $SkipStartupSeconds }
& $analyze @aArgs
