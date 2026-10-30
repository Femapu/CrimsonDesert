# Installare QuattroStagioni

Guida passo per passo per Crimson Desert **2.04** su PC (Steam, con o senza il DLC "Charting the
Unknown"). Non servono conoscenze tecniche: basta saper copiare file e fare doppio clic.

> **In breve**
> 1. Installa Ultimate ASI Loader (una volta sola).
> 2. Estrai lo zip di QuattroStagioni in una cartella qualsiasi.
> 3. Doppio clic su `qs_install.exe`.
> 4. Doppio clic su `genera_pacchetti.bat` (crea le texture stagionali sul tuo PC).
> 5. Avvia il gioco e premi **F5**.

QuattroStagioni **non modifica nessun file del gioco**. Aggiunge solo file in `bin64\scripts\` e
imposta due righe in `bin64\winmm.ini`. Si toglie del tutto con `disinstalla.bat`.

---

## Prima di cominciare

- **Versione del gioco**: la mod riconosce solo le versioni del gioco per cui è stata preparata
  (oggi la 2.04.00 e il suo hotfix del 30 ottobre 2026). Se il gioco si aggiorna, la mod si spegne
  da sola e il gioco funziona normalmente: aspetta una versione aggiornata della mod.
- **Spazio su disco**: i pacchetti stagionali occupano fino a circa **1,6 GB ciascuno** (quello dell'autunno, provato, 726 MB). Le quattro
  stagioni principali richiedono circa 6,5 GB; con tutte le transizioni graduali (19 pacchetti)
  servono circa 30 GB. Si possono mettere anche su un altro disco (vedi la [Guida](GUIDA.md),
  sezione `[Packs]`).
- **Altre mod**: se usi **Seasons of Pywel**, non serve disinstallarlo, ma le due mod non vanno
  usate insieme (l'installatore imposta il loader in modo che Seasons non venga caricato). Se usi un
  mod manager (DMM, CDUMM) o mod di texture per alberi ed erba, leggi la sezione
  [Altre mod](PROBLEMI.md#conflitti-con-altre-mod) di PROBLEMI.md.

## 1. Ultimate ASI Loader (una volta sola)

QuattroStagioni è un plugin `.asi`: il gioco lo carica grazie a **Ultimate ASI Loader**, un piccolo
programma gratuito e open source (licenza MIT) usato da moltissime mod.

> Se nello zip di QuattroStagioni c'è una cartella `loader`, salta questo passo: l'installatore
> copia il loader da solo quando manca.

1. Vai su <https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases> e scarica
   **`Ultimate-ASI-Loader_x64.zip`** (attenzione: la versione **x64**, non quella a 32 bit).
2. Dentro lo zip c'è `dinput8.dll`. **Rinominalo in `winmm.dll`**.
3. Apri la cartella del gioco: in Steam, tasto destro su Crimson Desert, *Gestisci*,
   *Sfoglia i file locali*. Entra in `bin64` (dove c'è `CrimsonDesert.exe`).
4. Copia lì `winmm.dll`.

Hai già un loader perché usi altre mod ASI? Va bene così: l'installatore lo riconosce. Non
installarne un secondo con un nome diverso (per esempio `version.dll` **e** `winmm.dll`): due
loader insieme fanno chiudere il gioco all'avvio.

## 2. Estrai lo zip

Estrai `QuattroStagioni-<versione>.zip` in una cartella a tua scelta, per esempio
`Documenti\QuattroStagioni`. Non serve metterla dentro la cartella del gioco. Tienila: contiene
anche il generatore dei pacchetti e il programma di disinstallazione.

Contenuto principale:

| File | A cosa serve |
|---|---|
| `qs_install.exe` | installa, controlla, disinstalla |
| `controlla.bat` | controllo senza modifiche (versione, loader, altre mod, archivi, pacchetti) |
| `genera_pacchetti.bat` | crea i pacchetti stagionali sul tuo PC |
| `disinstalla.bat` | rimette tutto com'era |
| `qs_gen.exe` | il generatore vero e proprio (usato da `genera_pacchetti.bat`) |
| `qs_preview.exe`, `qs_season.exe` | strumenti facoltativi (anteprime, calendario) |
| `bin64\` | i file che vengono copiati nel gioco |
| `data\` | ricette dei colori e l'elenco delle texture (solo numeri e percorsi) |
| `docs\` | queste guide |

## 3. Windows Defender e antivirus

Alcuni antivirus, Windows Defender compreso, a volte scambiano i plugin `.asi` per minacce
(falso positivo: un `.asi` è una DLL che si aggancia a un programma, proprio come fanno i virus).
Il codice di QuattroStagioni è pubblico e ogni file della release ha il suo SHA-256 nel file
`SHA256SUMS`.

Se dopo l'installazione `QuattroStagioni.asi` sparisce o il gioco non crea il file
`bin64\scripts\QuattroStagioni.log`, aggiungi un'esclusione:

1. *Start*, *Sicurezza di Windows*, *Protezione da virus e minacce*.
2. *Impostazioni di Protezione da virus e minacce*, *Gestisci impostazioni*.
3. In fondo, *Esclusioni*, *Aggiungi o rimuovi esclusioni*, *Aggiungi un'esclusione*, *Cartella*.
4. Scegli `...\Crimson Desert\bin64\scripts`.
5. Se il file era stato messo in quarantena: *Cronologia protezione*, seleziona l'elemento,
   *Azioni*, *Ripristina*. Poi rilancia `qs_install.exe`.

Escludi solo la cartella `scripts`, non tutto il disco o tutta la cartella del gioco.

## 4. Installa

1. Chiudi il gioco.
2. Doppio clic su **`qs_install.exe`**.
3. Il programma trova il gioco da solo tramite Steam e mostra un controllo:
   - **versione del gioco** (riconosciuta o no);
   - **loader** trovato in `bin64` ed eventuali altri loader o proxy (ReShade, OptiScaler...);
   - **altre mod** (Seasons of Pywel, altre `.asi`, cartelle di mod manager);
   - **archivi del gioco**: controllo in sola lettura che gli indici non siano stati alterati da
     altre mod;
   - **pacchetti stagionali** già presenti.
4. Righe `[OK]` e `[INFO]`: tutto bene. `[AVVISO]`: leggi il consiglio sotto la riga, di solito si
   può proseguire. `[ERRORE]`: va sistemato prima (per esempio manca il loader).
5. Rispondi **s** alla domanda *Procedere con l'installazione?*.

Cosa succede:

- vengono copiati `QuattroStagioni.asi`, `QuattroStagioni.ini` e la cartella `QuattroStagioni`
  dentro `bin64\scripts\`, e ogni file copiato viene riletto e verificato (SHA-256);
- in `bin64\winmm.ini` vengono impostati `LoadPlugins=1` e `LoadFromScriptsOnly=1` (il loader carica
  solo le mod nella cartella `scripts`); se il file esisteva, ne viene salvata una copia;
- se un file che va sostituito esiste già, viene salvato prima in
  `bin64\scripts\QuattroStagioni\install\backup\`;
- se hai già un `QuattroStagioni.ini` con le tue impostazioni, **resta il tuo**: i nuovi valori
  predefiniti finiscono accanto, in `QuattroStagioni.ini.new`;
- un registro dell'installazione finisce in `bin64\scripts\QuattroStagioni\install\qs_install.log`.

Il gioco non è in una libreria Steam standard o hai più copie? Apri un *Prompt dei comandi* nella
cartella della mod (nella barra degli indirizzi di Esplora file scrivi `cmd` e premi Invio) e indica
la cartella a mano:

    qs_install.exe --game "D:\Giochi\Crimson Desert"

## 5. Genera i pacchetti stagionali

Le texture autunnali, invernali e primaverili **non sono nello zip**: vengono create sul tuo PC,
partendo dai file del tuo gioco, dal generatore `qs_gen`. Così la mod non distribuisce nessun
contenuto di Pearl Abyss e i pacchetti corrispondono esattamente alla tua versione del gioco.

1. Doppio clic su **`genera_pacchetti.bat`**.
2. Lo script trova il gioco, fa un controllo rapido dell'elenco delle texture e poi genera i
   quattro stadi principali: **S** primavera, **L** fine estate secca, **A** autunno, **W**
   inverno. L'estate (**U**) è il gioco originale e non ha bisogno di un pacchetto.
3. Alla fine di ogni stadio compare un riepilogo con il **tasso di fit**: la percentuale di texture
   modificate con successo. Le texture che non rientrano restano quelle originali (non è un errore).

I pacchetti finiscono in `bin64\scripts\QuattroStagioni\packs\` (per esempio `A.qsp`). Restano sul
tuo PC: **non condividerli**, contengono dati derivati dai file del gioco.

Il tempo di generazione dipende dal processore e dal disco (usa tutti i core): sul PC di prova lo stadio A ha richiesto 43 secondi; calcola qualche
minuto per stadio. Lo si fa una volta sola, e di nuovo solo dopo un aggiornamento del gioco.

### Transizioni graduali (facoltative)

Tra una stagione e l'altra il paesaggio passa per stadi intermedi (impostazione
`TransitionSteps`, predefinita 3). Ogni stadio intermedio è un pacchetto in più, per esempio
`A>W.1`, `A>W.2`, `A>W.3` dall'autunno all'inverno. Se un pacchetto manca, durante quello stadio
si vedono le texture originali (estive).

Due scelte:

- **Solo le quattro stagioni** (consigliato per cominciare): imposta `TransitionSteps = 0` nel menu
  F5 (scheda *Stagioni*, *Stadi intermedi*: "cambio netto") oppure in `QuattroStagioni.ini`.
- **Transizioni complete**: genera anche gli stadi intermedi. L'elenco completo si vede con
  `qs_gen.exe --list-stages`; per generarne alcuni:

      genera_pacchetti.bat "A>W.1" "A>W.2" "A>W.3"

  (le virgolette servono per via del simbolo `>`). Il file del pacchetto usa `-` al posto di `>`:
  `A-W.2.qsp`.

Uso avanzato del generatore: vedi [GENERATORE.md](../GENERATORE.md) nel repository.

## 6. Primo avvio

1. Avvia Crimson Desert da Steam come sempre.
2. Al menu principale o in partita premi **F5**: si apre il menu di QuattroStagioni (calendario,
   stagioni, regioni, meteo e luce, info). **F5** o **Esc** lo chiudono. Mentre è aperto, mouse e
   tastiera vanno al menu e non al gioco.
3. Le modifiche valgono subito; il pulsante **Salva** le scrive in `QuattroStagioni.ini`.
4. Per vedere subito una stagione senza aspettare il calendario: scheda *Calendario*, sezione
   *Vai all'inizio di*, scegli per esempio **Autunno** (passa alla modalità manuale).

Le texture cambiano per quello che il gioco carica **dopo** il cambio di stadio: il paesaggio
già visibile intorno a te resta com'è finché il gioco non lo ricarica (allontanati, viaggio rapido
o carica una partita).

### Controllare che funzioni

Apri `bin64\scripts\QuattroStagioni.log` con il Blocco note. Le righe importanti:

| Riga nel log | Significato |
|---|---|
| `version check: ... OK` | versione del gioco riconosciuta |
| `packs: folder ...` | il sistema dei pacchetti è attivo |
| `packs: A.qsp verified ...` | il pacchetto dell'autunno è stato controllato ed è in uso |
| `season: ..., stage A` | il calendario è arrivato allo stadio A |
| `overlay: bound to swapchain ...` | il menu F5 è pronto |
| `startup in ... ms: file hooks on, packs on, season on, overlay on` | tutto acceso |

Se qualcosa è `off` o compare `ERROR`, vedi [PROBLEMI.md](PROBLEMI.md).

## Aggiornare QuattroStagioni

Estrai la nuova versione in una cartella nuova e lancia il suo `qs_install.exe`: riconosce
l'installazione precedente, aggiorna i file, tiene le tue impostazioni e i pacchetti. Se la nuova
versione cambia le ricette, rigenera i pacchetti.

## Dopo un aggiornamento del gioco

La mod se ne accorge e si spegne da sola: il gioco funziona normalmente, senza stagioni. Quando
esce una versione di QuattroStagioni compatibile, installala e rigenera i pacchetti con
`genera_pacchetti.bat` (quelli vecchi vengono rifiutati). Dettagli in
[PROBLEMI.md](PROBLEMI.md#dopo-una-patch-del-gioco).

## Disinstallare

Doppio clic su **`disinstalla.bat`** (oppure `qs_install.exe --uninstall`). Vengono tolti tutti i
file installati, ripristinati quelli che erano stati sostituiti (`winmm.ini` compreso) e cancellati
i file creati dalla mod durante il gioco (log, stato del calendario). I pacchetti stagionali restano,
perché sono grandi e lunghi da rifare: per cancellare anche quelli usa

    qs_install.exe --uninstall --purge

Ultimate ASI Loader (`winmm.dll`) resta, perché potrebbe servire ad altre mod; se non ti serve più
cancellalo a mano (se l'aveva copiato l'installatore, viene tolto anche lui).

Disattivare la mod senza disinstallarla: in `bin64\scripts\QuattroStagioni.ini` metti `Enabled=0`.
