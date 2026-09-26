# ==========================================================
#  Approve.ps1
#  Promote a fresh trajectory recording to a checked-in baseline.
#  Use after intentional physics changes (or when first creating a baseline).
#    Saved/Tests/latest/<TestName>.csv  ->  Tests/baselines/<TestName>.csv
# ==========================================================

param([Parameter(Mandatory)] [string]$TestName)

$ErrorActionPreference = 'Stop'

$ScriptDir    = $PSScriptRoot
$LatestPath   = Join-Path $ScriptDir "..\Saved\Tests\latest\$TestName.csv"
$BaselinePath = Join-Path $ScriptDir "baselines\$TestName.csv"

if (-not (Test-Path $LatestPath)) {
    Write-Host "ERROR: no latest CSV at $LatestPath"
    Write-Host "  Run the autopilot first."
    exit 1
}

$BaselineDir = Split-Path -Parent $BaselinePath
if (-not (Test-Path $BaselineDir)) {
    New-Item -ItemType Directory -Path $BaselineDir -Force | Out-Null
}

Copy-Item $LatestPath $BaselinePath -Force
$rows = (Import-Csv $BaselinePath).Count
Write-Host "Approved: $TestName"
Write-Host "  $LatestPath -> $BaselinePath ($rows rows)"
