@echo off
rem QuattroStagioni - disinstallazione completa: rimette bin64 com'era prima dell'installazione.
rem I pacchetti stagionali generati restano (sono grandi e lunghi da rifare): per cancellare anche
rem quelli usa  disinstalla.bat --purge
chcp 65001 >nul
cd /d "%~dp0"
qs_install.exe --uninstall --no-pause %*
echo.
pause
