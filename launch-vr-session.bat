@echo off
rem PenguinScreen2 - Windows VR launcher (double-click shim).
rem Every decision lives in launch-vr-session.ps1 beside this file: it checks
rem the active OpenXR runtime (SteamVR is the supported one), then starts
rem pcsx2-qt-vr.exe with --vr, waits for it, and pauses itself when there is
rem something to read. This shim only runs it with the execution policy
rem bypassed for this one process; a game file dropped onto this .bat is
rem passed through. The flat pcsx2-qt.exe is never used.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0launch-vr-session.ps1" %*
set RC=%ERRORLEVEL%
if not "%RC%"=="0" (
  echo.
  echo launch-vr-session.ps1 exited with code %RC%
  pause
)
exit /b %RC%
