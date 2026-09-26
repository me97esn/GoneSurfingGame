# Repeat runs to measure autopilot bury-vs-ride variance and the redirect's effect on it.
# Same autopilot, same binaries; vary only the redirect rate and repeat each N times.
$ErrorActionPreference = "Continue"
$env:TEST_MAP = "Surfing_infinite_wave"
$runner = "E:\windowsgrejor\git\GoneSurfingUE5\RunGameAndCollectLogs.ps1"
$latest = "E:\windowsgrejor\git\GoneSurfingUE5\GoneSurfing\Saved\Tests\latest"
$sweep  = "E:\windowsgrejor\git\GoneSurfingUE5\GoneSurfing\Saved\Tests\sweep"
New-Item -ItemType Directory -Force $sweep | Out-Null
$ap = "surfing_down_the_line_then_sharp_turn_left"

foreach ($angle in @("0","4")) {
  foreach ($i in 1..3) {
    $env:EXTRA_EXECCMDS = "p.Chaos.Solver.PlaningRedirectMaxAngle $angle"
    $label = "rep_th$($angle)_r$i"
    Write-Host "==================== $label ===================="
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $runner -Argument "::$ap" -TimeoutSeconds 120
    $src = Join-Path $latest "$ap.csv"
    $dst = Join-Path $sweep "$label.csv"
    if (Test-Path $src) {
      Copy-Item $src $dst -Force
      # First-turn (gs 7.3-9.5) peak underwater = bury-vs-ride proxy.
      $rows = Import-Csv $src | Where-Object { [double]$_.gameSeconds -ge 7.3 -and [double]$_.gameSeconds -le 9.5 }
      $peak = ($rows | Measure-Object underwater -Maximum).Maximum
      $dur  = [double]((Import-Csv $src)[-1].gameSeconds)
      Write-Host ("  {0}: firstTurnPeakUW={1:N2} dur={2:N1}s" -f $label, $peak, $dur)
    } else { Write-Host "  WARNING: no CSV" }
  }
}
Write-Host "==================== REPEATS COMPLETE ===================="
