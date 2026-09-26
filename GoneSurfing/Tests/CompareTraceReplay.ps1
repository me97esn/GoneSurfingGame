# Measures how far a PC replay's board trajectory diverges from the trajectory that was
# recorded INSIDE a phone input trace (the board_* columns; see
# specs/trace-trajectory-comparison.md). Lets us quantify PC-vs-phone divergence instead
# of judging it by feel.
#
# Usage:
#   pwsh Tests/CompareTraceReplay.ps1 -Trace <inputTrace.csv> -Replay <replayTrajectory.csv> [-AlignStart]
#
#   -Trace      An input trace recorded on the phone. Must have board_x/y/z and
#               board_roll/pitch/yaw columns (record a NEW trace with the updated build).
#   -Replay     The CSV written by AInputReplayAutoPilot's recorder when the SAME trace is
#               replayed on PC (Saved/Tests/latest/<TestName>.csv). Columns include
#               t,x,y,z,roll,pitch,yaw.
#   -AlignStart Subtract the t=0 position offset before comparing, so you measure trajectory
#               SHAPE rather than absolute position (use when the pop-up handoff leaves the
#               two starting on slightly different spots).
#
# Both clocks start at 0 at the replay handoff, so samples align on t directly. Samples are
# matched nearest-in-time (phone trace is ~60 Hz, replay recorder ~20 Hz).

param(
  [Parameter(Mandatory=$true)][string]$Trace,
  [Parameter(Mandatory=$true)][string]$Replay,
  [switch]$AlignStart
)
$ErrorActionPreference = "Stop"

function Read-Table {
  param([string]$Path)
  if (-not (Test-Path -LiteralPath $Path)) { throw "File not found: $Path" }
  $header = $null; $rows = @()
  foreach ($line in Get-Content -LiteralPath $Path) {
    $l = $line.Trim()
    if ($l -eq "" -or $l.StartsWith("#")) { continue }
    if ($null -eq $header) {
      if ($l.StartsWith("t,")) { $header = $l.Split(",") }
      continue
    }
    $rows += ,($l.Split(","))
  }
  if ($null -eq $header) { throw "No header row (starting with 't,') found in $Path" }
  $idx = @{}
  for ($i = 0; $i -lt $header.Count; $i++) { $idx[$header[$i]] = $i }
  return [pscustomobject]@{ Idx = $idx; Rows = $rows }
}

function Require-Cols {
  param($Table, [string]$Path, [string[]]$Cols)
  foreach ($c in $Cols) {
    if (-not $Table.Idx.ContainsKey($c)) { throw "$Path is missing required column '$c'." }
  }
}

function Nearest-Idx {
  param([double[]]$Times, [double]$Q)
  $n = $Times.Count
  if ($Q -le $Times[0]) { return 0 }
  if ($Q -ge $Times[$n-1]) { return $n-1 }
  $lo = 0; $hi = $n-1
  while (($hi - $lo) -gt 1) {
    $mid = [int](($lo + $hi) / 2)
    if ($Times[$mid] -le $Q) { $lo = $mid } else { $hi = $mid }
  }
  if (($Q - $Times[$lo]) -le ($Times[$hi] - $Q)) { return $lo } else { return $hi }
}

function Angle-Delta { param([double]$a, [double]$b)
  $d = $a - $b
  while ($d -gt 180)  { $d -= 360 }
  while ($d -lt -180) { $d += 360 }
  return $d
}

$traceTbl  = Read-Table -Path $Trace
$replayTbl = Read-Table -Path $Replay
Require-Cols $traceTbl  $Trace  @("t","board_x","board_y","board_z","board_roll","board_pitch","board_yaw")
Require-Cols $replayTbl $Replay @("t","x","y","z","roll","pitch","yaw")

# Dense phone trajectory.
$ti = $traceTbl.Idx
$pt=@(); $px=@(); $py=@(); $pz=@(); $proll=@(); $ppitch=@(); $pyaw=@()
foreach ($r in $traceTbl.Rows) {
  $pt     += [double]$r[$ti["t"]]
  $px     += [double]$r[$ti["board_x"]]
  $py     += [double]$r[$ti["board_y"]]
  $pz     += [double]$r[$ti["board_z"]]
  $proll  += [double]$r[$ti["board_roll"]]
  $ppitch += [double]$r[$ti["board_pitch"]]
  $pyaw   += [double]$r[$ti["board_yaw"]]
}
if ($pt.Count -lt 2) { throw "Trace has too few pose samples." }

# Optional start-alignment offset applied to the REPLAY positions: phone(t0) - replay(t0).
$ri = $replayTbl.Idx
$offX=0.0; $offY=0.0; $offZ=0.0
if ($AlignStart) {
  $r0 = $replayTbl.Rows[0]; $t0 = [double]$r0[$ri["t"]]
  $j0 = Nearest-Idx $pt $t0
  $offX = $px[$j0] - [double]$r0[$ri["x"]]
  $offY = $py[$j0] - [double]$r0[$ri["y"]]
  $offZ = $pz[$j0] - [double]$r0[$ri["z"]]
}

$n=0; $sumPos=0.0; $maxPos=-1.0; $maxPosT=0.0
$sumYaw=0.0; $maxYaw=-1.0; $maxYawT=0.0
$sumPitch=0.0; $maxPitch=-1.0
$sumRoll=0.0; $maxRoll=-1.0
$lastPos=0.0; $lastT=0.0; $firstT=$null

foreach ($r in $replayTbl.Rows) {
  $t = [double]$r[$ri["t"]]
  if ($t -lt $pt[0] -or $t -gt $pt[$pt.Count-1]) { continue }   # only the overlapping span
  $j = Nearest-Idx $pt $t

  $dx = ([double]$r[$ri["x"]] + $offX) - $px[$j]
  $dy = ([double]$r[$ri["y"]] + $offY) - $py[$j]
  $dz = ([double]$r[$ri["z"]] + $offZ) - $pz[$j]
  $dpos = [math]::Sqrt($dx*$dx + $dy*$dy + $dz*$dz)

  $dyaw   = [math]::Abs((Angle-Delta ([double]$r[$ri["yaw"]])   $pyaw[$j]))
  $dpitch = [math]::Abs((Angle-Delta ([double]$r[$ri["pitch"]]) $ppitch[$j]))
  $droll  = [math]::Abs((Angle-Delta ([double]$r[$ri["roll"]])  $proll[$j]))

  $sumPos+=$dpos;     if ($dpos   -gt $maxPos)   { $maxPos=$dpos;     $maxPosT=$t }
  $sumYaw+=$dyaw;     if ($dyaw   -gt $maxYaw)   { $maxYaw=$dyaw;     $maxYawT=$t }
  $sumPitch+=$dpitch; if ($dpitch -gt $maxPitch) { $maxPitch=$dpitch }
  $sumRoll+=$droll;   if ($droll  -gt $maxRoll)  { $maxRoll=$droll }
  if ($null -eq $firstT) { $firstT=$t }
  $lastPos=$dpos; $lastT=$t; $n++
}

if ($n -eq 0) { throw "No overlapping time range between trace and replay." }

"==== Trace vs Replay trajectory divergence ===="
"  trace : $Trace"
"  replay: $Replay"
("  compared {0} samples over t = {1:N2} .. {2:N2} s{3}" -f $n, $firstT, $lastT, ($(if($AlignStart){" (start-aligned, offset {0:N1} cm)" -f ([math]::Sqrt($offX*$offX+$offY*$offY+$offZ*$offZ))}else{""})))
""
("  Position drift (cm): mean {0,8:N1}   max {1,8:N1} @ t={2:N2}s   final {3,7:N1} @ t={4:N2}s" -f ($sumPos/$n), $maxPos, $maxPosT, $lastPos, $lastT)
("  Yaw drift    (deg): mean {0,8:N1}   max {1,8:N1} @ t={2:N2}s" -f ($sumYaw/$n), $maxYaw, $maxYawT)
("  Pitch drift  (deg): mean {0,8:N1}   max {1,8:N1}" -f ($sumPitch/$n), $maxPitch)
("  Roll drift   (deg): mean {0,8:N1}   max {1,8:N1}" -f ($sumRoll/$n), $maxRoll)
""
if (-not $AlignStart) {
  "  (tip: re-run with -AlignStart to remove any t=0 position offset and compare shape only)"
}
