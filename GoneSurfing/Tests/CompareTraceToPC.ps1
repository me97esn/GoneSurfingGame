# One command to compare a phone input trace against a PC replay of it.
#
#   pwsh Tests/CompareTraceToPC.ps1 -Trace phone-2026-07-17-08-39-13.csv
#
# It (1) replays the trace on PC via -ReplayTrace= (build + headless run, recording the PC trajectory),
# then reports (2) trajectory DRIFT and (3) BEHAVIOUR/FEEL metrics (phone recording vs PC replay).
#
# Read the FEEL metrics, not the drift: the surfing loop is chaotic, so exact trajectories diverge across
# machines/frame-rates no matter what -- that's expected. The fix's goal (see
# specs/framerate-independent-angular-damping.md) is that speed / turn-rate / path-length match, i.e. the
# board behaves the same. Small percent on those = pass, even if the drift is large.
#
# Params:
#   -Trace <file>   Phone trace (name under Tests/InputTraces/, or a path). Must have board_* columns.
#   -Fps <n>        PC replay frame rate (default 60).
#   -Free           Replay at variable timestep instead of -usefixedtimestep -fps=<n>.
#   -SkipReplay     Reuse the existing PC replay CSV (skip the build+run) -- just re-compare.

param(
  [Parameter(Mandatory=$true)][string]$Trace,
  [int]$Fps = 60,
  [switch]$Free,
  [switch]$SkipReplay
)
$ErrorActionPreference = "Stop"

$proj = "E:\windowsgrejor\git\GoneSurfingUE5\GoneSurfing"
$tracePath = if (Test-Path $Trace) { (Resolve-Path $Trace).Path } else { Join-Path $proj "Tests\InputTraces\$Trace" }
if (-not (Test-Path $tracePath)) { throw "Trace not found: $Trace" }
$traceFile = Split-Path -Leaf $tracePath
$baseName  = [System.IO.Path]::GetFileNameWithoutExtension($tracePath)
$replayCsv = Join-Path $proj "Saved\Tests\latest\$baseName.csv"

if (-not $SkipReplay) {
  $mode = if ($Free) { "variable timestep" } else { "fixed $Fps fps" }
  Write-Host "[1/3] Replaying '$traceFile' on PC ($mode) ..."
  if (Test-Path $replayCsv) { Remove-Item $replayCsv -Force }
  $ts = if ($Free) { "" } else { "-usefixedtimestep" }
  $env:TEST_MAP     = "Surfing_infinite_wave"
  $env:EXTRA_ARGS   = ("-ReplayTrace=$traceFile $ts -fps=$Fps").Trim() -replace "\s+"," "
  $env:EXTRA_EXECCMDS = ""
  $replayLog = Join-Path $env:TEMP "ctp_replay.log"
  & "E:\windowsgrejor\git\GoneSurfingUE5\RunGameAndCollectLogs.ps1" -TimeoutSeconds 180 *> $replayLog
  if (-not (Test-Path $replayCsv)) { throw "Replay produced no trajectory at $replayCsv. See $replayLog" }
}
if (-not (Test-Path $replayCsv)) { throw "No PC replay CSV at $replayCsv (run once without -SkipReplay)." }

# ---- 2. Trajectory drift (reuse the drift script) ----
Write-Host "`n[2/3] Trajectory drift (expected LARGE -- chaos; look at the feel metrics below instead):"
& (Join-Path $proj "Tests\CompareTraceReplay.ps1") -Trace $tracePath -Replay $replayCsv

# ---- 3. Behaviour / feel metrics ----
function LoadTraj($path, $cx,$cy,$cz, $cvx,$cvy,$cvz, $cyaw){
  $head=$null; $T=@();$X=@();$Y=@();$Z=@();$VX=@();$VY=@();$VZ=@();$YW=@()
  foreach($line in Get-Content -LiteralPath $path){
    $s=$line.Trim(); if($s -eq "" -or $s.StartsWith("#")){continue}
    if($null -eq $head){ if($s.StartsWith("t,")){$head=$s.Split(",")}; continue }
    $col=$s.Split(",")
    foreach($name in @($cx,$cy,$cz,$cvx,$cvy,$cvz,$cyaw)){ if($head.IndexOf($name) -lt 0){ throw "$path missing column '$name'." } }
    $T+=[double]$col[0]
    $X+=[double]$col[$head.IndexOf($cx)];   $Y+=[double]$col[$head.IndexOf($cy)];   $Z+=[double]$col[$head.IndexOf($cz)]
    $VX+=[double]$col[$head.IndexOf($cvx)]; $VY+=[double]$col[$head.IndexOf($cvy)]; $VZ+=[double]$col[$head.IndexOf($cvz)]
    $YW+=[double]$col[$head.IndexOf($cyaw)]
  }
  return @{T=$T;X=$X;Y=$Y;Z=$Z;VX=$VX;VY=$VY;VZ=$VZ;YW=$YW}
}
function AngDelta($a,$b){ $d=$a-$b; while($d -gt 180){$d-=360}; while($d -lt -180){$d+=360}; return $d }
function RunStats($tr){
  $n=$tr.T.Count; $sumSpd=0.0;$maxSpd=0.0;$path=0.0;$sumYaw=0.0;$yawN=0
  for($i=0;$i -lt $n;$i++){
    $spd=[math]::Sqrt($tr.VX[$i]*$tr.VX[$i]+$tr.VY[$i]*$tr.VY[$i]+$tr.VZ[$i]*$tr.VZ[$i])
    $sumSpd+=$spd; if($spd -gt $maxSpd){$maxSpd=$spd}
    if($i -gt 0){
      $path+=[math]::Sqrt(($tr.X[$i]-$tr.X[$i-1])*($tr.X[$i]-$tr.X[$i-1])+($tr.Y[$i]-$tr.Y[$i-1])*($tr.Y[$i]-$tr.Y[$i-1])+($tr.Z[$i]-$tr.Z[$i-1])*($tr.Z[$i]-$tr.Z[$i-1]))
      $dt=$tr.T[$i]-$tr.T[$i-1]; if($dt -gt 0){ $sumYaw+=[math]::Abs((AngDelta $tr.YW[$i] $tr.YW[$i-1]))/$dt; $yawN++ }
    }
  }
  $last=$n-1
  return [ordered]@{
    "mean speed (cm/s)" = [math]::Round($sumSpd/$n,0)
    "max speed (cm/s)"  = [math]::Round($maxSpd,0)
    "final speed (cm/s)"= [math]::Round([math]::Sqrt($tr.VX[$last]*$tr.VX[$last]+$tr.VY[$last]*$tr.VY[$last]+$tr.VZ[$last]*$tr.VZ[$last]),0)
    "path length (cm)"  = [math]::Round($path,0)
    "net disp (cm)"     = [math]::Round([math]::Sqrt(($tr.X[$last]-$tr.X[0])*($tr.X[$last]-$tr.X[0])+($tr.Y[$last]-$tr.Y[0])*($tr.Y[$last]-$tr.Y[0])),0)
    "mean turn (deg/s)" = [math]::Round($sumYaw/[math]::Max(1,$yawN),1)
  }
}
$phone = RunStats (LoadTraj $tracePath "board_x" "board_y" "board_z" "board_vx" "board_vy" "board_vz" "board_yaw")
$pc    = RunStats (LoadTraj $replayCsv  "x" "y" "z" "vx" "vy" "vz" "yaw")

Write-Host "`n[3/3] Behaviour / feel metrics (phone recording vs PC replay):"
"{0,-20} {1,12} {2,12}   {3,8}" -f "metric","phone","pc","dev"
foreach($k in $phone.Keys){
  $a=$phone[$k]; $b=$pc[$k]
  $dev = if($a -ne 0){ "{0:N1}%" -f ([math]::Abs($b-$a)/[math]::Abs($a)*100) } else { "-" }
  "{0,-20} {1,12} {2,12}   {3,8}" -f $k,$a,$b,$dev
}
Write-Host "`n(Small dev on speed / path / turn = the board behaves the same. Net-disp / final-speed run higher = residual chaos, not a regression.)"
