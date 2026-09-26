# Promote a board's live tuning overlay into its shipped profile.
#
# Tuning happens in four layers (specs/board-selection.md): compiled defaults, the board profile,
# the global overrides file, and finally the board's own overlay - the only one the in-game HUD
# writes, and the only one that is writable on the phone at all, because Content/Boards is staged
# INTO THE PAK and is read-only on device.
#
# So the overlay is exactly the diff you have dialled in but not yet committed to the board's
# identity. This promotes it: merge the overlay into Content/Boards/<n>-<id>.json and clear it, so
# the board now ships with those numbers and the overlay starts empty again for the next round.
#
#   pwsh Tools/PromoteBoardTuning.ps1 -Board shortboard              # from this PC's Saved/
#   pwsh Tools/PromoteBoardTuning.ps1 -Board shortboard -FromDevice  # pull off the phone first
#   pwsh Tools/PromoteBoardTuning.ps1 -Board shortboard -WhatIf      # show the merge, change nothing
#
# The overlay is archived beside the profile as <id>.promoted-<stamp>.json rather than deleted, so a
# promotion can be undone by hand if the numbers turn out worse on the next ride.
param(
    [Parameter(Mandatory = $true)][string]$Board,
    [switch]$FromDevice,
    [switch]$WhatIf
)
$ErrorActionPreference = "Stop"

$projectRoot = Split-Path -Parent $PSScriptRoot
$overlayDir  = Join-Path $projectRoot "Saved\BoardTuning"
$overlay     = Join-Path $overlayDir "$Board.json"
$boardsDir   = Join-Path $projectRoot "Content\Boards"

# --- optionally fetch the overlay off the phone --------------------------------------------------
if ($FromDevice) {
    $pkg  = "com.dsh.gonesurfing"
    $base = "/storage/emulated/0/Android/data/$pkg/files"

    $dev = (& adb devices) | Where-Object { $_ -match '^\S+\s+device\b' }
    if (-not $dev) { Write-Error "No adb device connected (run 'adb devices'; 'adb kill-server' often fixes it)."; exit 1 }

    # Same discovery as PullInputTraces.ps1: the device path has a doubled project folder and varies.
    $dir = (& adb shell "find '$base' -type d -iname BoardTuning") |
           ForEach-Object { $_.Trim() } | Where-Object { $_ } | Select-Object -First 1
    if (-not $dir) { Write-Error "No BoardTuning directory on the device - has the HUD written anything yet?"; exit 1 }

    New-Item -ItemType Directory -Force -Path $overlayDir | Out-Null
    & adb pull "$dir/$Board.json" "$overlay" | Out-Null
    if (-not (Test-Path $overlay)) { Write-Error "Device has no overlay for '$Board' at $dir."; exit 1 }
    Write-Host "pulled $dir/$Board.json"
}

if (-not (Test-Path $overlay)) {
    Write-Error "No overlay at $overlay. Nothing to promote - tune the board first, or pass -FromDevice."
    exit 1
}

# --- locate the profile (files are numbered for rack order, so match on the id) -------------------
$profile = Get-ChildItem -Path $boardsDir -Filter "*.json" | Where-Object {
    (Get-Content $_.FullName -Raw | ConvertFrom-Json).id -eq $Board
} | Select-Object -First 1
if (-not $profile) { Write-Error "No board profile with id '$Board' in $boardsDir."; exit 1 }

$overlayJson = Get-Content $overlay -Raw | ConvertFrom-Json
$profileJson = Get-Content $profile.FullName -Raw | ConvertFrom-Json

$changes = @()
foreach ($prop in $overlayJson.PSObject.Properties) {
    $was = if ($profileJson.tuning.PSObject.Properties.Name -contains $prop.Name) {
        $profileJson.tuning.($prop.Name)
    } else { "(default)" }
    $changes += [pscustomobject]@{ Key = $prop.Name; From = $was; To = $prop.Value }
}

if ($changes.Count -eq 0) {
    Write-Host "Overlay for '$Board' is empty - nothing to promote."
    exit 0
}

Write-Host ""
Write-Host "Promoting into $($profile.Name):"
$changes | Format-Table -AutoSize
if ($WhatIf) { Write-Host "-WhatIf: nothing written."; exit 0 }

foreach ($c in $changes) {
    if ($profileJson.tuning.PSObject.Properties.Name -contains $c.Key) {
        $profileJson.tuning.($c.Key) = $c.To
    } else {
        $profileJson.tuning | Add-Member -NotePropertyName $c.Key -NotePropertyValue $c.To
    }
}

# Depth 10: the profile nests ratings and tuning, and the default depth of 2 would flatten them
# into "System.Object[]" - silently destroying the file.
$profileJson | ConvertTo-Json -Depth 10 | Set-Content -Path $profile.FullName -Encoding utf8
Write-Host "wrote $($profile.FullName)"

$stamp   = Get-Date -Format "yyyyMMdd-HHmmss"
$archive = Join-Path $overlayDir "$Board.promoted-$stamp.json"
Move-Item -Path $overlay -Destination $archive -Force
Write-Host "overlay archived as $(Split-Path -Leaf $archive) - the board now ships these values."
Write-Host ""
Write-Host "Restart the game to ride the promoted profile with an empty overlay."
