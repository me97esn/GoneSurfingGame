# Confirm Phase-1 carve-grip is inert at defaults: run the sharp-turn autopilot x3 with no overrides
# (planing redirect at its 2.0 default, carve grip 0) and report the bury-vs-ride outcome each time.
$ErrorActionPreference = "Continue"
$env:TEST_MAP = "Surfing_infinite_wave"
Remove-Item Env:\EXTRA_EXECCMDS -ErrorAction SilentlyContinue
$runner = "E:\windowsgrejor\git\GoneSurfingUE5\RunGameAndCollectLogs.ps1"
$latest = "E:\windowsgrejor\git\GoneSurfingUE5\GoneSurfing\Saved\Tests\latest"
$ap = "surfing_down_the_line_then_sharp_turn_left"
foreach ($i in 1..3) {
  & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $runner -Argument "::$ap" -TimeoutSeconds 120
  $src = Join-Path $latest "$ap.csv"
  if (Test-Path $src) {
    $rows = Import-Csv $src | Where-Object { [double]$_.gameSeconds -ge 7.3 -and [double]$_.gameSeconds -le 9.5 }
    $peak = ($rows | Measure-Object underwater -Maximum).Maximum
    Write-Host ("  defaultOff_r{0}: peakUW={1:N2} -> {2}" -f $i, $peak, ($(if($peak -ge 0.95){"BURIED"}else{"rode"})))
  } else { Write-Host "  defaultOff_r${i}: no CSV" }
}
Write-Host "==================== DONE ===================="
