@echo off
setlocal

set "SCRIPT_DIR=%~dp0"
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT_DIR%tools\run_player_smoke_one_click.ps1" %*
set "EXITCODE=%ERRORLEVEL%"

echo.
echo ExitCode=%EXITCODE%
pause
exit /b %EXITCODE%
