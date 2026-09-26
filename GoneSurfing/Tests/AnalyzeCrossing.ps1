# Replay a phone input trace on PC and report WHEN (and whether) the board crosses the wave
# crest to the far side, timed against the WaterController's in-sim clock.
#
#   pwsh Tests/AnalyzeCrossing.ps1 -Trace phone-2026-07-17-14-50-53.csv
#   pwsh Tests/AnalyzeCrossing.ps1 -Trace phone-2026-07-17-14-50-53   # .csv optional
#   pwsh Tests/AnalyzeCrossing.ps1 -Trace <name> -SkipReplay          # re-parse last run's log
#
# How it works:
#   1. Replays the trace headlessly (RunGameAndCollectLogs.ps1) with surf.debug.flags 'crossing',
#      which makes ASharedCalculations log a per-tick CROSSING line containing the WaterController
#      SecondsElapsed / CurrentFrame, board pos, signedDistanceToCrest, vCrossFace and planing.
#   2. Parses those lines for the front SC actor, smooths signedDistanceToCrest, and finds the first
#      SUSTAINED crossing from the wave face (dist<0) to behind the crest (dist>0). Corroborates with
#      the planing decay (the board losing the wave as it goes over the top).
#
# Notes:
#   - signedDistanceToCrest is measured along the wave's cross-shore axis, derived at runtime from the
#     InfiniteWaveManager tile geometry, with an unsaturated hill-climb (see SharedCalculations.cpp).
#   - PC replay diverges from the on-device / editor ride (chaotic physics), so exact timing will not
#     match to the 1/100 s; use it to see the crossing behaviour, not a deterministic timestamp.
param(
  [Parameter(Mandatory=$true)][string]$Trace,
  [int]$Fps = 60,
  [switch]$SkipReplay,
  [string]$Actor = "SharedCalculationsBP_C_1",
  [int]$TimeoutSeconds = 300,
  # Ignore this many seconds after planing establishes: the drop-in / catch can briefly clip the
  # board behind the lip before the real down-the-line ride, which is not "the crossing".
  [double]$SkipCatchSeconds = 1.5
)
$ErrorActionPreference = "Stop"

$repo = "E:\windowsgrejor\git\GoneSurfingUE5"
$proj = Join-Path $repo "GoneSurfing"
$log  = Join-Path $proj "Saved\Logs\GoneSurfing.log"

$traceFile = if (Test-Path $Trace) { Split-Path -Leaf $Trace } else { $Trace }
if (-not $traceFile.ToLower().EndsWith(".csv")) { $traceFile += ".csv" }
$tracePath = Join-Path $proj "Tests\InputTraces\$traceFile"
if (-not (Test-Path $tracePath)) { throw "Trace not found: $tracePath" }

if (-not $SkipReplay) {
  Write-Host "[1/2] Replaying '$traceFile' on PC (fixed $Fps fps, crossing flag)..."
  $env:TEST_MAP      = "Surfing_infinite_wave"
  $env:EXTRA_ARGS    = "-ReplayTrace=$traceFile -usefixedtimestep -fps=$Fps"
  $env:EXTRA_EXECCMDS = "surf.debug.flags 'crossing'"
  $runLog = Join-Path $env:TEMP "analyze_crossing_run.log"
  & (Join-Path $repo "RunGameAndCollectLogs.ps1") -TimeoutSeconds $TimeoutSeconds *> $runLog
  if ($LASTEXITCODE -ne 0) { Write-Host "  (runner exit $LASTEXITCODE; see $runLog - parsing log anyway)" }
}
if (-not (Test-Path $log)) { throw "No game log at $log" }

Write-Host "[2/2] Parsing crossing log for $Actor ..."
$rx = [regex]("CROSSING \[" + [regex]::Escape($Actor) + "\] wcSecs=([-0-9.]+) wcFrame=([-0-9]+) " +
              "pos=\(([-0-9.]+), ([-0-9.]+), [-0-9.]+\) distToCrest=([-0-9.]+).*?" +
              "submersion=([-0-9.]+) underW=([-0-9.]+).*?vCrossFace=([-0-9.]+).*?planing=([-0-9.]+)")
$rows = New-Object System.Collections.Generic.List[object]
foreach ($line in [System.IO.File]::ReadLines($log)) {
  if ($line.IndexOf("CROSSING [$Actor]") -lt 0) { continue }
  $m = $rx.Match($line)
  if (-not $m.Success) { continue }
  $rows.Add([pscustomobject]@{
    s=[double]$m.Groups[1].Value; f=[int]$m.Groups[2].Value
    x=[double]$m.Groups[3].Value; y=[double]$m.Groups[4].Value
    dist=[double]$m.Groups[5].Value; sub=[double]$m.Groups[6].Value; uw=[double]$m.Groups[7].Value
    vcf=[double]$m.Groups[8].Value; plan=[double]$m.Groups[9].Value })
}
if ($rows.Count -eq 0) { throw "No CROSSING lines for $Actor found. Build stale, or crossing flag not set?" }

# Ride window: from the first tick planing is established (>=0.8).
$firstPlan = $rows | Where-Object { $_.plan -ge 0.8 } | Select-Object -First 1
$rideStart = if ($firstPlan) { $firstPlan.s } else { 6.0 }
$ride = @($rows | Where-Object { $_.s -ge $rideStart })

function SmoothDist([double]$s) {
  $sum=0.0; $n=0
  foreach ($q in $ride) { if ([math]::Abs($q.s - $s) -le 0.15) { $sum += $q.dist; $n++ } }
  if ($n -gt 0) { $sum/$n } else { 0.0 }
}

# Find first SUSTAINED crossing past the catch window: smoothed dist >=0 now AND mean dist over the
# next 0.4 s >=0. Also count how many distinct far-side excursions there are (the board can straddle
# the crest repeatedly), and note the planing-decay window (where it most commits to the far side).
$searchStart = $rideStart + $SkipCatchSeconds
$crossing = $null
$straddles = 0
$prevSide = $null
foreach ($r in $ride) {
  if ($r.s -lt $searchStart) { continue }
  $ds = SmoothDist $r.s
  $side = if ($ds -ge 0) { "behind" } else { "face" }
  if ($side -eq "behind" -and $prevSide -ne "behind") {
    $fwd = @($ride | Where-Object { $_.s -ge $r.s -and $_.s -le $r.s + 0.4 })
    if ((($fwd | Measure-Object dist -Average).Average) -ge 0) {
      $straddles++
      if (-not $crossing) { $crossing = $r }
    }
  }
  $prevSide = $side
}
# Planing-decay window: lowest planing during the ride (board most committed to going over the back).
$minPlan = $ride | Sort-Object plan | Select-Object -First 1

# Downsampled table (~0.25 s).
"`n{0,6} {1,6} {2,7} {3,7} {4,9} {5,7}  {6}" -f "wcSec","frame","x","y","distSm","plan","side"
$last = -9.0
foreach ($r in $ride) {
  if ($r.s - $last -lt 0.2499) { continue }
  $last = $r.s
  $ds = SmoothDist $r.s
  $side = if ($ds -lt -20) { "FACE" } elseif ($ds -gt 20) { "BEHIND (far side)" } else { "~crest" }
  "{0,6:N2} {1,6} {2,7:N0} {3,7:N0} {4,9:N0} {5,7:N2}  {6}" -f $r.s,$r.f,$r.x,$r.y,$ds,$r.plan,$side
}

"`n==== VERDICT ===="
if ($crossing) {
  "First crest crossing to the FAR SIDE (after the catch) at WaterController SecondsElapsed = {0:N2} s (CurrentFrame {1}), pos=({2:N0}, {3:N0}), planing={4:N2}." -f `
    $crossing.s, $crossing.f, $crossing.x, $crossing.y, $crossing.plan
  "Far-side excursions in this ride: $straddles (the board can straddle the crest more than once)."
  "Board most committed to the back (min planing {0:N2}) at SecondsElapsed = {1:N2} s (frame {2})." -f `
    $minPlan.plan, $minPlan.s, $minPlan.f
  "(PC replay; the on-device/editor crossing time differs slightly due to chaotic-physics divergence.)"
} else {
  $closest = $ride | Sort-Object dist -Descending | Select-Object -First 1
  "No sustained crossing to the far side within the ride. Closest approach: distToCrest={0:N0} cm at SecondsElapsed={1:N2} s (frame {2}). The board rides the face throughout." -f `
    $closest.dist, $closest.s, $closest.f
}

# THROUGH vs OVER: submersion = wave surface Z minus board Z (+ submerged, - board above surface).
# Compare the whole ride vs the crest-crossing ticks; if the board never surfaces at the crest it is
# gliding THROUGH the wave body; if submersion goes <=0 at the crossings it goes OVER the top.
"`n==== THROUGH vs OVER ===="
$behind = @($ride | Where-Object { (SmoothDist $_.s) -ge 0 })
$aboveN = @($ride | Where-Object { $_.sub -le 0 }).Count
$rideAvg = ($ride | Measure-Object sub -Average).Average
"Whole ride: submersion mean {0:N0} cm (board below surface = water on top); ticks with board ABOVE surface: {1} of {2}." -f $rideAvg, $aboveN, $ride.Count
if ($behind.Count -gt 0) {
  $bAvg = ($behind | Measure-Object sub -Average).Average
  $bMin = ($behind | Measure-Object sub -Minimum).Minimum
  $bUw  = ($behind | Measure-Object uw -Average).Average
  "At the {0} crest-crossing ticks: submersion mean {1:N0} cm (min {2:N0}), amountUnderWater mean {3:N2}." -f $behind.Count, $bAvg, $bMin, $bUw
  if ($bMin -gt 0) {
    "VERDICT: board stays submerged at the crest (never surfaces) => it glides THROUGH the wave, not over it."
  } else {
    "VERDICT: submersion reaches <=0 at the crest => the board clears the lip and goes OVER the top."
  }
} else {
  if ($aboveN -eq 0) { "VERDICT: board is submerged the whole ride (never surfaces) => gliding THROUGH." }
  else { "VERDICT: board surfaces during the ride => rides OVER/on the wave." }
}
