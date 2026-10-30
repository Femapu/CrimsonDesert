# Registro delle modifiche

Formato: una sezione per versione, la più recente in alto. Le versioni seguono
[Semantic Versioning](https://semver.org/lang/it/); finché la versione inizia con `0.` la mod è in
sviluppo e le impostazioni possono cambiare tra una versione e l'altra.

Legenda dello stato: **provato nel gioco** = verificato in Crimson Desert; **provato fuori dal
gioco** = verificato con test automatici, dati sintetici, wine o D3D12 software.

## [Non rilasciato]

### Aggiunto
- `qs_install.exe`: installatore, controllo e disinstallazione per i giocatori.
  - Trova il gioco tramite Steam (registro e `libraryfolders.vdf`) oppure con `--game`.
  - Controlla la versione dell'eseguibile e di `meta\0.paver` con lo stesso elenco del plugin
    (`src/core/known_builds.h`, unica fonte).
  - Rileva Ultimate ASI Loader, altri loader e proxy (`winmm`, `version`, `dinput8`, `dsound`,
    `dxgi`, `d3d12`...), ReShade, OptiScaler, il vecchio `Seasons.asi`, altre mod ASI, cartelle di mod
    manager e archivi del gioco alterati (checksum degli indici, dimensioni dei `.paz`, archivi in
    più), e controlla i pacchetti stagionali presenti.
  - Copie di riserva prima di sostituire qualsiasi file, verifica SHA-256 dopo ogni copia, registro
    dell'installazione, manifest di disinstallazione; `--uninstall` rimette tutto com'era, `--check`
    non modifica nulla. Non scrive mai fuori da `bin64\scripts\` e dal file ini del loader.
- `controlla.bat`, `genera_pacchetti.bat`, `disinstalla.bat`: comandi con doppio clic.
- `tools/make_release.sh` e la pubblicazione automatica delle release su GitHub (tag `v*`).
- Guide per i giocatori in `docs/utente/`: installazione, guida al menu e alle impostazioni,
  risoluzione dei problemi, domande frequenti. Nuovo README.

## [0.3.0] - 2026-10-30 (solo sviluppo, nessuna release pubblica)

Stato: tutto **provato fuori dal gioco**; niente è ancora **provato nel gioco**.

### Aggiunto
- Motore delle stagioni: calendario a cinque fasi, transizioni a stadi, varietà per pianta,
  regioni, temperatura, pesi del meteo, accumulo della neve, luce, alba e tramonto.
- Generatore `qs_gen`: legge gli archivi del gioco (anche cifrati), applica 19 ricette stagionali in
  OKLab a 3688 texture, le ricodifica alla stessa dimensione esatta e scrive pacchetti `.qsp`.
- Plugin: serve migliaia di texture da un pacchetto verificato a blocchi di 4 KiB e cambia
  pacchetto quando il calendario cambia stadio.
- Menu F5 in cinque schede (Calendario, Stagioni, Regioni, Meteo e luce, Info) con salvataggio
  nel file ini; compatibile con HDR, ReShade e Streamline.
- Strumenti `qs_preview` (anteprime) e `qs_season` (calendario fuori dal gioco).
- Probe di sola lettura per trovare orologio, meteo e luce del gioco (spente di default).

### Non ancora disponibile
- Applicazione al gioco di meteo, neve, luce, durata del giorno e particelle (oggi solo calcolati).
- Sostituzione delle texture già caricate quando cambia lo stadio.
- Pacchetti diversi per regione.

## [0.2.0] - fase 2 (solo sviluppo)

### Aggiunto
- Prima sostituzione sicura di una texture (logo di caricamento) senza modificare i file del gioco.
- Controllo di versione all'avvio: con un gioco diverso dalla 2.04.00 la mod si spegne da sola.
