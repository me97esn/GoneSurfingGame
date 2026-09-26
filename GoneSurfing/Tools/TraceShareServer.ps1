# Start the listener the phone's SEND panel posts ride traces to (specs/share-trace-from-phone.md).
#
#   .\Tools\TraceShareServer.ps1            # port 8765
#   .\Tools\TraceShareServer.ps1 -Port 9000
#
# First run on a machine: opens the port in Windows Firewall (needs an elevated shell once; if this
# one is not, it prints the command to run instead of failing) and prints the PC's Tailscale address,
# which is what goes into DefaultEngine.ini as surf.traceshare.url. Then runs the Python server in
# the foreground; Ctrl+C stops it. Arrivals print here and land in Saved/InputTraces/fromphone/.
param(
    [int]$Port = 8765
)

$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$project = Split-Path -Parent $here

# Firewall: one inbound rule for the port, any profile. Tailscale traffic arrives on its own
# adapter, which Windows usually classes as Public, so the rule must not be Private-only.
$ruleName = "GoneSurfing TraceShare $Port"
$existing = Get-NetFirewallRule -DisplayName $ruleName -ErrorAction SilentlyContinue
if (-not $existing) {
    $isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
        ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    if ($isAdmin) {
        New-NetFirewallRule -DisplayName $ruleName -Direction Inbound -Protocol TCP -LocalPort $Port -Action Allow | Out-Null
        Write-Host "Firewall: opened TCP $Port inbound ($ruleName)."
    } else {
        Write-Host "Firewall: TCP $Port is not open yet. Run once in an ELEVATED PowerShell:" -ForegroundColor Yellow
        Write-Host "  New-NetFirewallRule -DisplayName '$ruleName' -Direction Inbound -Protocol TCP -LocalPort $Port -Action Allow" -ForegroundColor Yellow
        Write-Host "(The server still starts; sends from the phone will fail until the rule exists.)"
    }
}

# The address the phone needs. Tailscale's CLI lives next to the app; absent = not installed.
$ts = Get-Command tailscale -ErrorAction SilentlyContinue
if (-not $ts -and (Test-Path "$env:ProgramFiles\Tailscale\tailscale.exe")) {
    $ts = Get-Command "$env:ProgramFiles\Tailscale\tailscale.exe"
}
if ($ts) {
    $ip = (& $ts.Source ip -4 2>$null | Select-Object -First 1)
    if ($ip) {
        Write-Host "Tailscale: this PC is $ip -> surf.traceshare.url=`"http://${ip}:$Port/trace`"" -ForegroundColor Cyan
    } else {
        Write-Host "Tailscale is installed but reports no IPv4 address - is it signed in and connected?" -ForegroundColor Yellow
    }
} else {
    Write-Host "Tailscale not found. Install it on this PC and the phone (same account) so the phone can reach this port from mobile data." -ForegroundColor Yellow
}

# python3 first: on this machine plain `python` is a 2.7 left on PATH, and the server is 3.x code.
$py = Get-Command python3 -ErrorAction SilentlyContinue
if (-not $py) { $py = Get-Command py -ErrorAction SilentlyContinue; $pyArgs = @("-3") } else { $pyArgs = @() }
if (-not $py) { throw "python3 not found on PATH" }

& $py.Source @pyArgs (Join-Path $here "TraceShareServer.py") --port $Port --project $project
