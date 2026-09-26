# Ad-hoc fin-tuning analyzer (not a committed test). Computes skid in the
# fin-carve-coupling AC2 window (t=6.5-7.5s) plus fin force percentiles.
# skid = velHdg - noseHdg, noseHdg = yaw + 90 (mesh rotated 90deg).
param(
    [string]$Csv = "E:\windowsgrejor\git\GoneSurfingUE5\GoneSurfing\Saved\Tests\latest\surfing-down-the-line.csv",
    [string]$Log = "E:\windowsgrejor\git\GoneSurfingUE5\GoneSurfing\Saved\Logs\GoneSurfing.log"
)
function Wrap($d){ while($d -gt 180){$d-=360}; while($d -lt -180){$d+=360}; $d }
function Pct($arr,$p){ if(-not $arr){return 0}; $s=@($arr|Sort-Object); $s[[int][math]::Floor(($s.Count-1)*$p)] }

$csvRows = Import-Csv $Csv
$rows = $csvRows | ForEach-Object {
    $t=[double]$_.t; $vx=[double]$_.vx; $vy=[double]$_.vy; $yaw=[double]$_.yaw
    $velHdg=[math]::Atan2($vy,$vx)*180/[math]::PI
    $skid=Wrap($velHdg-($yaw+90))
    [pscustomobject]@{ t=$t; skid=$skid; askid=[math]::Abs($skid); vmag=[math]::Sqrt($vx*$vx+$vy*$vy); yaw=$yaw; step=[int]$_.step }
}
$win = $rows | Where-Object { $_.t -ge 6.5 -and $_.t -le 7.5 }
"=== SKID window t=6.5-7.5 (AC2 target peak<=5) ==="
$win | ForEach-Object { "t={0:N2} skid={1,7:N1} vmag={2,6:N0} yaw={3,7:N1} step={4}" -f $_.t,$_.skid,$_.vmag,$_.yaw,$_.step }
"WINDOW peak |skid|: {0:N1}deg" -f ($win | Measure-Object askid -Maximum).Maximum
"OVERALL peak |skid|: {0:N1}deg  vmax: {1:N0}cm/s  yaw[{2:N0}..{3:N0}]" -f ($rows|Measure-Object askid -Maximum).Maximum,($rows|Measure-Object vmag -Maximum).Maximum,($rows|Measure-Object yaw -Minimum).Minimum,($rows|Measure-Object yaw -Maximum).Maximum

if (Test-Path $Log) {
    $L = Get-Content $Log
    $dmag = $L | Select-String 'FIN DRAG' | ForEach-Object { if ($_.Line -match 'magnitude: ([-\d.]+)') {[double]$Matches[1]} }
    $lmag = $L | Select-String 'Fin Lift FIRED' | ForEach-Object { if ($_.Line -match 'force: \(([-\d.]+), ([-\d.]+), ([-\d.]+)\)') {[math]::Sqrt([double]$Matches[1]*[double]$Matches[1]+[double]$Matches[2]*[double]$Matches[2]+[double]$Matches[3]*[double]$Matches[3])} }
    "=== FIN FORCES ==="
    if($dmag){"FIN DRAG  median={0:N1} p90={1:N1} p99={2:N1} max={3:N1}" -f (Pct $dmag 0.5),(Pct $dmag 0.9),(Pct $dmag 0.99),(($dmag|Measure-Object -Maximum).Maximum)}
    if($lmag){"FIN LIFT  median={0:N2} p90={1:N2} p99={2:N2} max={3:N2}" -f (Pct $lmag 0.5),(Pct $lmag 0.9),(Pct $lmag 0.99),(($lmag|Measure-Object -Maximum).Maximum)}
}
