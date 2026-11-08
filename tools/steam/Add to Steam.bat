@echo off
rem Adds the game to Steam, with its artwork: runs add-to-steam.ps1 beside it (quit Steam first).
rem "Add to Steam.bat -Remove" takes it out again.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0add-to-steam.ps1" %*
echo.
pause
