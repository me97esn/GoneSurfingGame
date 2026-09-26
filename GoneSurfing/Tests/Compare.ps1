# ==========================================================
#  Compare.ps1
#  Diff a snapshot test's latest run against its baseline.
#  Reads:
#    GoneSurfing/Tests/baselines/<TestName>.csv     (committed)
#    GoneSurfing/Saved/Tests/latest/<TestName>.csv  (latest run, gitignored)
#  Verdict:
#    OK         -- within thresholds
#    WARN       -- modest drift; eyeball
#    REGRESSION -- drift exceeds regression threshold
#  Exit codes: 0 OK, 1 WARN, 2 REGRESSION, 3 missing files / empty CSVs.
# ==========================================================

param(
    [Parameter(Mandatory)] [string]$TestName,
    # Defaults calibrated on barrel-glide-through (2026-05-08): two unmodified
    # runs vs the baseline produced 251cm and 739cm max drift, both in the long
    # surfing step where chaos compounds. Steps 0-4 stay within ~10cm.
    # WARN at ~1.5x upper noise, REGRESSION at ~4x. Pass overrides per test if
    # your scenario has a different noise floor.
    [int]$WarnPosCm    = 1000,
    [int]$RegressPosCm = 3000,
    [int]$WarnVelCmS    = 1500,
    [int]$RegressVelCmS = 4000,
    # Pitch thresholds: pitch is the diagnostic axis for nose-dive bugs and
    # planing stability. Roll and yaw are tracked too but only pitch counts
    # toward the verdict. Defaults are uncalibrated and conservative; lower
    # them once a noise-floor sample is collected for this test.
    [int]$WarnPitchDeg    = 15,
    [int]$RegressPitchDeg = 30
)

$ErrorActionPreference = 'Stop'

$ScriptDir    = $PSScriptRoot
$BaselinePath = Join-Path $ScriptDir "baselines\$TestName.csv"
$LatestPath   = Join-Path $ScriptDir "..\Saved\Tests\latest\$TestName.csv"

if (-not (Test-Path $BaselinePath)) {
    Write-Host "[$TestName] NO BASELINE at $BaselinePath"
    Write-Host "  Run the autopilot, eyeball the result, then: Tests\Approve.ps1 $TestName"
    exit 3
}
if (-not (Test-Path $LatestPath)) {
    Write-Host "[$TestName] NO LATEST at $LatestPath"
    Write-Host "  The autopilot didn't flush a CSV (did it reach its last step?)"
    exit 3
}

$Base = @(Import-Csv $BaselinePath)
$Lat  = @(Import-Csv $LatestPath)

if ($Base.Count -eq 0 -or $Lat.Count -eq 0) {
    Write-Host "[$TestName] EMPTY CSV -- baseline=$($Base.Count) rows, latest=$($Lat.Count) rows"
    exit 3
}

# Wrap a signed angle delta into [-180, 180] then take absolute. Roll and yaw
# can wrap; pitch is bounded but using the same helper costs nothing.
function AngleAbsDelta($a, $b) {
    $d = [double]$a - [double]$b
    while ($d -gt 180)  { $d -= 360 }
    while ($d -lt -180) { $d += 360 }
    return [Math]::Abs($d)
}

$Count = [Math]::Min($Base.Count, $Lat.Count)
$MaxPos = 0.0; $MaxPosT = 0.0; $MaxPosStep = 0
$SumPos = 0.0
$MaxVel = 0.0; $SumVel = 0.0
$MaxPitch = 0.0; $MaxPitchT = 0.0; $MaxPitchStep = 0; $SumPitch = 0.0
$MaxRoll  = 0.0; $SumRoll  = 0.0
$MaxYaw   = 0.0; $SumYaw   = 0.0
$StepStats = @{}  # step -> hashtable

for ($i = 0; $i -lt $Count; $i++) {
    $b = $Base[$i]; $l = $Lat[$i]

    $dx = [double]$l.x - [double]$b.x
    $dy = [double]$l.y - [double]$b.y
    $dz = [double]$l.z - [double]$b.z
    $dPos = [Math]::Sqrt($dx*$dx + $dy*$dy + $dz*$dz)

    $bvm = [Math]::Sqrt(([double]$b.vx)*([double]$b.vx) + ([double]$b.vy)*([double]$b.vy) + ([double]$b.vz)*([double]$b.vz))
    $lvm = [Math]::Sqrt(([double]$l.vx)*([double]$l.vx) + ([double]$l.vy)*([double]$l.vy) + ([double]$l.vz)*([double]$l.vz))
    $dVel = [Math]::Abs($lvm - $bvm)

    $dPitch = AngleAbsDelta $l.pitch $b.pitch
    $dRoll  = AngleAbsDelta $l.roll  $b.roll
    $dYaw   = AngleAbsDelta $l.yaw   $b.yaw

    if ($dPos -gt $MaxPos) {
        $MaxPos = $dPos
        $MaxPosT = [double]$b.t
        $MaxPosStep = [int]$b.step
    }
    if ($dVel -gt $MaxVel) { $MaxVel = $dVel }
    if ($dPitch -gt $MaxPitch) {
        $MaxPitch = $dPitch
        $MaxPitchT = [double]$b.t
        $MaxPitchStep = [int]$b.step
    }
    if ($dRoll -gt $MaxRoll) { $MaxRoll = $dRoll }
    if ($dYaw -gt $MaxYaw)   { $MaxYaw  = $dYaw  }
    $SumPos   += $dPos
    $SumVel   += $dVel
    $SumPitch += $dPitch
    $SumRoll  += $dRoll
    $SumYaw   += $dYaw

    $step = [int]$b.step
    if (-not $StepStats.ContainsKey($step)) {
        $StepStats[$step] = @{ MaxPos = 0.0; MaxPitch = 0.0; Count = 0 }
    }
    if ($dPos   -gt $StepStats[$step].MaxPos)   { $StepStats[$step].MaxPos   = $dPos }
    if ($dPitch -gt $StepStats[$step].MaxPitch) { $StepStats[$step].MaxPitch = $dPitch }
    $StepStats[$step].Count++
}

$MeanPos   = $SumPos   / $Count
$MeanVel   = $SumVel   / $Count
$MeanPitch = $SumPitch / $Count
$MeanRoll  = $SumRoll  / $Count
$MeanYaw   = $SumYaw   / $Count

# Final-position drift (last aligned sample)
$bLast = $Base[$Count - 1]; $lLast = $Lat[$Count - 1]
$fdx = [double]$lLast.x - [double]$bLast.x
$fdy = [double]$lLast.y - [double]$bLast.y
$fdz = [double]$lLast.z - [double]$bLast.z
$FinalDrift = [Math]::Sqrt($fdx*$fdx + $fdy*$fdy + $fdz*$fdz)

# Verdict. Pitch counts toward the verdict because nose-dive / planing-stability
# bugs may not show in position/velocity drift (a board on a different attitude
# can still be at the right XY). Roll and yaw are reported but informational.
if     ($MaxPos -gt $RegressPosCm -or $MaxVel -gt $RegressVelCmS -or $MaxPitch -gt $RegressPitchDeg) { $Verdict = "REGRESSION"; $Exit = 2 }
elseif ($MaxPos -gt $WarnPosCm    -or $MaxVel -gt $WarnVelCmS    -or $MaxPitch -gt $WarnPitchDeg)    { $Verdict = "WARN";       $Exit = 1 }
else                                                                                                  { $Verdict = "OK";         $Exit = 0 }

Write-Host "[$TestName] $Verdict"
Write-Host ("  pos:   max={0:N1}cm @ t={1:N2}s step={2} | mean={3:N1}cm | final={4:N1}cm" -f $MaxPos, $MaxPosT, $MaxPosStep, $MeanPos, $FinalDrift)
Write-Host ("  vel:   max={0:N1}cm/s | mean={1:N1}cm/s" -f $MaxVel, $MeanVel)
Write-Host ("  pitch: max={0:N1}deg @ t={1:N2}s step={2} | mean={3:N1}deg" -f $MaxPitch, $MaxPitchT, $MaxPitchStep, $MeanPitch)
Write-Host ("  roll:  max={0:N1}deg | mean={1:N1}deg  (informational)" -f $MaxRoll, $MeanRoll)
Write-Host ("  yaw:   max={0:N1}deg | mean={1:N1}deg  (informational)" -f $MaxYaw, $MeanYaw)
$perStepPos = foreach ($key in ($StepStats.Keys | Sort-Object)) {
    "step{0}={1:N0}cm" -f $key, $StepStats[$key].MaxPos
}
$perStepPitch = foreach ($key in ($StepStats.Keys | Sort-Object)) {
    "step{0}={1:N1}deg" -f $key, $StepStats[$key].MaxPitch
}
Write-Host ("  per-step max-pos:   {0}" -f ($perStepPos   -join "  "))
Write-Host ("  per-step max-pitch: {0}" -f ($perStepPitch -join "  "))
Write-Host ("  thresholds: WARN > {0}cm or {1}cm/s or {2}deg pitch; REGRESSION > {3}cm or {4}cm/s or {5}deg pitch" -f $WarnPosCm, $WarnVelCmS, $WarnPitchDeg, $RegressPosCm, $RegressVelCmS, $RegressPitchDeg)
if ($Base.Count -ne $Lat.Count) {
    Write-Host "  NOTE: row count mismatch (baseline=$($Base.Count), latest=$($Lat.Count)); compared first $Count rows"
}

exit $Exit
