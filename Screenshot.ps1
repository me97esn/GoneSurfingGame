# ==========================================================
#  Screenshot.ps1
#  Launch the game windowed, wait for it to settle, and save a PNG of the
#  client area. For eyeballing UI/overlay work without an Android deploy.
#
#  Kills the editor first (GPU + file locks) and does NOT relaunch it --
#  run LaunchUnrealEditor.bat afterwards if you were using it.
#
#  Exit codes: 0 captured, 1 no game window / capture failed, 3 setup error.
#
#  Examples:
#    ./Screenshot.ps1 -Out shot.png
#    ./Screenshot.ps1 -ExtraArgs "-ShowInstructions" -ExecCmds "surf.start.holdpose 1" -Phone
# ==========================================================

param(
    # Defaults to GoneSurfing/Saved/Screenshots/shot-<timestamp>.png.
    [string]$Out = "",
    [string]$Map = $(if ($env:TEST_MAP) { $env:TEST_MAP } else { "Boards_on_flat_water" }),
    # Console commands, comma-separated. Values containing commas need single quotes
    # (same rule as RunGameAndCollectLogs -- comma is the separator).
    [string]$ExecCmds = "",
    # Extra launch args read at BeginPlay, space-separated (e.g. "-ShowInstructions").
    [string]$ExtraArgs = "",
    [int]$ResX = 1280,
    [int]$ResY = 720,
    # Phone-equivalent logical size. A 2400x1080 phone renders Slate through the overlays'
    # 2x SDPIScaler, leaving 1200x540 of widget space; on desktop the scaler is 1x, so this
    # window reproduces the device's text wrapping and vertical crowding. Use it whenever
    # the thing being checked is layout -- 16:9 is far more forgiving about height.
    [switch]$Phone,
    # Seconds to wait after the game window appears, for level load + overlay install.
    [int]$LoadSeconds = 25,
    # How long to wait for the game window itself (cold shader compile is slow).
    [int]$WindowTimeoutSeconds = 180,
    # Leave the game running after the capture (take more shots by hand).
    [switch]$KeepOpen
)

$ErrorActionPreference = 'Stop'

$UeRoot      = "E:\windowsgrejor\git\UnrealEngine"
$UePath      = Join-Path $UeRoot "Engine\Binaries\Win64\UnrealEditor.exe"
$ProjectPath = Join-Path $PSScriptRoot "GoneSurfing\GoneSurfing.uproject"

if (-not (Test-Path $UePath))      { Write-Host "ERROR: UnrealEditor.exe not found at $UePath"; exit 3 }
if (-not (Test-Path $ProjectPath)) { Write-Host "ERROR: project not found at $ProjectPath"; exit 3 }

if ($Phone) { $ResX = 1200; $ResY = 540 }

if (-not $Out) {
    $stamp = Get-Date -Format "yyyy-MM-dd-HH-mm-ss"
    $Out = Join-Path $PSScriptRoot "GoneSurfing\Saved\Screenshots\shot-$stamp.png"
}
$OutDir = Split-Path -Parent $Out
if ($OutDir -and -not (Test-Path $OutDir)) { New-Item -ItemType Directory -Force -Path $OutDir | Out-Null }

Add-Type @"
using System;
using System.Runtime.InteropServices;
public class ShotWin {
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
}
"@

Write-Host "=== Screenshot ==="
Write-Host "  - map:   $Map"
Write-Host "  - size:  ${ResX}x${ResY}$(if ($Phone) { ' (phone-equivalent)' })"

if ($ExecCmds)  { Write-Host "  - cmds:  $ExecCmds" }
if ($ExtraArgs) { Write-Host "  - args:  $ExtraArgs" }
Write-Host "  - out:   $Out"

Get-Process UnrealEditor -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 2

# NOTE: no -log. It opens a console window that becomes the process's MainWindowHandle, and you
# screenshot scrolling log text instead of the game. The log still lands in Saved/Logs.
$ueArgs = @(
    "`"$ProjectPath`"", "`"$Map`"",
    "-game", "-WINDOWED", "-ResX=$ResX", "-ResY=$ResY"
)
if ($ExecCmds)  { $ueArgs += "-ExecCmds=`"$ExecCmds`"" }
if ($ExtraArgs) { $ueArgs += $ExtraArgs.Split(" ", [StringSplitOptions]::RemoveEmptyEntries) }

try {
    $proc = Start-Process -FilePath $UePath -ArgumentList $ueArgs -PassThru -ErrorAction Stop
} catch {
    Write-Host "ERROR: failed to launch UnrealEditor: $_"
    exit 3
}

# The splash window shows up first and is the wrong size, so poll for a client rect that is
# actually the resolution we asked for rather than trusting the first MainWindowHandle we see.
$h = [IntPtr]::Zero; $w = 0; $hgt = 0
$deadline = (Get-Date).AddSeconds($WindowTimeoutSeconds)
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 2
    if ($proc.HasExited) { Write-Host "ERROR: game exited before a window appeared (exit $($proc.ExitCode))"; exit 1 }
    $proc.Refresh()
    if ($proc.MainWindowHandle -eq 0) { continue }
    $r = New-Object ShotWin+RECT
    if (-not [ShotWin]::GetClientRect($proc.MainWindowHandle, [ref]$r)) { continue }
    if (($r.R - $r.L) -ge ($ResX - 8)) {
        $h = $proc.MainWindowHandle; $w = $r.R - $r.L; $hgt = $r.B - $r.T; break
    }
}
if ($h -eq [IntPtr]::Zero) {
    Write-Host "ERROR: no game window within ${WindowTimeoutSeconds}s (cold shader compile? raise -WindowTimeoutSeconds)"
    try { $proc.Kill() } catch { }
    exit 1
}

Write-Host "  - window up; waiting ${LoadSeconds}s for load"
Start-Sleep -Seconds $LoadSeconds

# CopyFromScreen reads the desktop, so the window has to actually be on top. (PrintWindow would
# avoid that but comes back black on a D3D swapchain.)
[void][ShotWin]::ShowWindow($h, 9)   # SW_RESTORE
[void][ShotWin]::SetForegroundWindow($h)
# SetForegroundWindow is not enough on its own: anything marked always-on-top stays above the game
# and CopyFromScreen photographs IT instead. Measured 2026-08-31 - every shot came back with its
# left half black, in the shape of another app's rounded-corner window sitting over the game.
# HWND_TOPMOST (-1) puts the game above that; NOTOPMOST (-2) hands the desktop back afterwards.
# SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE = 0x0002|0x0001|0x0010.
[void][ShotWin]::SetWindowPos($h, [IntPtr](-1), 0, 0, 0, 0, 0x0013)
Start-Sleep -Seconds 3

$tl = New-Object ShotWin+POINT
[void][ShotWin]::ClientToScreen($h, [ref]$tl)

Add-Type -AssemblyName System.Drawing
$bmp = New-Object System.Drawing.Bitmap $w, $hgt
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($tl.X, $tl.Y, 0, 0, $bmp.Size)

# A window that never came to the front, or a frame captured before the first present, yields a
# flat image. Cheap to check, and it is otherwise indistinguishable from a real capture.
$corners = @($bmp.GetPixel(4, 4), $bmp.GetPixel($w - 5, 4), $bmp.GetPixel(4, $hgt - 5),
             $bmp.GetPixel($w - 5, $hgt - 5), $bmp.GetPixel([int]($w / 2), [int]($hgt / 2)))
$flat = ($corners | ForEach-Object { $_.ToArgb() } | Select-Object -Unique).Count -eq 1

$g.Dispose()
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()

if (-not $KeepOpen) {
    try { $proc.Kill() } catch { }
    Start-Sleep -Seconds 2
}

if ($flat) {
    Write-Host "WARNING: every sampled pixel is identical -- the window was probably not in front,"
    Write-Host "         or the capture beat the first rendered frame. Raise -LoadSeconds and retry."
    Write-Host "wrote $Out"
    exit 1
}

Write-Host "wrote $Out ($w x $hgt)"
Write-Host "NOTE: the editor was killed and not relaunched -- run LaunchUnrealEditor.bat if you need it."
exit 0
