# Domande frequenti

### Cosa fa QuattroStagioni?

Porta le stagioni in Crimson Desert: primavera fiorita, l'estate originale, una fine estate secca e
dorata, un autunno arancio e rosso, un inverno con alberi spogli ed erba brinata. Il paesaggio
cambia un po' alla volta con un calendario che puoi regolare dal menu **F5**.

### Modifica i file del gioco? Posso rovinare l'installazione o i salvataggi?

No. La mod non scrive mai nei file del gioco né nei salvataggi. Le texture stagionali vengono
servite al gioco "al volo" mentre le legge, e solo dopo aver controllato che i byte siano proprio
quelli attesi. Se qualcosa non torna, quella texture resta originale. Per tornare al gioco
originale basta `Enabled=0` nel file ini o `disinstalla.bat`.

### Perché devo generare i pacchetti sul mio PC?

Perché le texture stagionali sono derivate da quelle del gioco, che appartengono a Pearl Abyss e
non si possono ridistribuire. La mod distribuisce solo codice e "ricette" (come cambiare i colori);
il generatore `qs_gen` applica le ricette ai **tuoi** file. Così i pacchetti corrispondono anche
esattamente alla tua versione del gioco.

### Quanto spazio serve?

Circa 1,6 GB per pacchetto. Le quattro stagioni principali: circa 6,5 GB. Con tutte le transizioni
graduali (3 stadi intermedi per passaggio, 19 pacchetti): circa 30 GB. Puoi mettere i pacchetti su
un altro disco (`Folder` nella sezione `[Packs]`, vedi la [Guida](GUIDA.md)).

### Pesa sulle prestazioni?

Il plugin controlla le letture degli archivi e, per le texture interessate, legge i dati dal
pacchetto invece che dall'archivio. Il costo dovrebbe essere piccolo, ma **non è ancora stato
misurato nel gioco vero**: se noti scatti nello streaming, segnalalo con il log.

### Funziona con il DLC "Charting the Unknown"?

La mod è preparata per la 2.04.00 con il DLC installato. Nelle zone del DLC la vegetazione propria
del DLC (se c'è) non è ancora nell'elenco delle texture: resta com'è.

### Funziona con Game Pass, Epic o versioni non Steam?

È preparata e controllata solo per la versione Steam. Su altre versioni il controllo di versione la
spegne da sola.

### Funziona su Steam Deck o Linux (Proton)?

Non è stato provato. Ultimate ASI Loader funziona sotto Proton se imposti nelle opzioni di avvio
`WINEDLLOVERRIDES="winmm=n,b" %command%`. Il menu F5 usa D3D12 e dovrebbe funzionare, ma senza
garanzie.

### Posso usarla con Seasons of Pywel?

No, le due mod fanno la stessa cosa. Non serve disinstallare Seasons: l'installatore imposta il
loader in modo da caricare solo le mod nella cartella `scripts`, e `Seasons.asi` sta in `bin64`.

### E con ReShade, RenoDX, OptiScaler, Crimson Weather?

ReShade e RenoDX: dovrebbero convivere (il menu F5 gestisce HDR e ReShade). OptiScaler: sono stati
segnalati crash con altre mod ASI, prova prima senza. Crimson Weather: oggi QuattroStagioni non
tocca il meteo, quindi convivono. Vedi [PROBLEMI.md](PROBLEMI.md#conflitti-con-altre-mod).

### Il meteo, la neve, le giornate corte e le foglie che cadono funzionano?

Non ancora. La mod li **calcola** già (li vedi nel menu, scheda *Meteo e luce*) ma gli agganci ai
sistemi del gioco sono in sviluppo. Oggi cambiano le texture della vegetazione e del terreno.

### Ho cambiato stagione dal menu ma gli alberi intorno a me sono uguali.

Il cambio vale per quello che il gioco carica dopo. Allontanati, usa il viaggio rapido o ricarica
la partita. La sostituzione delle texture già caricate è uno dei prossimi passi.

### In autunno alcuni alberi restano verdi.

Può essere voluto (conifere e sempreverdi restano verdi; la *varietà per pianta* fa cambiare alcuni
alberi prima o dopo) oppure una texture che il generatore non è riuscito a far rientrare nella
dimensione originale (il riepilogo di `qs_gen` lo dice con il *tasso di fit*). Le ricette verranno
affinate con le prove nel gioco.

### Quanto dura un anno?

Con le impostazioni predefinite 38 giorni di calendario. Con il tempo di gioco a 90 minuti reali
per giorno, circa 57 ore di gioco. Puoi accorciarlo o allungarlo nella scheda *Stagioni* o con
*Minuti reali per giorno*.

### Si può scegliere la stagione e fermare il calendario?

Sì: menu F5, scheda *Calendario*, *Vai all'inizio di* oppure *Sorgente del giorno: Manuale*.

### Il gioco si è aggiornato e la mod non fa più niente.

È voluto: dopo una patch la mod si spegne da sola per sicurezza. Aspetta la versione aggiornata,
installala e rigenera i pacchetti. Vedi [PROBLEMI.md](PROBLEMI.md#dopo-una-patch-del-gioco).

### L'antivirus dice che il file è pericoloso.

È un falso positivo comune per i plugin `.asi`. Il codice è pubblico e ogni file della release ha
il suo SHA-256 in `SHA256SUMS`. Vedi [INSTALLAZIONE.md](INSTALLAZIONE.md#3-windows-defender-e-antivirus).

### Posso condividere i miei pacchetti `.qsp`?

No: contengono dati derivati dai file del gioco. Ognuno genera i propri.

### Come si disinstalla?

`disinstalla.bat` (o `qs_install.exe --uninstall`) rimette tutto com'era. Con `--purge` cancella
anche i pacchetti generati.

### Dove segnalo un problema o un'idea?

Nella pagina GitHub del progetto (sezione *Issues*), con `controllo.txt` e il log. Vedi
[PROBLEMI.md](PROBLEMI.md#segnalare-un-problema).
