@echo off
setlocal

set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"

set "MODE=full"
if /I not "%~1"=="" set "MODE=%~1"

set "INSTALLER=auto"
if /I not "%~2"=="" set "INSTALLER=%~2"

echo ==^> Building CGPlay installer
echo     Mode      : %MODE%
echo     Installer : %INSTALLER%

powershell -NoProfile -ExecutionPolicy Bypass ^
  -File "%ROOT%\tools\package_installer.ps1" ^
  -ProjectRoot "%ROOT%" ^
  -PackageMode "%MODE%" ^
  -Installer "%INSTALLER%"

if errorlevel 1 (
  echo.
  echo Installer build failed.
  exit /b %errorlevel%
)

echo.
echo Installer build completed successfully.
exit /b 0
