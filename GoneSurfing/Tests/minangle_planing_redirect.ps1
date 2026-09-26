# Min-angle test for the planing redirect. Now that SurfboardUtils owns the CVar (pushes it from the
# tuning subsystem each tick), the value must come from Saved/TuningOverrides.json, not a console CVar.
# Writes the JSON per angle (preserving finDragCoefficient), runs the sharp-turn autopilot x3 each,
# then RESTORES the original JSON so the feature is left OFF in the editor.
$ErrorActionPreference = "Continue"
$env:TEST_MAP = "Surfing_infinite_wave"
$runner  = "E:\windowsgrejor\git\GoneSurfingUE5\RunGameAndCollectLogs.ps1"
$latest  = "E:\windowsgrejor\git\GoneSurfingUE5\GoneSurfing\Saved\Tests\latest"
$sweep   = "E:\windowsgrejor\git\GoneSurfingUE5\GoneSurfing\Saved\Tests\sweep"
$tuning  = "E:\windowsgrejor\git\GoneSurfingUE5\GoneSurfing\Saved\TuningOverrides.json"
New-Item -ItemType Directory -Force $sweep | Out-Null
$ap = "surfing_down_the_line_then_sharp_turn_left"
$orig = if (Test-Path $tuning) { Get-Content $tuning -Raw } else { "{}" }

try {
  foreach ($angle in @("2","3")) {
    foreach ($i in 1..3) {
      # finDragCoefficient preserved; only PlaningRedirectMaxAngle varied.
      $json = "{`n`t""finDragCoefficient"": 9.9999997473787516e-05,`n`t""PlaningRedirectMaxAngle"": $angle`n}"
      [System.IO.File]::WriteAllText($tuning, $json)
      $label = "min_th$($angle)_r$i"
      Write-Host "==================== $label (TuningOverrides PlaningRedirectMaxAngle=$angle) ===================="
      & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $runner -Argument "::$ap" -TimeoutSeconds 120
      $src = Join-Path $latest "$ap.csv"
      $dst = Join-Path $sweep "$label.csv"
      if (Test-Path $src) {
        Copy-Item $src $dst -Force
        $rows = Import-Csv $src | Where-Object { [double]$_.gameSeconds -ge 7.3 -and [double]$_.gameSeconds -le 9.5 }
        $peak = ($rows | Measure-Object underwater -Maximum).Maximum
        $dur  = [double]((Import-Csv $src)[-1].gameSeconds)
        Write-Host ("  {0}: firstTurnPeakUW={1:N2} -> {2} ; dur={3:N1}s" -f $label, $peak, ($(if($peak -ge 0.95){"BURIED"}else{"rode"})), $dur)
      } else { Write-Host "  WARNING: no CSV" }
    }
  }
} finally {
  [System.IO.File]::WriteAllText($tuning, $orig)
  Write-Host "Restored original TuningOverrides.json (feature OFF)."
}
Write-Host "==================== MIN-ANGLE TEST COMPLETE ===================="
