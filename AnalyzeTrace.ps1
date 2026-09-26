<#
.SYNOPSIS
    Headless performance analysis of an Unreal Insights .utrace capture.

.DESCRIPTION
    Runs UnrealInsights.exe headless (-NoUI -AutoQuit) to export aggregated per-timer
    statistics for the GameThread and RenderThread (plus the thread + counter registries),
    then parses the CSVs and prints a performance summary: frame time (median / avg / max),
    game-thread wait ratio, streaming-hitch indicators, and the top self-time / inclusive
    hotspots per thread.

    Why headless export instead of the Insights GUI:
      - The GUI can't be driven programmatically for a quick summary.
      - Insights parses -ExecOnAnalysisCompleteCmd but FParse::Value breaks on the space
        between an export command and its file path when the whole flag gets shell-quoted,
        so we pass a response file:  -ExecOnAnalysisCompleteCmd=@=<cmds.txt>
        (the '@=' form has no spaces in the argument, and each line inside is parsed cleanly).
      - Per-thread timer statistics come from "TimingInsights.ExportTimerStatistics" with
        -threads="..." -columns="*".

.PARAMETER Trace
    Path to a .utrace file. Defaults to the newest *.utrace in <project>/Saved/Profiling.

.PARAMETER Pull
    Before analyzing, adb-pull the newest .utrace off the connected device into
    <project>/Saved/Profiling, and analyze that.

.PARAMETER Device
    adb device serial (passed as 'adb -s <serial>'). Optional; only used with -Pull.

.PARAMETER Top
    Number of hotspot rows to show per section (default 15).

.PARAMETER KeepCsv
    Keep the intermediate CSVs (in <Saved/Profiling>/_analysis) instead of leaving them silently.

.PARAMETER StartTime
    Analyze only from this ABSOLUTE trace timestamp onwards. Note a trace's timeline does not start
    at 0 -- the 2026-07-15 capture's first event is past t=20 -- so this is rarely the flag you want.
    Prefer -SkipStartupSeconds.

.PARAMETER EndTime
    Analyze only up to this ABSOLUTE trace timestamp.

.PARAMETER SkipStartupSeconds
    Ignore the first N seconds *of gameplay*, resolved relative to the first frame in the trace
    rather than to absolute zero. A capture armed by CaptureTrace.ps1 starts at process launch, so
    its opening seconds are engine startup and level load; those hundreds of package loads otherwise
    dominate the streaming-hitch line and drag the average frame time away from steady-state play.
    20-30 is usually enough to clear it. Costs one extra Insights pass to locate the first frame.

.EXAMPLE
    ./AnalyzeTrace.ps1
    Analyze the newest local capture.

.EXAMPLE
    ./AnalyzeTrace.ps1 -Pull
    Pull the newest capture off the phone, then analyze it.

.EXAMPLE
    ./AnalyzeTrace.ps1 -Trace GoneSurfing/Saved/Profiling/20260715_150140.utrace -Top 25
#>
[CmdletBinding()]
param(
    [string]$Trace,
    [switch]$Pull,
    [string]$Device,
    [int]$Top = 15,
    [switch]$KeepCsv,
    [double]$StartTime = -1,
    [double]$EndTime = -1,
    [double]$SkipStartupSeconds = 0
)

$ErrorActionPreference = 'Stop'
$RepoRoot    = $PSScriptRoot
$ProjectDir  = Join-Path $RepoRoot 'GoneSurfing'
$ProfilingDir= Join-Path $ProjectDir 'Saved\Profiling'
$UeRoot      = if ($env:UE_ROOT) { $env:UE_ROOT } else { 'E:\windowsgrejor\git\UnrealEngine' }
$Insights    = Join-Path $UeRoot 'Engine\Binaries\Win64\UnrealInsights.exe'
$DeviceTraceDir = '/storage/emulated/0/Android/data/com.dsh.gonesurfing/files/UnrealGame/GoneSurfing/GoneSurfing/Saved/Profiling'

function Fail($msg) { Write-Host "ERROR: $msg" -ForegroundColor Red; exit 1 }

if (-not (Test-Path $Insights)) { Fail "UnrealInsights.exe not found at '$Insights'. Set `$env:UE_ROOT." }

# ---- Optionally pull the newest capture from the device ---------------------
if ($Pull) {
    $adbArgs = @()
    if ($Device) { $adbArgs += @('-s', $Device) }
    Write-Host "Finding newest .utrace on device..." -ForegroundColor Cyan
    # -1t = one per line, sorted newest first
    $newest = (& adb @adbArgs shell "ls -1t $DeviceTraceDir/*.utrace 2>/dev/null" | Select-Object -First 1)
    if (-not $newest) { Fail "No .utrace found on device under $DeviceTraceDir" }
    $newest = $newest.Trim()
    $leaf = Split-Path $newest -Leaf
    if (-not (Test-Path $ProfilingDir)) { New-Item -ItemType Directory -Force -Path $ProfilingDir | Out-Null }
    $dest = Join-Path $ProfilingDir $leaf
    Write-Host "Pulling $leaf ..." -ForegroundColor Cyan
    & adb @adbArgs pull $newest $dest | Out-Null
    if (-not (Test-Path $dest)) { Fail "adb pull failed for $newest" }
    $Trace = $dest
}

# ---- Resolve the trace file -------------------------------------------------
if (-not $Trace) {
    if (-not (Test-Path $ProfilingDir)) { Fail "No trace given and $ProfilingDir does not exist." }
    $latest = Get-ChildItem -Path $ProfilingDir -Filter '*.utrace' -File |
              Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $latest) { Fail "No .utrace files found in $ProfilingDir. Use -Trace or -Pull." }
    $Trace = $latest.FullName
}
$Trace = (Resolve-Path $Trace).Path
Write-Host "Trace: $Trace" -ForegroundColor Green

# ---- Run headless Insights export ------------------------------------------
$OutDir = Join-Path $ProfilingDir '_analysis'
if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Force -Path $OutDir | Out-Null }
# Insights wants forward slashes in the response-file commands.
$gt  = ($OutDir + '\gt_stats.csv') -replace '\\','/'
$rt  = ($OutDir + '\rt_stats.csv') -replace '\\','/'
$thr = ($OutDir + '\threads.csv')  -replace '\\','/'
$cmdsFile = Join-Path $OutDir 'cmds.txt'
$InsightsLog = Join-Path $UeRoot 'Engine\Programs\UnrealInsights\Saved\Logs'

# UnrealInsights.exe is a GUI-subsystem binary, so PowerShell's '&' does NOT wait for it.
# Start-Process -Wait blocks until analysis + export finish and the CSVs are flushed.
# It parses -ExecOnAnalysisCompleteCmd via a response file (@=) to dodge the space-in-value
# quoting bug, and writes its own log under Engine\Programs\UnrealInsights\Saved\Logs.
function Invoke-Insights([string[]]$Commands) {
    $Commands | Set-Content -Path $cmdsFile -Encoding ascii
    $cmdsFwd = ($cmdsFile) -replace '\\','/'
    $procArgs = @(
        "-OpenTraceFile=$Trace"
        '-AutoQuit'
        '-NoUI'
        "-ExecOnAnalysisCompleteCmd=@=$cmdsFwd"
        '-log'
    )
    Start-Process -FilePath $Insights -ArgumentList $procArgs -Wait -NoNewWindow | Out-Null
}

# ---- Resolve the time window ------------------------------------------------
# Insights' -startTime=/-endTime= are ABSOLUTE trace timestamps, and a trace's timeline does NOT
# start at 0 -- a device capture's first frame can sit tens of seconds in. So "-SkipStartupSeconds 20"
# cannot just be passed through as -startTime=20; on the 2026-07-15 capture that window contained the
# whole trace and silently changed nothing. Instead, find the first frame's absolute timestamp and
# offset from there. Costs one extra (small) Insights pass, and only when the flag is used.
if ($SkipStartupSeconds -gt 0) {
    $frames = ($OutDir + '\frame_events.csv') -replace '\\','/'
    Remove-Item $frames -ErrorAction SilentlyContinue
    Write-Host "Locating first frame (pre-pass)..." -ForegroundColor DarkYellow
    Invoke-Insights @("TimingInsights.ExportTimingEvents $frames -threads=`"GameThread`" -timers=`"FEngineLoop::Tick`" -columns=`"StartTime,EndTime`"")
    if (-not (Test-Path $frames)) { Fail "Pre-pass produced no frame CSV; cannot resolve -SkipStartupSeconds. Check $InsightsLog." }
    $first = (Import-Csv $frames | Select-Object -First 1).StartTime
    if ($null -eq $first) { Fail "Pre-pass CSV had no frames; cannot resolve -SkipStartupSeconds." }
    $StartTime = [double]$first + $SkipStartupSeconds
    Write-Host ("  first frame at {0:N2}s -> analyzing from {1:N2}s" -f [double]$first, $StartTime) -ForegroundColor DarkYellow
}

$window = ''
if ($StartTime -ge 0) { $window = "$window -startTime=$StartTime" }
if ($EndTime   -ge 0) { $window = "$window -endTime=$EndTime" }
if ($window) { Write-Host "Time window:$window" -ForegroundColor DarkYellow }

Remove-Item $gt,$rt,$thr -ErrorAction SilentlyContinue
Write-Host "Analyzing (headless Insights)..." -ForegroundColor Cyan
Invoke-Insights @(
    "TimingInsights.ExportTimerStatistics $gt -threads=`"GameThread`" -columns=`"*`"$window"
    "TimingInsights.ExportTimerStatistics $rt -threads=`"RenderThread 0`" -columns=`"*`"$window"
    "TimingInsights.ExportThreads $thr"
)
if (-not (Test-Path $gt)) {
    Fail "Export produced no GameThread CSV. Check the newest log under $InsightsLog (the trace may have failed to open)."
}
if ((Get-Item $gt).Length -lt 100) {
    Write-Host "  WARNING: the GameThread export is empty - the time window probably excludes the whole trace." -ForegroundColor Yellow
}

# ---- Parse + summarize ------------------------------------------------------
function Load-Stats($path) {
    if (-not (Test-Path $path)) { return @() }
    Import-Csv $path | ForEach-Object {
        [pscustomobject]@{
            Name    = $_.Name
            Count   = [int]$_.Count
            InclMs  = [double]$_.Incl  * 1000
            IAvgMs  = [double]$_.'I.Avg' * 1000
            IMedMs  = [double]$_.'I.Med' * 1000
            IMaxMs  = [double]$_.'I.Max' * 1000
            ExclMs  = [double]$_.Excl  * 1000
            EAvgMs  = [double]$_.'E.Avg' * 1000
            EMedMs  = [double]$_.'E.Med' * 1000
        }
    }
}

$GT = Load-Stats $gt
$RT = Load-Stats $rt

function Find-Timer($rows, $name) { $rows | Where-Object { $_.Name -eq $name } | Select-Object -First 1 }

$frame = Find-Timer $GT 'FEngineLoop::Tick'; if (-not $frame) { $frame = Find-Timer $GT 'Frame' }
$wait  = Find-Timer $GT 'GameThreadWaitForTask'
$flush = Find-Timer $GT 'FlushAsyncLoading'
$loadPkg = Find-Timer $GT 'LoadPackageInternal'

$bar = ('=' * 78)
Write-Host ""
Write-Host $bar -ForegroundColor DarkGray
Write-Host " PERFORMANCE SUMMARY  ($([IO.Path]::GetFileName($Trace)))" -ForegroundColor White
Write-Host $bar -ForegroundColor DarkGray

if ($frame) {
    $fps = if ($frame.IMedMs -gt 0) { 1000.0 / $frame.IMedMs } else { 0 }
    $fpsAvg = if ($frame.IAvgMs -gt 0) { 1000.0 / $frame.IAvgMs } else { 0 }
    Write-Host ("  Frames captured : {0}" -f $frame.Count)
    Write-Host ("  Frame time      : median {0,6:N2} ms ({1,3:N0} FPS)   avg {2,6:N2} ms ({3,3:N0} FPS)   max {4,7:N1} ms" -f `
        $frame.IMedMs, $fps, $frame.IAvgMs, $fpsAvg, $frame.IMaxMs)
    if ($wait) {
        $waitPct = if ($frame.IMedMs -gt 0) { 100.0 * $wait.IMedMs / $frame.IMedMs } else { 0 }
        Write-Host ("  GameThread wait : median {0,6:N2} ms/frame  ({1:N0}% of frame -> {2})" -f `
            $wait.IMedMs, $waitPct, $(if ($waitPct -gt 40) {'render/GPU-bound'} else {'game-thread work dominates'}))
    }
}
if ($flush -or $loadPkg) {
    Write-Host "  Streaming hitches:" -ForegroundColor Yellow
    if ($flush)   { Write-Host ("    FlushAsyncLoading   : {0} calls, {1:N0} ms total, worst {2:N0} ms" -f $flush.Count, $flush.InclMs, $flush.IMaxMs) }
    if ($loadPkg) { Write-Host ("    LoadPackageInternal : {0} calls, {1:N0} ms total, worst {2:N0} ms" -f $loadPkg.Count, $loadPkg.InclMs, $loadPkg.IMaxMs) }
}

function Show-Top($title, $rows, $sortProp, $color) {
    Write-Host ""
    Write-Host "  $title" -ForegroundColor $color
    Write-Host ("    {0,-44} {1,8} {2,10} {3,9} {4,9}" -f 'Timer','Count','Total ms','Avg ms','Max ms')
    $rows | Sort-Object -Property $sortProp -Descending | Select-Object -First $Top | ForEach-Object {
        $total = if ($sortProp -eq 'ExclMs') { $_.ExclMs } else { $_.InclMs }
        $avg   = if ($sortProp -eq 'ExclMs') { $_.EAvgMs } else { $_.IAvgMs }
        Write-Host ("    {0,-44} {1,8} {2,10:N1} {3,9:N3} {4,9:N2}" -f `
            ($_.Name.Substring(0,[Math]::Min(44,$_.Name.Length))), $_.Count, $total, $avg, $_.IMaxMs)
    }
}

Show-Top "GameThread - top SELF time (where CPU is actually spent)" $GT 'ExclMs' 'Cyan'
Show-Top "GameThread - top INCLUSIVE time"                          $GT 'InclMs' 'DarkCyan'
if ($RT.Count -gt 0) { Show-Top "RenderThread - top INCLUSIVE time" $RT 'InclMs' 'Magenta' }

Write-Host ""
Write-Host $bar -ForegroundColor DarkGray
Write-Host ("  CSVs: {0}" -f $OutDir) -ForegroundColor DarkGray
if (-not $KeepCsv) { Write-Host "  (re-run with -KeepCsv to preserve, or open them for deeper drill-down)" -ForegroundColor DarkGray }
Write-Host ""
