@echo off
REM ==========================================================
REM  PackageAndroidRelease.bat
REM  Full production package for Android: Shipping config,
REM  clean (non-iterative) cook, stage, package. Does NOT
REM  deploy. Output lands in
REM    GoneSurfing\Saved\Releases\Android_ASTC\   (APK + OBB)
REM  and the script prints their sizes at the end - the point
REM  of this script is to know how big the app really is.
REM
REM  Differences from DeployAndroid.bat (the dev iteration loop):
REM    - -clientconfig=Shipping and -build: compiles the Shipping
REM      .so (Development is ~3x larger and drags the Vulkan
REM      validation layer into the APK).
REM    - Clean cook: Saved\Cooked\Android_ASTC is wiped first.
REM      Iterative cooking keeps previously cooked packages even
REM      when a cook-time CVar changed, so a release must not
REM      inherit stale bytes (e.g. distance fields, below).
REM    - -distribution only if a signing keystore is configured
REM      in DefaultEngine.ini; otherwise the APK is debug-signed,
REM      which is fine for measuring size but not for Play.
REM
REM  Mesh distance fields: r.GenerateMeshDistanceFields is True
REM  for desktop Lumen, and the cooker embeds a distance field in
REM  every static mesh for any platform whose r.DistanceFields is
REM  on - Android included, though mobile never reads them. They
REM  were 1.46 GB of the 2.1 GB cooked content. The cook gate reads
REM  the COOKER PROCESS's own CVar (TargetPlatformSettingsBase::
REM  UsesDistanceFields), so a per-platform ini cannot turn it
REM  off; the override has to go on the cooker command line.
REM
REM  Exit 0 ok, non-zero = UAT failed.
REM ==========================================================
setlocal
call "%~dp0_paths.bat"

REM --- NDK (same story as DeployAndroid.bat) ------------------
set NDK_DIR=%LOCALAPPDATA%\Android\Sdk\ndk\25.2.9519653
if not exist "%NDK_DIR%" (
    echo ERROR: NDK not found at %NDK_DIR%
    echo Installed NDK versions:
    dir /b "%LOCALAPPDATA%\Android\Sdk\ndk"
    exit /b 2
)
set NDKROOT=%NDK_DIR%
set ANDROID_NDK_ROOT=%NDK_DIR%

REM In Shipping the AndroidFileServer plugin (bIncludeInShipping=
REM False) builds a separate companion app via Gradle Exec
REM "cmd /c gradlew.bat" with a workingDir. If this env var is set
REM (Claude Code's shell sets it), cmd refuses to resolve a bare
REM command in the working directory and the step fails with
REM "'gradlew.bat' is not recognized". Clear it for the run.
set NoDefaultCurrentDirectoryInExePath=

REM The cooker must not fight the editor for file locks, and a
REM running editor caches read-only CVars from before any ini edit.
taskkill /F /IM UnrealEditor.exe >nul 2>&1
taskkill /F /IM LiveCodingConsole.exe >nul 2>&1

set COOKED_DIR=%~dp0GoneSurfing\Saved\Cooked\Android_ASTC
set ARCHIVE_DIR=%~dp0GoneSurfing\Saved\Releases
if exist "%COOKED_DIR%" (
    echo Clean cook: removing %COOKED_DIR%
    rmdir /s /q "%COOKED_DIR%"
)

REM --- distribution signing only when a keystore is set up -----
REM Two places to look. The signing entries belong in the Android platform
REM config, which is gitignored, because they carry the keystore passwords;
REM DefaultEngine.ini is still checked so an older setup keeps working.
REM Create both with MakeUploadKeystore.ps1.
set DIST_ARG=
set KEYSTORE_INI=
findstr /B /C:"KeyStore=" "%~dp0GoneSurfing\Config\Android\AndroidEngine.ini" >nul 2>&1
if not errorlevel 1 set KEYSTORE_INI=Config\Android\AndroidEngine.ini
if "%KEYSTORE_INI%"=="" (
    findstr /B /C:"KeyStore=" "%~dp0GoneSurfing\Config\DefaultEngine.ini" >nul 2>&1
    if not errorlevel 1 set KEYSTORE_INI=Config\DefaultEngine.ini
)
if not "%KEYSTORE_INI%"=="" (
    set DIST_ARG=-distribution
    echo Keystore configured in %KEYSTORE_INI%: packaging with -distribution.
) else (
    echo NOTE: no KeyStore in Config\Android\AndroidEngine.ini or DefaultEngine.ini -
    echo       the package will be debug-signed. Fine for sideloading, rejected by Play.
    echo       Run MakeUploadKeystore.ps1 to set one up.
)

set COOKER_OPTS=-ini:Engine:[/Script/Engine.RendererSettings]:r.GenerateMeshDistanceFields=False

call "%UE_ROOT%\Engine\Build\BatchFiles\RunUAT.bat" BuildCookRun ^
  -project="%PROJECT_PATH%" ^
  -noP4 -platform=Android -cookflavor=ASTC -clientconfig=Shipping ^
  -build -cook -stage -package -nocompileeditor %DIST_ARG% ^
  -archive -archivedirectory="%ARCHIVE_DIR%" ^
  -additionalcookeroptions="%COOKER_OPTS%" ^
  -utf8output
set UAT_ERR=%ERRORLEVEL%

if not "%UAT_ERR%"=="0" (
    echo.
    echo PackageAndroidRelease: UAT FAILED with %UAT_ERR%
    exit /b %UAT_ERR%
)

echo.
echo PackageAndroidRelease: done. Package sizes:
powershell -NoProfile -Command ^
  "Get-ChildItem -Recurse '%ARCHIVE_DIR%' -Include *.apk,*.aab,*.obb | Sort-Object LastWriteTime -Descending | Select-Object -First 4 | ForEach-Object { '{0,10:N1} MB  {1}' -f ($_.Length/1MB), $_.FullName }"
exit /b 0
