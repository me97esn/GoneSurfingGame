@echo off
setlocal
REM ==========================================================
REM  LaunchForLive.bat
REM  Launch the game with Unreal's Remote Control HTTP server
REM  running, so Tools/mcp/surf_live.py can reach it.
REM
REM  Unlike _paths.bat, the project is derived from THIS script's
REM  location, so it always launches the worktree it lives in.
REM  (_paths.bat hard-codes the main worktree's .uproject, which
REM  is wrong for any other checkout.)
REM
REM  -game is required: USurfTuningSubsystem is a GameInstance
REM  subsystem, so it does not exist in an editor with no PIE
REM  session running.
REM
REM  -RCWebControlEnable is required too: the plugin refuses to
REM  serve outside the editor without it.
REM
REM  Usage:  LaunchForLive.bat [MapName]
REM ==========================================================

set UE_ROOT=E:\windowsgrejor\git\UnrealEngine
set UE_EXE=%UE_ROOT%\Engine\Binaries\Win64\UnrealEditor.exe
set PROJECT=%~dp0..\..\GoneSurfing.uproject
set MAP=%1
if "%MAP%"=="" set MAP=Boards_on_flat_water

if not exist "%UE_EXE%" (
    echo ERROR: editor not found at %UE_EXE%
    exit /b 2
)
if not exist "%PROJECT%" (
    echo ERROR: project not found at %PROJECT%
    exit /b 2
)

REM No surf.autopilots filter is set, so bAutoQuitOnComplete stays
REM inert and the game keeps running after the scripted intro --
REM which is what you want while poking values at it.
echo Launching %PROJECT% (%MAP%) with Remote Control on :30010
start "" "%UE_EXE%" "%PROJECT%" %MAP% -game -windowed -resx=1280 -resy=720 ^
    -log -stdout -RCWebControlEnable -ExecCmds="WebControl.StartServer"

echo.
echo Then, from Tools/mcp:
echo   python3 -c "import surf_live as s; print(s.dumps(s.status()))"
endlocal
