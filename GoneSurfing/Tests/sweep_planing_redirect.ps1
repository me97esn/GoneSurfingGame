# Planing-redirect θ sweep (world-frame v2). MaxAngle is a rad/s rotation rate toward up-slope.
# Runs each config, copies the recorded CSV off before the next overwrites Saved/Tests/latest.
$ErrorActionPreference = "Continue"
$env:TEST_MAP = "Surfing_infinite_wave"
$runner = "E:\windowsgrejor\git\GoneSurfingUE5\RunGameAndCollectLogs.ps1"
$latest = "E:\windowsgrejor\git\GoneSurfingUE5\GoneSurfing\Saved\Tests\latest"
$sweep  = "E:\windowsgrejor\git\GoneSurfingUE5\GoneSurfing\Saved\Tests\sweep"
New-Item -ItemType Directory -Force $sweep | Out-Null
$ap = "surfing_down_the_line_then_sharp_turn_left"

foreach ($angle in @("0","1","2","3")) {
  $env:EXTRA_EXECCMDS = "p.Chaos.Solver.PlaningRedirectMaxAngle $angle"
  $label = "v2_th$($angle)"
  Write-Host "==================== SWEEP RUN: $label ===================="
  & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $runner -Argument "::$ap" -TimeoutSeconds 200
  Write-Host "  run exit code: $LASTEXITCODE"
  $src = Join-Path $latest "$ap.csv"
  $dst = Join-Path $sweep "$label.csv"
  if (Test-Path $src) {
    Copy-Item $src $dst -Force
    # Quick metrics: rows, max underwater, min planing while surfing, peak z.
    $rows = Import-Csv $src
    $maxuw = ($rows | Measure-Object underwater -Maximum).Maximum
    $maxz  = ($rows | Measure-Object z -Maximum).Maximum
    $dur   = [double]($rows[-1].gameSeconds)
    Write-Host ("  {0}: rows={1} dur={2:N1}s maxUW={3:N3} maxZ={4:N0}" -f $label, $rows.Count, $dur, $maxuw, $maxz)
  } else { Write-Host "  WARNING: no CSV at $src" }
}
Write-Host "==================== SWEEP COMPLETE ===================="
