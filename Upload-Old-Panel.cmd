@echo off
powershell.exe -NoProfile -File "%~dp0build.ps1" -Panel Old -Upload
pause
