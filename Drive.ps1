# ==========================================================
#  Drive.ps1
#  Launch the game windowed at phone-equivalent size (1200x540, like Screenshot.ps1 -Phone), then
#  walk a scripted list of steps against the live window: "click x y", "drag x0 y0 x1 y1" (a
#  held mouse drag over ~0.4 s - drives the touch pads, since mouse down/up is touch start/end
#  on desktop), "wait s", "shot name".
#  Coordinates are client pixels; screenshots land in GoneSurfing/Saved/Screenshots/<name>.png.
#  For eyeballing multi-screen flows (hub -> ride -> wipeout card -> replay ...) without a device
#  and without a human at the mouse. Synthetic clicks reach viewport Slate on desktop -game even
#  though the log says the cursor is hidden - verified 2026-09-14 (specs/two-screen-navigation.md).
#
#  Kills the editor first and does NOT relaunch it.
#
#  Example:
#    ./Drive.ps1 -Steps "click 600 381; wait 24; shot card; click 543 331; wait 8; shot hub"
#    ./Drive.ps1 -Map Boards_on_flat_water -LoadSeconds 10 -ExecCmds "surf.input.touchui 2" `
#                -Steps "drag 144 270 220 270; wait 1; shot latched; click 144 270; wait 1; shot cleared"
#  The game log is flushed (-ForceLogFlush), so Saved/Logs/GoneSurfing.log is complete afterwards.
# ==========================================================
param(
    [string]$Map = "Surfing_infinite_wave",
    [string]$Steps = "",
    [int]$LoadSeconds = 30,
    # Console commands at launch, comma-separated (single-quote values that contain commas).
    [string]$ExecCmds = ""
)
$ErrorActionPreference = 'Stop'
$UePath = "E:\windowsgrejor\git\UnrealEngine\Engine\Binaries\Win64\UnrealEditor.exe"
$ProjectPath = "E:\windowsgrejor\git\GoneSurfingUE5\GoneSurfing\GoneSurfing.uproject"
$OutDir = "E:\windowsgrejor\git\GoneSurfingUE5\GoneSurfing\Saved\Screenshots"
$ResX = 1200; $ResY = 540

Add-Type @"
using System;
using System.Runtime.InteropServices;
public class DrvWin {
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint dx, uint dy, uint data, UIntPtr extra);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
}
"@
Add-Type -AssemblyName System.Drawing

Get-Process UnrealEditor -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 2
$ueArgs = @("`"$ProjectPath`"", "`"$Map`"", "-game", "-WINDOWED", "-ResX=$ResX", "-ResY=$ResY", "-ForceLogFlush")
if ($ExecCmds) { $ueArgs += "-ExecCmds=`"$ExecCmds`"" }
$proc = Start-Process -FilePath $UePath -ArgumentList $ueArgs -PassThru

$h = [IntPtr]::Zero; $w = 0; $hgt = 0
$deadline = (Get-Date).AddSeconds(180)
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 2
    if ($proc.HasExited) { Write-Host "ERROR: exited"; exit 1 }
    $proc.Refresh()
    if ($proc.MainWindowHandle -eq 0) { continue }
    $r = New-Object DrvWin+RECT
    if (-not [DrvWin]::GetClientRect($proc.MainWindowHandle, [ref]$r)) { continue }
    if (($r.R - $r.L) -ge ($ResX - 8)) { $h = $proc.MainWindowHandle; $w = $r.R - $r.L; $hgt = $r.B - $r.T; break }
}
if ($h -eq [IntPtr]::Zero) { Write-Host "ERROR: no window"; try { $proc.Kill() } catch {}; exit 1 }
Write-Host "window up; waiting $LoadSeconds s"
Start-Sleep -Seconds $LoadSeconds
[void][DrvWin]::ShowWindow($h, 9)
[void][DrvWin]::SetForegroundWindow($h)
[void][DrvWin]::SetWindowPos($h, [IntPtr](-1), 0, 0, 0, 0, 0x0013)
Start-Sleep -Seconds 2

function Origin { $p = New-Object DrvWin+POINT; [void][DrvWin]::ClientToScreen($h, [ref]$p); return $p }
function Shot($name) {
    # Re-assert foreground before every shot, not just once at startup. A modal that takes the
    # game's mouse capture (the wipeout card does) can let the window fall behind whatever was
    # there before, and CopyFromScreen then silently photographs THAT - a screenshot of the editor
    # that looks like a game that never opened its card. Hit 2026-09-16.
    [void][DrvWin]::ShowWindow($h, 9)
    [void][DrvWin]::SetForegroundWindow($h)
    [void][DrvWin]::SetWindowPos($h, [IntPtr](-1), 0, 0, 0, 0, 0x0013)
    Start-Sleep -Milliseconds 400
    $tl = Origin
    $bmp = New-Object System.Drawing.Bitmap $w, $hgt
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($tl.X, $tl.Y, 0, 0, $bmp.Size)
    $g.Dispose()
    $path = Join-Path $OutDir "$name.png"
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
    Write-Host "shot $path"
}
function Click($x, $y) {
    $tl = Origin
    [void][DrvWin]::SetCursorPos($tl.X + $x, $tl.Y + $y)
    Start-Sleep -Milliseconds 150
    [DrvWin]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)   # down
    Start-Sleep -Milliseconds 80
    [DrvWin]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)   # up
    Write-Host "click $x,$y"
}

function Drag($x0, $y0, $x1, $y1) {
    $tl = Origin
    [void][DrvWin]::SetCursorPos($tl.X + $x0, $tl.Y + $y0)
    Start-Sleep -Milliseconds 150
    [DrvWin]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)   # down
    $n = 16
    for ($i = 1; $i -le $n; $i++) {
        $t = $i / $n
        [void][DrvWin]::SetCursorPos($tl.X + [int]($x0 + ($x1 - $x0) * $t), $tl.Y + [int]($y0 + ($y1 - $y0) * $t))
        Start-Sleep -Milliseconds 25
    }
    Start-Sleep -Milliseconds 150
    [DrvWin]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)   # up
    Write-Host "drag $x0,$y0 -> $x1,$y1"
}

foreach ($step in $Steps.Split(";", [StringSplitOptions]::RemoveEmptyEntries)) {
    $parts = $step.Trim().Split(" ", [StringSplitOptions]::RemoveEmptyEntries)
    switch ($parts[0]) {
        "click" { Click ([int]$parts[1]) ([int]$parts[2]) }
        "drag"  { Drag ([int]$parts[1]) ([int]$parts[2]) ([int]$parts[3]) ([int]$parts[4]) }
        "wait"  { Start-Sleep -Seconds ([double]$parts[1]) }
        "shot"  { Shot $parts[1] }
    }
}
try { $proc.Kill() } catch {}
Start-Sleep -Seconds 2
