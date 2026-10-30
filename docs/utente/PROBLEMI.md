# Risolvere i problemi

Quasi tutto si capisce da due strumenti:

1. **`controlla.bat`** (oppure `qs_install.exe --check`): controllo senza modifiche di versione del
   gioco, loader, altre mod, archivi e pacchetti. Salva il testo in `controllo.txt`.
2. **Il log della mod**: `bin64\scripts\QuattroStagioni.log` (la partita precedente è in
   `QuattroStagioni.previous.log`). Si apre con il Blocco note. Il percorso si copia anche dal menu
   F5, scheda *Info*, *Copia percorso*.

La regola della mod è sempre la stessa: **se qualcosa non torna, quella parte si spegne e il gioco
continua normalmente**. Nessun problema di QuattroStagioni può rovinare i file del gioco o i
salvataggi, perché la mod non li scrive mai.

Cerca nel log la riga di avvio:

    startup in 412.3 ms: file hooks on, packs on, season on, overlay on, probes off

Ogni `off` (tranne `probes`, che è spento di proposito) ha una sezione qui sotto.

---

## Il log non viene creato

La mod non viene caricata.

- **Manca Ultimate ASI Loader** o è la versione a 32 bit: `controlla.bat` lo segnala con
  `[ERRORE] Ultimate ASI Loader non trovato`. Vedi [INSTALLAZIONE.md](INSTALLAZIONE.md), passo 1.
- **`winmm.ini` con `LoadPlugins=0`**: rilancia `qs_install.exe`, che lo corregge.
- **Antivirus**: Windows Defender può aver messo in quarantena `QuattroStagioni.asi`. Controlla che
  il file esista in `bin64\scripts\`; se manca, aggiungi l'esclusione (INSTALLAZIONE.md, passo 3) e
  reinstalla.
- **Due loader**: vedi [Conflitti con altri loader](#conflitti-con-altri-loader).

## Controllo di versione: "version guard failed"

Nel log:

    version check: exe size 336264088 timestamp 1793296370 (expected ...) -> MISMATCH
    game files do not match 2.04.00: substitution disabled
    version guard failed: no file hooks and no packs, game files are served unmodified

Significa che l'eseguibile o i dati del gioco non sono quelli per cui questa versione della mod è
stata preparata, quasi sempre perché **il gioco si è aggiornato**. La mod non cambia nessuna
texture (il menu F5 e il calendario restano attivi). Vedi [Dopo una patch del
gioco](#dopo-una-patch-del-gioco).

Se il gioco non si è aggiornato e vedi comunque `MISMATCH`:

- in Steam, *Proprietà*, *File installati*, **Verifica integrità dei file di gioco**;
- versioni non Steam (altri negozi, copie modificate) non sono supportate.

## Pacchetto rifiutato

Nel log, una di queste righe:

| Riga | Causa | Cosa fare |
|---|---|---|
| `packs: ... refused: pack was generated for game build "...", installed is "..."` | il pacchetto è di un'altra versione del gioco | rigeneralo con `genera_pacchetti.bat` |
| `... 0.pamt hash ..., pack expects ...: regenerate the pack` | un indice degli archivi è cambiato (patch o altra mod) | rigeneralo |
| `... size ..., pack expects ...` | un archivio `.paz` ha dimensione diversa (patch o mod di texture) | rigeneralo; vedi anche [mod di texture](#mod-di-texture-alberi-erba-hd) |
| `pack rejected: ... hash mismatch (corrupt file)` | file del pacchetto danneggiato (disco pieno, copia interrotta) | cancellalo e rigeneralo |
| `season: no pack for stage A>W.1 (...): vanilla textures` | manca il pacchetto di quello stadio | generalo, oppure metti *Stadi intermedi* a 0 (cambio netto) |
| `packs: no active pack configured` | normale con `Active=` vuoto finché il calendario non sceglie uno stadio | nessuna |

`controlla.bat` fa lo stesso controllo su tutti i pacchetti e scrive `da rigenerare` accanto a
quelli che il plugin rifiuterebbe.

Durante il gioco, ogni 30 secondi il log scrive una riga come:

    pack A: reads served 1520, records hit 340, patched 338, already 0, LEFT UNTOUCHED: mismatch 0 ...

`LEFT UNTOUCHED` maggiore di zero vuol dire che alcune letture non corrispondevano ai byte attesi e
sono state lasciate originali, per sicurezza. Pochi casi non sono un problema; molti indicano che
un'altra mod ha cambiato quelle texture.

## Le piante non cambiano

1. Nel log c'è `packs: ... verified`? Se no, vedi [Pacchetto rifiutato](#pacchetto-rifiutato).
2. Lo stadio attuale ha un pacchetto? Il menu F5 mostra in basso `stadio ... | pacchetto ...`.
   In estate (`U`) non c'è niente da cambiare: è il gioco originale.
3. Le texture cambiano per ciò che il gioco carica **dopo** il cambio di stadio. Allontanati,
   usa il viaggio rapido o ricarica la partita.
4. Il riepilogo di `qs_gen` aveva un **tasso di fit** basso? Le texture che non rientrano nella
   dimensione originale restano quelle del gioco. Segnalalo con il log `qs_gen_<stadio>.log` che
   trovi nella cartella dei pacchetti.
5. Rallentamenti o texture che compaiono in ritardo: lascia `WaitForTargetReads=1` e prova
   `CacheMB=256`.

## Il menu F5 non compare (overlay disabilitato)

Nel log cerca le righe che iniziano con `overlay`:

| Riga | Significato |
|---|---|
| `overlay disabled: ... (the game is not affected)` | l'overlay si è spento da solo per un problema; il resto della mod funziona |
| `overlay: no swapchain creation seen after ... ms, hooking from a dummy device` | il gioco ha creato la finestra prima della mod: normale, l'overlay si aggancia lo stesso |
| `overlay: device removed` | errore della scheda video (spesso driver o overclock) |
| `overlay [start]: dxgi.dll is a proxy: ... (ReShade)` | c'è ReShade: di solito funziona insieme |
| `overlay [start]: found ... frame generation` | frame generation attiva |

Cose da provare, una alla volta:

- **Frame generation** (DLSS FG, FSR FG, XeFG): spegnila e riprova. Con la generazione dei frame il
  menu può tremolare o non comparire.
- **Altri overlay**: Steam, Discord, RivaTuner, NVIDIA App. Chiudili per prova.
- **ReShade / OptiScaler**: prova senza; se il menu compare, il problema è la convivenza.
  Segnalacelo con il log.
- **Modalità finestra**: prova *Schermo intero senza bordi*.
- Il tasto F5 è usato da un altro programma? Per ora il tasto non si cambia dal file ini.

Il menu non è indispensabile: tutte le impostazioni si cambiano anche in `QuattroStagioni.ini`.

## Conflitti con altri loader

Due loader ASI insieme (per esempio `winmm.dll` e `version.dll`, o il loader di un mod manager)
fanno chiudere il gioco all'avvio. `controlla.bat` li elenca:

    [ERRORE] 2 copie di Ultimate ASI Loader con nomi diversi
    [AVVISO] version.dll: proxy sconosciuto

Tieni **un solo** loader. Sposta gli altri in una cartella fuori dal gioco (non cancellarli se non
sai da quale mod arrivano). Se usi già un loader con un altro nome (per esempio `dinput8.dll`),
non serve `winmm.dll`: l'installatore lo riconosce e scrive le impostazioni nel suo file
(`dinput8.ini`).

Proxy grafici (`dxgi.dll`, `d3d12.dll`) di ReShade o OptiScaler non sono loader ASI: possono
convivere. Con OptiScaler sono stati segnalati crash insieme ad altre mod ASI: se il gioco si
chiude, prova prima senza.

## Conflitti con altre mod

- **Seasons of Pywel** (`Seasons.asi`): le due mod gestiscono entrambe stagioni e meteo, non usarle
  insieme. Con `LoadFromScriptsOnly=1` (impostato dall'installatore) il loader non carica i file
  `.asi` nella cartella `bin64`, quindi `bin64\Seasons.asi` resta lì ma spento. Se lo avevi messo in
  `bin64\scripts\`, spostalo.
- **Altre mod ASI** in `bin64\scripts\`: vengono caricate insieme a QuattroStagioni. Di solito
  convivono; se il gioco si chiude, prova a toglierle una alla volta.
- **Mod manager** (Definitive Mod Manager, CDUMM) e mod che cambiano gli archivi del gioco:
  QuattroStagioni lavora sui byte che il gioco legge davvero e controlla ogni texture prima di
  cambiarla. Se un'altra mod cambia gli archivi **dopo** che hai generato i pacchetti, i pacchetti
  vengono rifiutati: rigenerali. Ordine consigliato: prima installi e attivi le altre mod, poi
  generi i pacchetti. `controlla.bat` segnala archivi alterati (`checksum interno errato`,
  `dimensione ..., l'indice dice ...`, `archivio non presente nell'indice`).
- **Crimson Weather** e altre mod del meteo: oggi QuattroStagioni non cambia il meteo, quindi non
  si pestano i piedi. Quando il controllo del meteo sarà attivo, spegni *Meteo stagionale* se
  preferisci l'altra mod.

### Mod di texture (alberi, erba HD)

Le ricette partono dalle texture del gioco originale. Con una mod che sostituisce le texture di
alberi ed erba, `qs_gen` lavora sulle texture che trova (quelle della mod), ma l'elenco delle
texture (`--verify-inventory`) segnalerà differenze e alcune piante verranno saltate. Il risultato
non è garantito.

## Dopo una patch del gioco

1. La mod si spegne da sola (`version guard failed` nel log). Il gioco funziona normalmente.
2. Aspetta una versione di QuattroStagioni che riconosca la nuova versione del gioco (vedi la
   pagina delle release). Nel frattempo non serve disinstallare nulla.
3. Installa la nuova versione con il suo `qs_install.exe`.
4. Rigenera i pacchetti con `genera_pacchetti.bat`: quelli vecchi vengono rifiutati
   (`pack was generated for game build ...`).

Se vuoi aiutare: lancia `controlla.bat` e allega `controllo.txt` alla segnalazione. Contiene la
dimensione e il timestamp del nuovo eseguibile, che servono per aggiornare la mod.

## Il gioco si chiude all'avvio

1. Due loader? Vedi sopra.
2. Metti `Enabled=0` in `QuattroStagioni.ini`. Se il gioco parte, il problema è la mod: rimetti
   `Enabled=1`, imposta `LogLevel=2` e manda il log.
3. Se si chiude anche con `Enabled=0`, il problema è un'altra mod o il loader: togli
   temporaneamente `winmm.dll` (o il tuo loader) per verificarlo.

## Disinstallazione: "nessuna installazione di qs_install trovata"

Avevi installato a mano (copiando i file). Togli:

- `bin64\scripts\QuattroStagioni.asi` e `bin64\scripts\QuattroStagioni.ini`;
- la cartella `bin64\scripts\QuattroStagioni` (contiene anche i pacchetti);
- in `bin64\winmm.ini` rimetti `LoadPlugins=0` (o i valori che avevi prima).

## Segnalare un problema

Apri una segnalazione (issue) sulla pagina GitHub del progetto con:

- `controllo.txt` (da `controlla.bat`);
- `bin64\scripts\QuattroStagioni.log` (meglio se con `LogLevel=2`);
- cosa stavi facendo, impostazioni grafiche (frame generation, HDR, ReShade, OptiScaler).

I log non contengono dati personali oltre ai percorsi delle cartelle. Non allegare i pacchetti
`.qsp` né file del gioco.
