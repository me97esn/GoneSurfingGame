@echo off
setlocal

REM Ensure Windows tools (find, findstr, etc.) win over any inherited
REM PATH (e.g. when invoked via bash hook -> cmd, where Git's GNU find
REM would otherwise shadow Windows find.exe).
set PATH=%SystemRoot%\System32;%SystemRoot%;%PATH%

REM ==========================================================
REM  BuildAndLaunch.bat
REM  Non-interactive build-then-launch flow for GoneSurfing.
REM
REM  Steps:
REM    1. Kill any running UnrealEditor.exe (so build isn't
REM       blocked by file locks).
REM    2. Build GoneSurfingEditor (Win64 Development) via UBT.
REM    3. If build OK: launch the editor with the project.
REM       If build fails: print summary, exit nonzero, do NOT
REM       relaunch the editor (so you can fix errors).
REM
REM  Exit codes:
REM    0  build + launch succeeded
REM    1  build failed
REM    2  setup error (missing path)
REM ==========================================================

REM --- Paths (shared with RunGameAndCollectLogs.bat via _paths.bat) ---
call "%~dp0_paths.bat"

REM --- DDC / TEMP overrides (keep all activity on E:) ---
set UE-LocalDataCachePath=%DDC_PATH%
set UE-SharedDataCachePath=%DDC_PATH%
set TEMP=%TEMP_PATH%
set TMP=%TEMP_PATH%
if not exist "%TEMP%" mkdir "%TEMP%"

REM --- Sanity checks ---
if not exist "%BUILD_BAT%" (
    echo ERROR: Build.bat not found at: %BUILD_BAT%
    exit /b 2
)
if not exist "%UE_PATH%" (
    echo ERROR: UnrealEditor.exe not found at: %UE_PATH%
    exit /b 2
)
if not exist "%PROJECT_PATH%" (
    echo ERROR: Project not found at: %PROJECT_PATH%
    exit /b 2
)

REM --- Step 1: stop any running editor ---
echo [1/3] Stopping any running UnrealEditor...
tasklist /FI "IMAGENAME eq UnrealEditor.exe" 2>nul | find /I "UnrealEditor.exe" >nul
if %ERRORLEVEL% EQU 0 (
    taskkill /IM UnrealEditor.exe /F >nul 2>&1
    REM brief pause so file locks release before UBT touches the DLLs
    timeout /t 2 /nobreak >nul
    echo   - editor stopped
) else (
    echo   - no editor running
)

REM --- Step 2: build editor target ---
echo.
echo [2/3] Building %EDITOR_TARGET% (Win64 Development)...
call "%BUILD_BAT%" %EDITOR_TARGET% Win64 Development -Project="%PROJECT_PATH%" -WaitMutex -FromMsBuild
set BUILD_RESULT=%ERRORLEVEL%
if not "%BUILD_RESULT%"=="0" (
    echo.
    echo ============================================
    echo BUILD FAILED (exit code %BUILD_RESULT%)
    echo Editor not launched. Fix errors above and re-run.
    echo ============================================
    exit /b 1
)
echo   - build succeeded

REM Launch step removed - the caller (hook script) is responsible for
REM relaunching the editor. Doing the launch here put the editor too deep
REM in the hook process tree (bash -> cmd -> bat -> launcher -> editor),
REM where it would die or never surface.

exit /b 0
