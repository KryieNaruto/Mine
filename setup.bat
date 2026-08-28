@echo off
chcp 65001 >nul
rem Mine one-click setup entry: double-click to run tools/setup-env.sh via Git Bash.
where bash >nul 2>nul
if errorlevel 1 (
  echo [ERROR] bash.exe not found - Git for Windows is not on PATH.
  echo Install Git for Windows from https://gitforwindows.org/ then retry.
  pause
  exit /b 1
)
bash "%~dp0tools\setup-env.sh"
set "rc=%errorlevel%"
if not "%rc%"=="0" (
  echo.
  echo [ERROR] setup-env.sh exited with code %rc%. See output above.
)
pause
exit /b %rc%
