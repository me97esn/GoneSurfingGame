@echo off
REM ==========================================================
REM  DeployAndroid.bat
REM  Cook + stage + package + install to the connected Android
REM  device. Use for config/shader changes that need a recook -
REM  read-only CVars (r.AllowStaticLighting,
REM  r.Mobile.EnableStaticAndCSMShadowReceivers, ...) bake at
REM  cook time and cannot be changed at runtime.
REM
REM  Exit 0 ok, non-zero = UAT failed (see the log path echoed
REM  at the end).
REM
REM  NOTE: skips compiling (-nobuild). If you changed C++, build
REM  the Android target first or drop -nobuild below.
REM ==========================================================
setlocal
call "%~dp0_paths.bat"

REM --- NDK ---------------------------------------------------
REM The machine's NDKROOT env var points at 25.1.8937393 (the
REM engine's DEFAULT_NDK_VERSION, UEDeployAndroid.cs) which is
REM NOT installed - only 25.2.9519653 is. UAT fails with
REM "Couldn't find 32-bit or 64-bit versions of the Android
REM toolchain" unless this is corrected. Override locally
REM rather than changing the machine's environment.
set NDK_DIR=%LOCALAPPDATA%\Android\Sdk\ndk\25.2.9519653
if not exist "%NDK_DIR%" (
    echo ERROR: NDK not found at %NDK_DIR%
    echo Installed NDK versions:
    dir /b "%LOCALAPPDATA%\Android\Sdk\ndk"
    exit /b 2
)
set NDKROOT=%NDK_DIR%
set ANDROID_NDK_ROOT=%NDK_DIR%

REM Gradle runs "cmd /c gradlew.bat" from a working directory; this
REM env var (set by Claude Code's shell) stops cmd resolving it there.
set NoDefaultCurrentDirectoryInExePath=

REM The cooker must not fight the editor for file locks, and a
REM running editor also caches read-only CVars from before any
REM ini edit - cooking from it would silently use stale values.
taskkill /F /IM UnrealEditor.exe >nul 2>&1
taskkill /F /IM LiveCodingConsole.exe >nul 2>&1

REM --- optional map argument -----------------------------------
REM   DeployAndroid.bat                       -> normal iterative cook
REM   DeployAndroid.bat /Game/levels/MyTest    -> cook ONLY that map, iterative OFF
REM
REM Iterative cooking SILENTLY IGNORES a newly added -map: the map is
REM passed to the cooker, the build reports success, and the device
REM still says "Failed to load package". So when a map is named,
REM iterative is forced off.
set MAP_ARG=
set ITERATIVE=-iterativecooking
if not "%~1"=="" (
    set MAP_ARG=-map=%~1
    set ITERATIVE=
    echo Cooking map %~1 with iterative cooking DISABLED.
)

REM Strip mesh distance fields from the cook - mobile never reads
REM them and they were ~70% of the pak. Same override as
REM PackageAndroidRelease.bat (see the explanation there). Note
REM iterative cooking keeps already-cooked meshes as they are:
REM after changing this, wipe Saved\Cooked\Android_ASTC once.
set COOKER_OPTS=-ini:Engine:[/Script/Engine.RendererSettings]:r.GenerateMeshDistanceFields=False

call "%UE_ROOT%\Engine\Build\BatchFiles\RunUAT.bat" BuildCookRun ^
  -project="%PROJECT_PATH%" ^
  -noP4 -platform=Android -cookflavor=ASTC -clientconfig=Development ^
  -cook %MAP_ARG% -stage -package -deploy -nobuild -nocompileeditor ^
  -additionalcookeroptions="%COOKER_OPTS%" ^
  %ITERATIVE% -utf8output
set UAT_ERR=%ERRORLEVEL%

if not "%UAT_ERR%"=="0" (
    echo.
    echo DeployAndroid: UAT FAILED with %UAT_ERR%
    exit /b %UAT_ERR%
)

REM --- verify the data actually landed ------------------------
REM UAT's deploy step pushes the paks as loose files, and the big
REM one (.ucas, ~750MB) can fail with repeated "network error"
REM retries followed by a NullReferenceException. UAT has been
REM seen reporting BUILD SUCCESSFUL anyway, leaving the device with
REM a new APK and NO game data - which shows up on the phone as
REM "Failed to open descriptor file ... .uproject", not as a
REM deploy error. Check every staged pak file against the device by
REM size (a missing file and a truncated push both fail), and push
REM manually if any is off.
REM The staged build lives next to the .uproject, not next to this
REM script - the two are in different checkouts.
for %%P in ("%PROJECT_PATH%") do set PROJECT_DIR=%%~dpP
set STAGED_PAKS=%PROJECT_DIR%Saved\StagedBuilds\Android_ASTC\GoneSurfing\Content\Paks
set DEV_PAKS=/sdcard/Android/data/com.dsh.gonesurfing/files/UnrealGame/GoneSurfing/GoneSurfing/Content/Paks
if not exist "%STAGED_PAKS%\*.ucas" (
    echo DeployAndroid: no staged paks at %STAGED_PAKS%
    exit /b 3
)
call :check_paks
if errorlevel 1 (
    echo DeployAndroid: device pak data does not match the staged build - pushing manually.
    adb push "%STAGED_PAKS%" "%DEV_PAKS%/.."
    call :check_paks
    if errorlevel 1 (
        echo DeployAndroid: device pak data STILL does not match after the manual push.
        exit /b 3
    )
)
echo DeployAndroid: device pak data matches the staged build.

echo.
echo DeployAndroid: done. Verify what actually landed on device with:
echo   adb pull /sdcard/Android/data/com.dsh.gonesurfing/files/UnrealGame/GoneSurfing/GoneSurfing/Saved/Logs/GoneSurfing.log
echo   grep "Set CVar" GoneSurfing.log
exit /b 0

REM --- :check_paks -------------------------------------------
REM Exit 0 when every file in STAGED_PAKS is on the device at
REM DEV_PAKS with the same size, 1 otherwise. Mismatches are echoed.
:check_paks
set PAKS_BAD=0
for %%F in ("%STAGED_PAKS%\*") do call :check_one "%%~nxF" %%~zF
exit /b %PAKS_BAD%

:check_one
set DEV_SIZE=missing
REM Inside the double quotes the redirects go to the device shell
REM as-is; carets there would be passed through literally.
for /f %%S in ('adb shell "wc -c < %DEV_PAKS%/%~1 2>/dev/null"') do set DEV_SIZE=%%S
if not "%DEV_SIZE%"=="%~2" (
    echo   %~1: staged %~2 bytes, device %DEV_SIZE%
    set PAKS_BAD=1
)
exit /b 0
