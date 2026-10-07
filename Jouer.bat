@echo off
rem Burnout Dominator Reversed - start the game (after Installer.bat).
setlocal
cd /d "%~dp0"
set "PY=python"
where py >nul 2>nul && set "PY=py -3"
%PY% tools\project.py run --tail 80
echo.
pause
