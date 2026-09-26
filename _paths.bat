@echo off
REM ==========================================================
REM  _paths.bat
REM  Shared path/configuration for BuildAndLaunch.bat and
REM  RunGameAndCollectLogs.bat. Sourced via:  call _paths.bat
REM ==========================================================

set UE_ROOT=E:\windowsgrejor\git\UnrealEngine
set UE_PATH=%UE_ROOT%\Engine\Binaries\Win64\UnrealEditor.exe
set BUILD_BAT=%UE_ROOT%\Engine\Build\BatchFiles\Build.bat
set PROJECT_PATH=E:\windowsgrejor\git\GoneSurfingUE5\GoneSurfing\GoneSurfing.uproject
set EDITOR_TARGET=GoneSurfingEditor
set LOG_PATH=E:\windowsgrejor\git\GoneSurfingUE5\GoneSurfing\Saved\Logs\GoneSurfing.log
set DDC_PATH=E:\windowsgrejor\git\GoneSurfingUE5\GoneSurfing\Saved\DDC
set TEMP_PATH=E:\windowsgrejor\git\GoneSurfingUE5\GoneSurfing\Saved\Temp

REM Default test map (can be overridden by setting TEST_MAP before invoking)
if "%TEST_MAP%"=="" set TEST_MAP=Boards_on_flat_water
