@echo off
rem Burnout Dominator Reversed - installation. Drop the ISO on this file, or run it and paste the path.
setlocal
cd /d "%~dp0"
rem Explorer quotes a dropped path only when it has spaces, and cmd splits an unquoted path at commas,
rem semicolons and equals signs: take the whole command line when there is more than one argument.
set "ISO=%~1"
if not "%~2"=="" set "ISO=%*"
if defined ISO set "ISO=%ISO:"=%"
if defined ISO goto run
echo Glisse le fichier ISO de Burnout Dominator dans cette fenetre puis appuie sur Entree.
echo (Entree seule si l'ISO n'a pas change d'emplacement depuis la derniere installation.)
set /p "ISO=ISO : "
if defined ISO set "ISO=%ISO:"=%"
:run
set "PY=python"
where py >nul 2>nul && set "PY=py -3"
%PY% tools\install.py --iso "%ISO%"
echo.
pause
