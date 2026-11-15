@echo off
py -3 "%~dp0tools\diagnose_build.py" --build
echo.
echo Diagnostics and compile logs are saved beside this file.
echo This check does not upload firmware or change Windows settings.
pause
