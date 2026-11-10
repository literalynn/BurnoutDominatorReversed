@echo off
rem Burnout Dominator Reversed - launcher window: installation, save profiles, the game and its console,
rem settings and updates. No console window stays open.
setlocal
cd /d "%~dp0"
where pyw >nul 2>nul && (start "" pyw -3 tools\launcher.py & exit /b)
where pythonw >nul 2>nul && (start "" pythonw tools\launcher.py & exit /b)
echo Python was not found. Install it with:  winget install Python.Python.3.12
echo.
pause
