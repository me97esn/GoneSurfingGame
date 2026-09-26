# Pulls the newest input-trace CSVs recorded on the connected Android phone into
# GoneSurfing/Saved/InputTraces/ (the "fresh device dumps" search path of
# AInputReplayAutoPilot; curated traces live in Tests/InputTraces/). See
# specs/input-trace-replay.md.
#
#   pwsh Tests/PullInputTraces.ps1            # newest 5 (default)
#   pwsh Tests/PullInputTraces.ps1 -Count 10  # newest 10
#
# The device path has a doubled project folder and can vary, so the InputTraces
# directory is discovered with `adb shell find` rather than hard-coded.
param([int]$Count = 5)
$ErrorActionPreference = "Stop"

$pkg  = "com.dsh.gonesurfing"
$base = "/storage/emulated/0/Android/data/$pkg/files"

$dev = (& adb devices) | Where-Object { $_ -match '^\S+\s+device\b' }
if (-not $dev) { Write-Error "No adb device connected (run 'adb devices')."; exit 1 }

$dir = (& adb shell "find '$base' -type d -iname InputTraces") |
    ForEach-Object { $_.Trim() } | Where-Object { $_ } | Select-Object -First 1
if (-not $dir) { Write-Error "InputTraces directory not found on device under $base"; exit 1 }

$dest = Join-Path (Split-Path $PSScriptRoot -Parent) "Saved/InputTraces"
New-Item -ItemType Directory -Force -Path $dest | Out-Null

$files = (& adb shell "ls -t '$dir'/*.csv") |
    ForEach-Object { $_.Trim() } | Where-Object { $_ -ne "" } | Select-Object -First $Count
if (-not $files) { Write-Host "No .csv input traces found in $dir"; exit 0 }

Write-Host ("Pulling newest {0} input trace(s) from {1}" -f @($files).Count, $dir)
foreach ($f in $files) {
    $bn  = ($f -split '/')[-1]
    $out = Join-Path $dest $bn
    & adb pull "$f" "$out" | Out-Null
    if (Test-Path $out) { Write-Host ("  {0}  ({1} bytes)" -f $bn, (Get-Item $out).Length) }
    else                { Write-Host ("  FAILED: {0}" -f $bn) }
}
Write-Host ("Done -> {0}" -f $dest)
