# Test locali sul PC di Stefano (branch `local/test-in-gioco`)

Risultati dei passi 1-5 di "Prossimi passi" in `HANDOFF.md`, eseguiti con i file veri del gioco.
Qui vanno solo esiti, conteggi e osservazioni: mai hash, offset, estratti o anteprime dei file del
gioco (repo pubblico). I bug trovati vanno in `docs/dev/BUGS.md`.

## 0. Ambiente (30/10/2026)

- Gioco: Steam, `C:\Program Files (x86)\Steam\steamapps\common\Crimson Desert`, build Steam
  aggiornata il 30/10 alle 10:47. L'exe corrisponde al hotfix del 30/10 già in `kKnownExes`
  (stessa dimensione e timestamp), `meta/0.paver` = 2.04.00 con DLC `dlc_01`.
- Gli archivi del DLC stanno in `dlc/dlc_01/<gruppo>/` (visto nel log del plugin), non solo nelle
  cartelle numerate della radice: da tenere presente per la verifica dell'inventario.
- Cartelle archivio in radice: da 0000 a 0035, senza 0018 e 0034.
- Strumenti: Git 2.56, Python 3.12.10. Gli eseguibili arrivano dall'artifact CI `QuattroStagioni-dist`
  in `dist-ci/` (ignorata da git).

### Mod manager DMM (`Desktop\Desert Mods`), controllo in sola lettura

- `config.json`: nessuna mod di archivio attiva (`activeMods` vuoto), nessuna texture
  (`activeTextures` vuoto). Unica ASI attiva: `Seasons` (il vecchio `Seasons.asi`, in `bin64\`).
- Ultima attività di DMM: 4/10. Il suo backup "vanilla" dei `.pamt` è di quella data, quindi
  precede le patch del 29-30/10: tutti i 37 file (pamt e meta) risultano diversi dal backup, ma
  hanno tutti data di modifica 29/10 sera o 30/10 mattina, cioè gli aggiornamenti Steam. Non sono
  modifiche di DMM.
- `winmm.ini` ha `LoadFromScriptsOnly=1`, quindi `bin64\Seasons.asi` non viene caricato.
- Conclusione: DMM non altera archivi né texture; non serve disattivarlo per i test. Per una
  conferma definitiva basta "Verifica integrità dei file" di Steam prima del passo 1.

### Stato di `bin64\scripts` trovato all'inizio

- È installato `QuattroStagioni.asi` 0.2.0 (fase 2, sostituzione della sola texture di test del
  logo di caricamento), installato il 30/10 alle 10:50.
- Log del 30/10: il controllo di versione è passato con l'exe del hotfix, gli hook sono stati
  installati in circa 170 ms e la texture di test è stata servita correttamente (HIT, patch in
  0,004 ms). Prima del riavvio, con l'elenco versioni vecchio, il plugin si era spento da solo
  come previsto (controllo di versione: MISMATCH).
- Al passo 3 questa installazione va sostituita con quella dell'artifact CI.

## 1. Verifica inventario (30/10/2026)

Strumenti: artifact CI `QuattroStagioni-dist` della build 9 (commit `7666317`), sha256 dello zip
uguale al digest pubblicato da GitHub. Verifica d'integrità di Steam non eseguita (scelta di
Stefano): la base è il gioco così com'è dopo il hotfix del 30/10.

Comando: `qs_gen.exe --game "<Crimson Desert>" --verify-inventory --report local-out\verify.txt`
(report e output restano in `local-out/`, ignorata da git).

| Voce | Esito |
|---|---|
| Voci controllate | 3688 |
| OK (indice, decodifica stretta, header DDS) | 3686 |
| Mancanti | 0 |
| Indice diverso | 2, solo l'offset nel paz (vedi BUGS.md, B1) |
| Errori di decodifica / header diversi | 0 / 0 |
| Coperte dal DLC (shadowed) / errori di gruppo | 0 / 0 |
| Tempo | 3 s (circa 980 MB letti, decodifica completa) |

Le due voci con offset diverso sono texture d'erba del terreno nel gruppo 0000. Il generatore le
salta (restano vanilla), quindi non è bloccante, ma l'inventario va aggiornato al hotfix.

### DLC e gruppi

- Tutti gli indici (`0.pamt`) della radice e di `dlc/dlc_01` hanno l'hash interno corretto.
- In radice mancano 0018 e 0034: nessuna voce dell'inventario li usa, nessun errore di gruppo.
- Il DLC ha 40 gruppi (`dlc/dlc_01/0000`-`0040`, alcuni vuoti) con circa 76.800 texture DDS:
  - circa 75.500 sono tile di settore del terreno in `leveldata/rootlevel` (colore, normali,
    speculare);
  - circa 750 in `character/texture`, circa 520 in `ui/texture`;
  - solo 34 in `object/texture`, tutte di oggetti e arredi (decorazioni, statue, gimmick), nessuna
    pianta.
- Cercando nomi di vegetazione (tree, grass, leaf, bush, plant, flower, moss, ivy, fern, reed, crop)
  nel DLC risultano solo 3 texture di alghe (`leaf_seaweed`, personaggi). **Il DLC non ha vegetazione
  propria**: conferma l'inventario v0. Le piante della regione del DLC riusano quelle del gioco base.
- Domanda aperta per la sessione cloud: i tile di settore del DLC possono contenere colore del
  terreno (prati) della nuova regione; oggi l'inventario include i tile `terrain` del gioco base ma
  non quelli del DLC.

## 2. Anteprime vere (30/10/2026)

Campione: 3 texture a caso (seme fisso) per ognuno dei 19 profili, 52 texture distinte, estratte
con `tools/index_browser.py extract`. Per ognuna `qs_preview --encode --fit --stages 1` con il seme
dell'asset. Texture, PNG e log restano in `local-out/` (script: `local-out/scripts/sample_previews.py`).

### Tasso di riuscita del fitter

- 52 texture su 52 elaborate, nessun errore di estrazione o di decodifica.
- **Nessun mip oltre la dimensione originale**: 91 mip ricompressi LZ4 tutti dentro il limite al
  primo tentativo (`fb=0`, mai servito il fitBias), gli altri sono mip salvati non compressi e
  rientrano per costruzione. Margine tipico tra il 10% e il 60% sotto la dimensione originale.
- Tempi: 80-370 ms per mip 1024, circa 1,2 s per il mip0 2048 della `global_colormap`.

### Osservazioni visive (prime 5 anteprime guardate)

- `deciduous` (impostor di betulla): inverno spoglio convincente (rami nudi, foglie tolte con
  l'alfa), autunno diverso per seme (seme 1 rosso, seme 10 giallo oliva spento). Primavera quasi
  identica all'estate e fine estate poco distinta: da tarare.
- `ground_grass` e `terrain_color` (erba del terreno): progressione buona, fine estate paglierina,
  autunno oliva scuro, inverno grigio slavato.
- Uno dei 3 tile `terrain_color` è quasi tutto nero già in vanilla (tile fuori mappa o di mare):
  lavoro e spazio sprecati, da escludere nell'inventario se sono tanti.
- `grass` con `weed_salvia_03` (salvia viola): nessun cambiamento visibile in nessuna stagione, la
  ricetta dell'erba non tocca i toni viola. Probabilmente va nel profilo `flower`.
- Una normal map classificata come colore: vedi BUGS.md, B2.

Taratura di `data/recipes/vegetation.ini`: in attesa del parere di Stefano sulla galleria
(`local-out/prev/index.html`).

## 3. Primo pacchetto in gioco (30/10/2026)

Installazione: contenuto di `dist-ci/bin64/scripts` in `bin64\scripts` (sha256 dell'asi verificato),
`winmm.ini` già identico, vecchia 0.2.0 salvata in `local-out/backup_0.2.0`. Nell'ini `Active=A`.
Restano `manifest.ini` e `test.bin` della fase 2 (logo di caricamento arancione), che funzionano ancora.

### Generazione dello stadio A

`qs_gen.exe --game ... --stage A --out "<bin64>\scripts\QuattroStagioni\packs"`, 43 s:

- 3688 voci, lo stadio cambia 3058 texture; 2963 a piena qualità, 8 con fitBias, 0 a forza
  ridotta, 0 lasciate vanilla per mancato fit: **tasso di fit 100%** (2971 su 2971 tentate).
- 630 non toccate dallo stadio, 85 identiche dopo la ricetta, 2 saltate per B1 (codice di uscita 3
  solo per queste).
- `A.qsp`: 2971 record in 20 archivi, 726 MB. `--check-pack`: tutto OK.

### Sessione di gioco (13:44-13:50, circa 5 minuti)

Dal log del plugin 0.3.0:

- Avvio in 41 ms: hook dei file, pacchetti, motore stagioni e overlay attivi; probe spenti.
  Riconosciuti Streamline, XeSS frame generation, overlay di Steam e l'Ultimate ASI Loader.
- Pacchetto A verificato in circa 10 ms a ogni attivazione.
- Menu F5: usato più volte (Autunno, Estate, Inverno, Autunno, Inverno), il config viene salvato
  dal menu ogni volta (`DaySource = manual`, `StartDay = 29` nell'ini).
- Lettura delle texture: circa 5.000 record serviti dal pacchetto, **0 rifiutati, 0 errori di
  payload, 0 letture in sospeso**; patch di una lettura tra 0,01 e 0,4 ms. Nessun segno di
  rallentamento dello streaming nel log (`WaitForTargetReads=1`).
- Cache: satura a 64 MB nei primi due minuti (prima del passaggio ad Autunno), poi 18-27 MB.
- Passaggio a Estate: pacchetto "none", corretto. Il cambio vale solo per le texture caricate
  dopo (limite noto, HANDOFF passo 6).
- Bug sulla scelta del pacchetto quando quello dello stadio non esiste: vedi BUGS.md, B3.

Osservazioni visive di Stefano:

- Autunno: "era tutto più autunnoso", effetto visibile su vegetazione e terreno.
- Estate: corretta (gioco originale).
- Inverno: poco diverso. Spiegazione: `W.qsp` non esiste, quindi per B3 restava attivo il pacchetto
  A (si vedeva l'autunno, non l'inverno). Da riprovare dopo aver generato `W.qsp`.
- Personaggio: nessun cambiamento possibile. L'inventario contiene solo `tree/`, `leveldata/`,
  `object/` (vegetazione e terreno), nessuna texture di `character/`, e nel log non c'è nessuna
  lettura del pacchetto su `character/`. Luce e meteo per stagione non sono ancora agganciati
  (probe spenti), quindi anche l'illuminazione del personaggio è quella originale.

## 4. Menu F5 in gioco

Saltata su scelta di Stefano (30/10). Quello che risulta dalla sessione della tappa 3, con frame
generation XeSS attiva: il menu si apre, 5 cambi di stagione applicati, config salvato 8 volte
dal menu, nessun errore dell'overlay (né sul subclass della finestra né sulla GPU). Non verificati:
blocco dell'input, frame generation spenta, persistenza dopo un riavvio, leggibilità delle schede.
Nota per la sessione cloud: il plugin non scrive nel log apertura/chiusura del menu né lo stato del
blocco input; una riga INFO per ciascuno renderebbe verificabile questa tappa dal solo log.

## 5. Probe (A1-b)

### Primo giro, sola lettura (30/10/2026, 14:11-14:17)

`probes.ini` dell'artifact con `Enabled=1`, `AllowHooks=0` (weather_manager spento). Annotazioni
di Stefano dall'HUD:

| Ora reale | HUD | Note |
|---|---|---|
| 14:12 | 19:13, domenica, giorno 245 | dopo il caricamento |
| 14:14 | 19:27 | |
| 14:15 | 19:39 | apre la mappa |
| 14:16 | 19:42 | chiude la mappa |

Risultati (`probe.log`):

- **Exe e sezioni come previsto**: timestamp 0x6AE387F2; una sola sezione di codice `.impdata` a RVA
  0x1000; `.00cfg`, `.tls`, `.rdata` marcate OBFUSCATED (mai lette). Indice RTTI: 15.360 classi.
- **Firme**: tutte e 4 con una sola corrispondenza (`clock_a`, `clock_b`, `cvar_snow_start`,
  `cvar_snow_full`).
- **GameTime pubblicato** (`clock_b` a RVA 0x6E9C998, `clock_a` a 0x6E9C9B8, nella sezione dati):
  - al menu principale vale giorno 1, 12:00; dopo il caricamento giorno 245, 19:13, che
    **coincide con l'HUD** alle 14:12 (giorno e ora). Il giorno della settimana a `+0x14` vale 0 con
    "domenica" e 245 % 7 = 0: compatibile con weekday = day % 7, domenica = 0.
  - `rate` a `+0x16` vale **12**, non 24 come atteso dal piano.
  - **Non è un orologio vivo**: due letture a circa 105 s di distanza (14:13:50 e 14:15:36) danno
    entrambe 19:13, mentre l'HUD avanza fino a 19:39. Sembra una fotografia presa al caricamento
    (o a eventi), non aggiornata a ogni frame. Per `DaySource::GameClock` serve l'oggetto vivo:
    secondo giro con capture su `GameTime_Publish` / `GameTime_Get`.
- **Velocità del tempo dall'HUD**: 29 minuti di gioco in circa 4 minuti reali (circa 7 min di gioco
  al minuto). Con la mappa aperta l'orologio avanza ancora (19:39 -> 19:42 in circa un minuto),
  forse più lento: annotazioni al minuto, troppo grossolane per dirlo.
- **Cvar della neve confermate**: SnowStart -5, SnowFull -20 (valore e default, classe
  `pa::GlobalVariableFloat`, RVA 0x71A8D10 e 0x71A8D60).
- **Scansione dell'heap inutilizzabile così com'è**: `ServerWeatherManager`,
  `weatherTimeSimulatorScriptComponent` e `PAClimateTexture` danno 0 oggetti, ogni scansione dura
  circa 35 s e blocca i campionamenti delle altre probe. Vedi BUGS.md, B4.
- Classi utili viste: i 6 `scriptComponent` del meteo (Cloud, Effect, Simulator, Surface,
  Time, worldWeatherEffect, 20 slot ciascuno), `ConditionData_IsCoveredBySnow`,
  `GimmickEventHandlerData_ClearCoveredSnow`, `SequencerGamePlayEventSetTimeOfDay_ITimelineEventData`,
  `UITimeOfDayEvent`. Nessuna classe `*Environment*` legata al meteo.

### Indirizzi per `targets.ini` (exe hotfix 30/10, timestamp 0x6AE387F2)

Da inserire in `targets.ini` quando la sessione che scrive `src/win/apply/` ne fissa il formato.
Le RVA valgono solo per questo exe; le firme sono quelle di `probes.ini`.

| Bersaglio | Come trovarlo | RVA | Stato |
|---|---|---|---|
| GameTime pubblicato, copia B | firma `clock_b` | 0x6E9C998 | confermato, ma non vivo |
| GameTime pubblicato, copia A | firma `clock_a` | 0x6E9C9B8 | confermato, ma non vivo |
| cvar SnowStart (valore a +0x38) | firma `cvar_snow_start` | 0x71A8D10 | confermato (-5) |
| cvar SnowFull (valore a +0x38) | firma `cvar_snow_full` | 0x71A8D60 | confermato (-20) |
