@echo off
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0installer_hires.ps1" -Uninstall %*
echo.
pause
