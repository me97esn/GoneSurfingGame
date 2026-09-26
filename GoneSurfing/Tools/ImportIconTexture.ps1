# ==========================================================
#  ImportIconTexture.ps1
#  SVG (or PNG) -> tintable UE texture, in one step.
#
#  Renders the SVG with Inkscape (drawing bounds, transparent ground), turns every pixel WHITE
#  while keeping its alpha, and imports the result as /Game/<DestPath>/<Name> through the
#  ImportAssets commandlet. The game tints the texture at draw time (MakeBox multiplies colour),
#  so white-with-alpha is the only form that lets the code own the colour - a black shape stays
#  black whatever tint it is given. Draw in black in Inkscape (readable on the page); this whitens.
#
#  Group/layer opacity in the SVG survives as per-pixel alpha, which is how the pump button's
#  50%-ghost pose works. Hidden layers (display:none) are not rendered.
#
#  Kills the editor first: once the editor has loaded the texture it holds the .uasset open and the
#  commandlet's save fails with a sharing violation (error 32). Same convention as the build
#  scripts; the Stop hook relaunches it on a dirty tree.
#
#  Examples:
#    ./Tools/ImportIconTexture.ps1 -Svg "images/pump button.svg" -Name T_PumpIcon
#    ./Tools/ImportIconTexture.ps1 -Png Saved/some.png -Name T_Thing -DestPath Images -Width 256
#
#  Exit codes: 0 imported, 1 render/import failed, 3 setup error.
# ==========================================================
param(
    [string]$Svg = "",
    [string]$Png = "",
    [Parameter(Mandatory = $true)][string]$Name,
    [string]$DestPath = "Images",
    # Longest edge of the export. The pump disc is ~540 px across on a 1080p-tall phone and the
    # glyph ~62% of that, so 512 is crisp there with a mip to spare; the cooked texture is ~0.2 MB.
    [int]$Width = 512,
    [string]$Inkscape = "C:\Program Files\Inkscape\bin\inkscape.exe"
)

$ErrorActionPreference = 'Stop'
$Project = Resolve-Path (Join-Path $PSScriptRoot "..\GoneSurfing.uproject")
$UeCmd   = "E:\windowsgrejor\git\UnrealEngine\Engine\Binaries\Win64\UnrealEditor-Cmd.exe"
$Work    = Join-Path $PSScriptRoot "..\Saved\IconImport"
New-Item -ItemType Directory -Force $Work | Out-Null

if (-not (Test-Path $UeCmd)) { Write-Host "ERROR: $UeCmd not found"; exit 3 }
if (-not $Svg -and -not $Png) { Write-Host "ERROR: pass -Svg or -Png"; exit 3 }

# ---- 0. the editor holds loaded assets open ----------------------------------------------------
Get-Process UnrealEditor, LiveCodingConsole -ErrorAction SilentlyContinue | Stop-Process -Force -Confirm:$false
Start-Sleep -Seconds 2

# ---- 1. render -----------------------------------------------------------------------------------
$raw = Join-Path $Work "$Name.raw.png"
if ($Svg) {
    if (-not (Test-Path $Inkscape)) { Write-Host "ERROR: Inkscape not at $Inkscape"; exit 3 }
    $SvgAbs = (Resolve-Path $Svg).Path
    & $Inkscape $SvgAbs --export-area-drawing --export-type=png --export-width=$Width `
        --export-background-opacity=0 --export-filename=$raw 2>&1 | Out-Null
    if (-not (Test-Path $raw)) { Write-Host "ERROR: Inkscape produced nothing"; exit 1 }
    Write-Host "rendered  $Svg -> $raw"
} else {
    Copy-Item (Resolve-Path $Png).Path $raw -Force
}

# ---- 2. whiten, keep alpha -----------------------------------------------------------------------
Add-Type -AssemblyName System.Drawing
$bmp = [System.Drawing.Bitmap]::FromFile($raw)
$w = $bmp.Width; $h = $bmp.Height
$rect = New-Object System.Drawing.Rectangle 0, 0, $w, $h
$fmt  = [System.Drawing.Imaging.PixelFormat]::Format32bppArgb
$data = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly, $fmt)
$bytes = New-Object byte[] ($data.Stride * $h)
[System.Runtime.InteropServices.Marshal]::Copy($data.Scan0, $bytes, 0, $bytes.Length)
$bmp.UnlockBits($data)
for ($i = 0; $i -lt $bytes.Length; $i += 4) { $bytes[$i] = 255; $bytes[$i + 1] = 255; $bytes[$i + 2] = 255 }
$out = New-Object System.Drawing.Bitmap $w, $h, $fmt
$od = $out.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::WriteOnly, $fmt)
[System.Runtime.InteropServices.Marshal]::Copy($bytes, 0, $od.Scan0, $bytes.Length)
$out.UnlockBits($od)
$final = Join-Path $Work "$Name.png"
$out.Save($final, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose(); $out.Dispose()
Write-Host "whitened  ${w}x${h} -> $final"

# ---- 3. import -----------------------------------------------------------------------------------
$json = Join-Path $Work "$Name.import.json"
$finalFwd = $final -replace '\\', '/'
@"
{ "ImportGroups": [ {
    "GroupName": "$Name",
    "FileNames": [ "$finalFwd" ],
    "DestinationPath": "/Game/$DestPath",
    "bReplaceExisting": true,
    "bSkipReadOnly": false,
    "FactoryName": "TextureFactory"
} ] }
"@ | Set-Content -Encoding ASCII $json

& $UeCmd $Project -run=ImportAssets "-importsettings=$json" -AllowCommandletRendering -unattended -nopause -nosplash -NullRHI 2>&1 | Out-Null
$asset = Join-Path $PSScriptRoot "..\Content\$DestPath\$Name.uasset"
if (-not (Test-Path $asset) -or ((Get-Item $asset).LastWriteTime -lt (Get-Item $final).LastWriteTime)) {
    Write-Host "ERROR: $asset was not (re)written - see Saved/Logs for the commandlet output"; exit 1
}
Write-Host "imported  /Game/$DestPath/$Name  ($([math]::Round((Get-Item $asset).Length / 1KB)) KB)"
exit 0
