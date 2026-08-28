@echo off
rem Mine 一键搭建入口:双击运行,调用 tools/setup-env.sh(Git for Windows 的 bash)。
chcp 65001 >nul
where bash >nul 2>nul
if errorlevel 1 (
  echo [ERROR] 未找到 bash.exe(Git for Windows)。
  echo 请先安装 Git for Windows: https://gitforwindows.org/ 后重试。
  pause
  exit /b 1
)
bash "%~dp0tools\setup-env.sh"
set "rc=%errorlevel%"
if not "%rc%"=="0" (
  echo.
  echo [ERROR] setup-env.sh 退出码 %rc%,见上方日志。
)
pause
exit /b %rc%
