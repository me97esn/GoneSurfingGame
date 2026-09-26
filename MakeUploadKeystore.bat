@echo off
REM ==========================================================
REM  MakeUploadKeystore.bat
REM  Wrapper for MakeUploadKeystore.ps1.
REM
REM  Exists because this machine's PowerShell execution policy is Restricted
REM  (every scope Undefined), so a bare ".\MakeUploadKeystore.ps1" fails with
REM  "running scripts is disabled on this system". -ExecutionPolicy Bypass
REM  applies to this one process and changes nothing permanently.
REM
REM  The script is interactive - it asks for a keystore password - so run it
REM  from a console you can type into, not from a build step.
REM ==========================================================
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0MakeUploadKeystore.ps1" %*
exit /b %ERRORLEVEL%
