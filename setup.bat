@echo off
setlocal
rem Mine one-click setup entry: run tools/setup-env.sh via Git Bash.
echo Mine one-click setup - starting via Git Bash (first run downloads toolchains, please wait)...
rem Prefer Git for Windows' own bash. System32\bash.exe is WSL and would run the Linux path.
if exist "%ProgramFiles%\Git\bin\bash.exe" set "GIT_BASH=%ProgramFiles%\Git\bin\bash.exe"
if not defined GIT_BASH if exist "%ProgramFiles(x86)%\Git\bin\bash.exe" set "GIT_BASH=%ProgramFiles(x86)%\Git\bin\bash.exe"
if not defined GIT_BASH if exist "%LocalAppData%\Programs\Git\bin\bash.exe" set "GIT_BASH=%LocalAppData%\Programs\Git\bin\bash.exe"
if not defined GIT_BASH (
  where bash >nul 2>nul
  if errorlevel 1 (
    echo [ERROR] Git Bash not found. Install Git for Windows from https://gitforwindows.org/ then retry.
    pause
    exit /b 1
  )
  set "GIT_BASH=bash"
)
rem UTF-8 console so the setup scripts' Chinese output is readable.
chcp 65001 >nul
rem Work around a Git for Windows console regression that can blank child output.
set "MSYS=disable_pcon"
set "LANG=C.UTF-8"
set "LC_ALL=C.UTF-8"
"%GIT_BASH%" "%~dp0tools\setup-env.sh"
set "rc=%errorlevel%"
if not "%rc%"=="0" (
  echo.
  echo [ERROR] setup-env.sh exited with code %rc%. See output above.
)
pause
exit /b %rc%
