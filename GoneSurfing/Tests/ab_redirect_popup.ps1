# A/B: does the pop-up/replay turn-into-wave depend on the planing redirect / carve grip?
# Empty autopilot filter => pop_up + InputReplay run at umap defaults; the replay records the CSV.
# Drive redirect strengths via TuningOverrides.json (project owns the CVars). Restore JSON after.
$ErrorActionPreference = "Continue"
$env:TEST_MAP = "Surfing_infinite_wave"
Remove-Item Env:\EXTRA_EXECCMDS -ErrorAction SilentlyContinue
$runner = "E:\windowsgrejor\git\GoneSurfingUE5\RunGameAndCollectLogs.ps1"
$latest = "E:\windowsgrejor\git\GoneSurfingUE5\GoneSurfing\Saved\Tests\latest"
$sweep  = "E:\windowsgrejor\git\GoneSurfingUE5\GoneSurfing\Saved\Tests\sweep"
$tuning = "E:\windowsgrejor\git\GoneSurfingUE5\GoneSurfing\Saved\TuningOverrides.json"
$log    = "E:\windowsgrejor\git\GoneSurfingUE5\GoneSurfing\Saved\Logs\GoneSurfing.log"
New-Item -ItemType Directory -Force $sweep | Out-Null
$replayCsv = "surfing-down-the-line"   # the InputReplay's TestName
$orig = if (Test-Path $tuning) { Get-Content $tuning -Raw } else { "{}" }
$fin = '"finDragCoefficient": 9.9999997473787516e-05'

# label -> extra JSON entries (defaults: planing 2, grip 4)
$cases = @(
  @{ label = "on";         extra = "" },
  @{ label = "planingoff"; extra = ",`n`t""PlaningRedirectMaxAngle"": 0" },
  @{ label = "bothoff";    extra = ",`n`t""PlaningRedirectMaxAngle"": 0,`n`t""CarveGripRate"": 0" }
)
try {
  foreach ($c in $cases) {
    [System.IO.File]::WriteAllText($tuning, "{`n`t$fin$($c.extra)`n}")
    Write-Host "==================== POP A/B: $($c.label) ===================="
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $runner -Argument "state,lift:SharedCalculationsBP,bottom_left_middle,bottom_right_middle" -TimeoutSeconds 90
    $src = Join-Path $latest "$replayCsv.csv"
    if (Test-Path $src) {
      Copy-Item $src (Join-Path $sweep "pop_$($c.label).csv") -Force
      if (Test-Path $log) { Copy-Item $log (Join-Path $sweep "pop_$($c.label).log") -Force }
      $rows = Import-Csv $src
      $dur = [double]($rows[-1].gameSeconds)
      Write-Host ("  pop_{0}: rows={1} dur={2:N1}s" -f $c.label, $rows.Count, $dur)
    } else { Write-Host "  WARNING: no CSV for $($c.label)" }
  }
} finally {
  [System.IO.File]::WriteAllText($tuning, $orig)
  Write-Host "Restored TuningOverrides.json"
}
Write-Host "==================== A/B DONE ===================="
