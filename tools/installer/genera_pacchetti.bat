@echo off
rem QuattroStagioni - genera i pacchetti stagionali sul tuo PC (legge i file del gioco, non li modifica).
rem   genera_pacchetti.bat              primavera, fine estate, autunno, inverno (S L A W)
rem   genera_pacchetti.bat A W          solo gli stadi indicati
rem   genera_pacchetti.bat "A>W.2"      uno stadio di transizione (sempre tra virgolette)
rem Elenco completo degli stadi: qs_gen.exe --list-stages
setlocal EnableExtensions
chcp 65001 >nul
cd /d "%~dp0"

set "GAME="
for /f "usebackq delims=" %%G in (`qs_install.exe --print-game`) do set "GAME=%%G"
if not defined GAME (
  echo Non trovo Crimson Desert in automatico.
  echo In Steam: tasto destro sul gioco, Gestisci, Sfoglia i file locali, poi copia il percorso dalla barra.
  set /p "GAME=Incolla qui la cartella di Crimson Desert e premi Invio: "
)
if not exist "%GAME%\bin64\CrimsonDesert.exe" (
  echo In "%GAME%" non c'e' bin64\CrimsonDesert.exe: controlla il percorso.
  goto :fine
)
set "OUT=%GAME%\bin64\scripts\QuattroStagioni\packs"
echo Gioco:     %GAME%
echo Pacchetti: %OUT%
echo.
echo Controllo rapido dell'elenco delle texture (dopo una patch del gioco puo' segnalare differenze)...
qs_gen.exe --game "%GAME%" --verify-inventory --quick
if errorlevel 3 (
  echo.
  echo ATTENZIONE: l'elenco delle texture non corrisponde del tutto al gioco installato.
  echo Le texture diverse verranno saltate. Vedi docs\utente\PROBLEMI.md, "Dopo una patch del gioco".
  choice /c SN /m "Continuare lo stesso"
  if errorlevel 2 goto :fine
)

rem %* puo contenere stadi tra virgolette (transizioni): mai dentro set "...", romperebbe le virgolette
if "%~1"=="" (set "STAGES=S L A W") else (set STAGES=%*)
for %%S in (%STAGES%) do (
  echo.
  echo ==== Stadio "%%~S"
  qs_gen.exe --game "%GAME%" --stage "%%~S" --out "%OUT%"
  if errorlevel 1 echo Stadio "%%~S": qs_gen ha segnalato un errore, vedi il log nella cartella dei pacchetti.
)
echo.
echo Fatto. Avvia il gioco e premi F5: nella scheda Info trovi lo stato, nel log le righe "packs:".

:fine
echo.
pause
endlocal
