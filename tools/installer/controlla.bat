@echo off
rem QuattroStagioni - controllo senza modifiche (versione del gioco, loader, altre mod, archivi, pacchetti).
rem Il testo del controllo viene salvato anche in controllo.txt, utile da allegare a una segnalazione.
chcp 65001 >nul
cd /d "%~dp0"
qs_install.exe --check --no-pause --report controllo.txt %*
echo.
echo Testo salvato in "%~dp0controllo.txt".
pause
