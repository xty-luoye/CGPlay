@echo off
setlocal

set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"

if not exist "%ROOT%\build_win_full\CMakeCache.txt" (
  echo Release build tree is not configured: %ROOT%\build_win_full
  exit /b 1
)

powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%\tools\apply_tlrender_patches.ps1"
if errorlevel 1 exit /b %errorlevel%

cmake -S "%ROOT%" -B "%ROOT%\build_win_full" -DCGPLAY_REQUIRE_RUNTIME=ON
if errorlevel 1 exit /b %errorlevel%

cmake --build "%ROOT%\build_win_full" --config Release --target CGPlay
if errorlevel 1 exit /b %errorlevel%

echo Release build completed:
echo %ROOT%\build_win_full\bin\Release\CGPlay.exe
exit /b 0
