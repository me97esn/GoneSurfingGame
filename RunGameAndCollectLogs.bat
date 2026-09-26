@echo off
setlocal

REM ==========================================================
REM  RunGameAndCollectLogs.bat
REM  Thin wrapper around RunGameAndCollectLogs.ps1 — see
REM  GoneSurfing\specs\run-game-and-collect-logs.md.
REM
REM  Usage:
REM    RunGameAndCollectLogs.bat                                  (no debug overrides)
REM    RunGameAndCollectLogs.bat thrust                           (flags only)
REM    RunGameAndCollectLogs.bat thrust,buoyancy:tail,left_middle (flags:actors)
REM    RunGameAndCollectLogs.bat thrust:bottom:surfing-down-the-line
REM                                                               (flags:actors:autopilot-filter)
REM    RunGameAndCollectLogs.bat ::surfing-down-the-line          (autopilot filter only)
REM ==========================================================

REM Pull shared paths/configuration into our environment.
call "%~dp0_paths.bat"

REM Argument is forwarded as-is to the .ps1 script.
set ARG=%~1

REM PowerShell with -NoProfile for predictable behavior; -ExecutionPolicy
REM Bypass since this script is local and signed only by the dev. The
REM exit code from PowerShell propagates as the bat's exit code.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0RunGameAndCollectLogs.ps1" -Argument "%ARG%"
exit /b %ERRORLEVEL%
