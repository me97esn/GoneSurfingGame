# ==========================================================
#  RecordVideo.ps1
#  Launch the game windowed and record its client area to MP4 with ffmpeg.
#  The moving-picture sibling of Screenshot.ps1, and it inherits that script's
#  launch/window-find logic for the same reasons (see the skill: record-game-video).
#
#  Needs ffmpeg. Install once:  winget install --id Gyan.FFmpeg -e
#
#  Exit codes: 0 recorded, 1 no game window / capture failed, 3 setup error.
#
#  Examples:
#    ./RecordVideo.ps1                                     # scripted autopilot ride
#    ./RecordVideo.ps1 -ReplayTrace phone-2026-09-05-19-39-42.csv
#    ./RecordVideo.ps1 -RecordSeconds 15 -Web              # + a small web-embeddable copy
# ==========================================================
param(
    # Defaults to GoneSurfing/Saved/Videos/ride-<timestamp>.mp4
    [string]$Out         = "",
    [string]$Map         = $(if ($env:TEST_MAP) { $env:TEST_MAP } else { "Surfing_infinite_wave" }),
    # Something has to drive the board or you film a board sitting still.
    # Either a scripted autopilot by TestName, or a recorded player trace.
    [string]$Autopilot   = "surfing-down-the-line",
    [string]$ReplayTrace = "",
    [string]$Board       = "shortboard",
    [string]$ExecCmds    = "",
    [string]$ExtraArgs   = "",
    [int]$ResX = 1280,
    [int]$ResY = 720,
    # Seconds to wait after the window appears: level load, shader compile, pop-up.
    [int]$LoadSeconds   = 30,
    [int]$RecordSeconds = 26,
    [int]$Fps = 30,
    # Also write <out>-web.mp4, re-encoded to fit an Artifact data URI (see the skill).
    [switch]$Web,
    [switch]$KeepOpen,
    # Write ffmpeg's start time (UTC, o-format) here, so a take can be cut against the game log's
    # wall-clock frame stamps (e.g. "trace t=5" = the CROSSING line at handoff frame + 300).
    [string]$StampFile = ""
)
$ErrorActionPreference = 'Stop'

$UeRoot      = "E:\windowsgrejor\git\UnrealEngine"
$UePath      = Join-Path $UeRoot "Engine\Binaries\Win64\UnrealEditor.exe"
$ProjectPath = Join-Path $PSScriptRoot "GoneSurfing\GoneSurfing.uproject"

# winget's shim is only on PATH for shells started after the install, so find the exe directly.
$FFmpeg = (Get-Command ffmpeg -ErrorAction SilentlyContinue).Source
if (-not $FFmpeg) {
    $FFmpeg = (Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Recurse -Filter ffmpeg.exe -ErrorAction SilentlyContinue |
               Select-Object -First 1).FullName
}
if (-not $FFmpeg)                  { Write-Host "ERROR: ffmpeg not found. winget install --id Gyan.FFmpeg -e"; exit 3 }
if (-not (Test-Path $UePath))      { Write-Host "ERROR: UnrealEditor.exe not found at $UePath"; exit 3 }
if (-not (Test-Path $ProjectPath)) { Write-Host "ERROR: project not found at $ProjectPath"; exit 3 }

if (-not $Out) {
    $stamp = Get-Date -Format "yyyy-MM-dd-HH-mm-ss"
    $Out = Join-Path $PSScriptRoot "GoneSurfing\Saved\Videos\ride-$stamp.mp4"
}
$OutDir = Split-Path -Parent $Out
if ($OutDir -and -not (Test-Path $OutDir)) { New-Item -ItemType Directory -Force -Path $OutDir | Out-Null }

Add-Type @"
using System;
using System.Runtime.InteropServices;
public class RecWin {
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
}
"@

Write-Host "=== RecordVideo ==="
Write-Host "  - map:    $Map"
Write-Host "  - driver: $(if ($ReplayTrace) { "trace $ReplayTrace" } else { "autopilot $Autopilot" })"
Write-Host "  - out:    $Out"

Get-Process UnrealEditor -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 2

# No -log: its console window becomes MainWindowHandle and you film scrolling text.
$cmds = @()
if ($Autopilot -and -not $ReplayTrace) { $cmds += "surf.autopilots '$Autopilot'" }
if ($ExecCmds) { $cmds += $ExecCmds }

$ueArgs = @("`"$ProjectPath`"", "`"$Map`"", "-game", "-WINDOWED", "-ResX=$ResX", "-ResY=$ResY")
if ($cmds.Count -gt 0) { $ueArgs += "-ExecCmds=`"$($cmds -join ', ')`"" }
if ($ReplayTrace)      { $ueArgs += "-ReplayTrace=$ReplayTrace" }
if ($Board)            { $ueArgs += @("-BoardInTests", "-Board=$Board") }
if ($ExtraArgs)        { $ueArgs += $ExtraArgs.Split(" ", [StringSplitOptions]::RemoveEmptyEntries) }

$proc = Start-Process -FilePath $UePath -ArgumentList $ueArgs -PassThru

# The splash window appears first and reports a wrong-size client rect, so poll for the
# size actually asked for rather than trusting the first MainWindowHandle.
$h = [IntPtr]::Zero
$deadline = (Get-Date).AddSeconds(180)
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 2
    if ($proc.HasExited) { Write-Host "ERROR: game exited before a window appeared ($($proc.ExitCode))"; exit 1 }
    $proc.Refresh()
    if ($proc.MainWindowHandle -eq 0) { continue }
    $r = New-Object RecWin+RECT
    if (-not [RecWin]::GetClientRect($proc.MainWindowHandle, [ref]$r)) { continue }
    if (($r.R - $r.L) -ge ($ResX - 8)) { $h = $proc.MainWindowHandle; break }
}
if ($h -eq [IntPtr]::Zero) { Write-Host "ERROR: no game window in 180s (cold shader compile?)"; try { $proc.Kill() } catch {}; exit 1 }

Write-Host "  - window up; waiting ${LoadSeconds}s for load"
Start-Sleep -Seconds $LoadSeconds

# gdigrab captures a SCREEN REGION, not a window, so whatever is on top gets filmed. Windows refuses
# SetForegroundWindow to a process that does not already own the foreground, and when it refuses you
# silently record someone else's window -- so verify, retry, and fail loudly rather than film the
# wrong thing. Minimize+restore is what usually grants foreground rights back.
$gotFocus = $false
foreach ($try in 1..6) {
    [void][RecWin]::SetWindowPos($h, [IntPtr](-1), 0, 0, 0, 0, 0x0002 -bor 0x0001)   # HWND_TOPMOST
    [void][RecWin]::SetForegroundWindow($h)
    Start-Sleep -Milliseconds 400
    if ([RecWin]::GetForegroundWindow() -eq $h) { $gotFocus = $true; break }
    [void][RecWin]::ShowWindow($h, 6)   # SW_MINIMIZE
    Start-Sleep -Milliseconds 250
    [void][RecWin]::ShowWindow($h, 9)   # SW_RESTORE
    Start-Sleep -Milliseconds 400
    if ([RecWin]::GetForegroundWindow() -eq $h) { $gotFocus = $true; break }
    Write-Host "  - foreground attempt $try failed, retrying"
}
if (-not $gotFocus) {
    Write-Host "ERROR: could not bring the game window to the foreground after 6 attempts."
    Write-Host "       Recording now would capture whatever is on top instead of the game."
    try { $proc.Kill() } catch {}
    exit 1
}
Start-Sleep -Milliseconds 700

$r = New-Object RecWin+RECT
[void][RecWin]::GetClientRect($h, [ref]$r)
$p = New-Object RecWin+POINT          # fresh POINT: ClientToScreen converts IN PLACE
[void][RecWin]::ClientToScreen($h, [ref]$p)
$w  = ($r.R - $r.L) - (($r.R - $r.L) % 2)   # even dimensions or yuv420p refuses
$hh = ($r.B - $r.T) - (($r.B - $r.T) % 2)
Write-Host "  - recording ${w}x${hh} at ($($p.X),$($p.Y)) for ${RecordSeconds}s"

# Winning the foreground once is not enough: something else (an IDE, a terminal printing output)
# takes it back mid-recording and you silently film that instead. gdigrab reads a screen REGION, and
# window-DC capture (-i title=...) fails outright on this D3D swapchain, so the only robust answer is
# to hold the window on top for the whole take. Run ffmpeg async and re-assert HWND_TOPMOST while it
# records -- SWP_NOACTIVATE so we pin z-order without yanking focus every 200 ms.
$ffArgs = @(
    "-hide_banner", "-loglevel", "error", "-y",
    "-f", "gdigrab", "-framerate", "$Fps",
    "-offset_x", "$($p.X)", "-offset_y", "$($p.Y)", "-video_size", "${w}x${hh}",
    "-draw_mouse", "0", "-t", "$RecordSeconds", "-i", "desktop",
    "-c:v", "libx264", "-preset", "medium", "-crf", "24",
    "-pix_fmt", "yuv420p", "-movflags", "+faststart", "$Out"
)
$ff = Start-Process -FilePath $FFmpeg -ArgumentList $ffArgs -PassThru -NoNewWindow
if ($StampFile) { (Get-Date).ToUniversalTime().ToString("o") | Set-Content -Encoding ascii $StampFile }
while (-not $ff.HasExited) {
    [void][RecWin]::SetWindowPos($h, [IntPtr](-1), 0, 0, 0, 0, 0x0002 -bor 0x0001 -bor 0x0010)
    Start-Sleep -Milliseconds 200
}

if (-not $KeepOpen) { try { $proc.Kill() } catch {} }

if (-not (Test-Path $Out)) { Write-Host "ERROR: no output written"; exit 1 }
Write-Host ("  - wrote $Out ({0:N2} MB)" -f ((Get-Item $Out).Length / 1MB))

if ($Web) {
    $webOut = [IO.Path]::ChangeExtension($Out, $null) + "-web.mp4"
    & $FFmpeg -hide_banner -loglevel error -y -i $Out -vf "fps=24" `
        -c:v libx264 -preset slow -crf 27 -pix_fmt yuv420p -an -movflags +faststart $webOut
    $mb = (Get-Item $webOut).Length / 1MB
    Write-Host ("  - wrote $webOut ({0:N2} MB, ~{1:N1} MB as base64)" -f $mb, ($mb * 1.37))
}

Write-Host "  - editor was killed and NOT relaunched: cmd /c LaunchUnrealEditor.bat 1"
