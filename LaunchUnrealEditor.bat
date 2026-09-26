@echo off
REM Launch Unreal Editor directly from E: drive without VS Code
REM This avoids D: drive activity during packaging

echo ============================================
echo Unreal Engine Launcher (Custom Build)
echo ============================================
echo.

REM Set paths
set UE_ROOT=E:\windowsgrejor\git\UnrealEngine
set UE_PATH=%UE_ROOT%\Engine\Binaries\Win64\UnrealEditor.exe
set PROJECT_PATH=E:\windowsgrejor\git\GoneSurfingUE5\GoneSurfing\GoneSurfing.uproject

REM Non-interactive use (agents, hooks, scripts): pass the menu choice as an
REM argument -- "LaunchUnrealEditor.bat 1" launches straight away with no prompt.
REM Without an argument the menu below is shown, and `set /p` BLOCKS FOREVER when
REM stdin is not a real console: the cmd window just sits at "Enter your choice"
REM and the caller never returns. Always pass 1 from a script.
set "choice=%~1"
if not "%choice%"=="" goto dispatch

echo Options:
echo [1] Launch Unreal Editor directly
echo [2] Rebuild Engine then Launch
echo [3] Rebuild Project only then Launch
echo [4] Exit
echo.

set /p choice="Enter your choice (1-4): "

:dispatch
if "%choice%"=="1" goto launch
if "%choice%"=="2" goto rebuild_engine
if "%choice%"=="3" goto rebuild_project
if "%choice%"=="4" goto end
echo Invalid choice. Exiting.
goto end

:rebuild_engine
echo.
echo Rebuilding Unreal Engine...
echo This may take a while...
echo.
cd /d "%UE_ROOT%"
call GenerateProjectFiles.bat
echo.
echo Building engine with Visual Studio...
"C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" "%UE_ROOT%\Engine\Source\UE5.sln" /t:Build /p:Configuration=Development /p:Platform=Win64
if %ERRORLEVEL% NEQ 0 (
    echo.
    echo ERROR: Engine build failed!
    pause
    goto end
)
echo Engine rebuilt successfully!
echo.
goto launch

:rebuild_project
echo.
echo Rebuilding GoneSurfing project...
echo.
cd /d "E:\windowsgrejor\git\GoneSurfingUE5\GoneSurfing"
"%UE_PATH%" "%PROJECT_PATH%" -run=CompileEditor -project="%PROJECT_PATH%" -NoEngineChanges
if %ERRORLEVEL% NEQ 0 (
    echo.
    echo ERROR: Project build failed!
    pause
    goto end
)
echo Project rebuilt successfully!
echo.
goto launch

:launch
echo.
echo ============================================
echo Launching Unreal Editor...
echo ============================================
echo Engine: %UE_PATH%
echo Project: %PROJECT_PATH%
echo.

if not exist "%UE_PATH%" (
    echo ERROR: Unreal Editor not found at: %UE_PATH%
    pause
    goto end
)

if not exist "%PROJECT_PATH%" (
    echo ERROR: Project not found at: %PROJECT_PATH%
    pause
    goto end
)

REM Force DDC and temp directories to E: drive
set UE-LocalDataCachePath=E:\windowsgrejor\git\GoneSurfingUE5\GoneSurfing\Saved\DDC
set UE-SharedDataCachePath=E:\windowsgrejor\git\GoneSurfingUE5\GoneSurfing\Saved\DDC
set TEMP=E:\windowsgrejor\git\GoneSurfingUE5\GoneSurfing\Saved\Temp
set TMP=E:\windowsgrejor\git\GoneSurfingUE5\GoneSurfing\Saved\Temp

REM Create temp directory if it doesn't exist
if not exist "%TEMP%" mkdir "%TEMP%"

echo.
echo Environment configured to use E: drive only
echo DDC: %UE-LocalDataCachePath%
echo TEMP: %TEMP%
echo.

start "" "%UE_PATH%" "%PROJECT_PATH%"

echo.
echo Unreal Editor launched successfully!
echo You can close this window.
REM Only linger when a human is watching -- `timeout` errors out on redirected
REM stdin, and a script caller wants this window gone immediately.
if "%~1"=="" timeout /t 3
goto end

:end
