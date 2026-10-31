# Handoff QuattroStagioni (aggiornato 31 ottobre 2026, branch claude/funzioni-finali)

## Obiettivo
Mod pubblicabile di stagioni dinamiche per Crimson Desert 2.04 con DLC "Charting the Unknown".
Requisiti di Stefano (tutti):
1. Fase 3: estrazione fogliame e varianti stagionali (primavera, estate, fine estate secca, autunno, inverno spoglio).
2. Transizioni graduali (stadi intermedi nel corso dei giorni).
3. Varietà: fioriture, prati secchi, alberi spogli, differenze per regione (deserto escluso).
4. Meteo coerente con la stagione, neve che si accumula in inverno, estati limpide.
5. Menu F5 curato: calendario, stagione, lunghezza stagioni, opzioni per regione.
6. Installazione pulita con controllo di versione (dopo una patch la mod si spegne da sola).
7. Luce per stagione (sole più basso, toni freddi in inverno), se agganciabile.
8. Giornate più corte d'inverno, se agganciabile il ciclo giorno/notte.
9. Particelle: foglie che cadono, fiato visibile al freddo, riusando effetti del gioco.

Documenti chiave: `docs/DESIGN.md` (visione e architettura), `docs/dev/CONTRACTS.md` (moduli e
interfacce), `docs/GENERATORE.md` (uso di qs_gen), `docs/dev/season_engine.md`,
`docs/dev/texture_recipes.md`, `docs/recon/probe_plan.md`, `docs/recon/recon_D_web.md`.

## Stato (30/10, fine sessione cloud)
Tutto compila (`./build.sh test`: 6 binari di test, oltre 72.000 controlli, tutti verdi) e la CI su
GitHub Actions produce un pacchetto `QuattroStagioni-dist`. NIENTE di quanto segue è ancora stato
provato sui file veri o nel gioco: è stato provato su dati sintetici, sotto wine e con D3D12 software.

| Modulo | Cartelle | Stato |
|---|---|---|
| Inventario v1 | `data/vegetation_classes_v1.json`, `tools/classify_inventory.py` | 3688 texture bersaglio con profilo stagionale, ~1,6 GB per look. Basato sull'inventario v0 non verificato |
| Codec texture | `src/core/texture/` | BC1/BC3/BC4/BC5/raw, deterministico (stesso output Linux/Windows), fitBias per rientrare nella dimensione LZ4 |
| Ricette | `src/core/recipe/`, `data/recipes/vegetation.ini` | 19 profili in OKLab, palette autunnali per seme, caduta foglie, interpolazione tra look |
| Generatore | `src/core/gen/`, `src/core/pack/`, `src/core/crypto/`, `src/cli/qs_gen.cpp` | lettura archivi (ChaCha20, LZ4, mip), fitter, pacchetti `.qsp`, verifica inventario |
| Plugin: file | `src/win/io_hooks.*`, `setup.*` | serve migliaia di texture da un pacchetto, verifica a blocchi da 4 KiB, cambio pacchetto a runtime |
| Motore stagioni | `src/core/season/`, `src/cli/qs_season.cpp` | calendario, 20 stadi, regioni, temperatura, meteo, neve, luce, alba/tramonto, velocità giorno/notte |
| Tick stagioni | `src/win/season_tick.*` | calendario nel plugin, stato in `QuattroStagioni\state.ini`, pacchetto attivo = stadio della regione temperata |
| Overlay e menu F5 | `src/win/overlay/`, `third_party/imgui/` | hook DXGI di sistema, menu in 5 schede, HDR, coesistenza ReShade/Streamline; demo `tools/overlay_demo/` |
| Probe runtime | `src/win/probe/`, `src/core/probe/`, `dist/.../probes.ini` | RTTI, firme, hook di sola cattura, guidati da ini; provati su un finto gioco sotto wine |
| Anteprime | `src/cli/qs_preview.cpp`, `docs/img/` | contact sheet di look e transizioni da PNG o DDS |

Strumenti Windows (in `build/win/` o nell'artifact della CI): `qs_gen.exe`, `qs_preview.exe`,
`qs_season.exe`, `qs_lz4exact.exe`.

## Aggiornamento finale (30/10, sera)
**Provato in gioco (sessione locale, `docs/recon/test_locali.md`):** inventario 3686/3688 OK, DLC senza
vegetazione propria, fitter 100% (pacchetto A: 2971 texture, 726 MB, 43 s), in gioco circa 5000
texture servite con 0 errori e senza rallentamenti, autunno ben visibile, menu F5 funzionante anche
con frame generation XeSS.

**Fatto dopo il test:** B2 corretto (normal map escluse per nome, salvia nel profilo `flower`);
primavera e fine estate più marcate sulle latifoglie (`data/recipes/vegetation.ini`); installer
`qs_install.exe` + guide utente (`docs/utente/`) + README + workflow di rilascio sui tag `v*`;
lista unica delle build note (`src/core/known_builds.h`, usata da plugin e installer); lingua
dell'interfaccia come il gioco (italiano incluso, inglese da `lang/en.txt`, `[General] Language=`).
Nota: le ultime modifiche (B2, ricette, known_builds, lingua) hanno solo controlli rapidi: la build
completa e i test li fa la CI su GitHub, controllare che l'ultima esecuzione sia verde.

**Lavoro interrotto per budget, su branch separati (situazione superata, vedi sotto):**
- `wip/pacchetti-per-texture`: pacchetti v2 con regione e seme per texture, più pacchetti aperti,
  `qs_gen --plan`. Doveva includere anche B1 (offset dal pamt vivo) e B3 (stadio senza pacchetto).
- `wip/gpu-swap`: nucleo portabile del cambio texture su GPU (impronte, indice, scheduler) + test.
- `wip/agganci-gioco`: schema `targets.ini`, meteo, orologio, controller con ripristino.

**Bug:** B1, B2 e B3 corretti, da riprovare in gioco (`docs/dev/BUGS.md`).

**Prossimo per la sessione locale:** tappa 5 (probe, `docs/recon/probe_plan.md`), poi riprova
Autunno/Inverno con il nuovo artifact generando sia `A.qsp` sia `W.qsp`.

## Branch `claude/neve-inverno` (30/10, notte tardi)
- **Neve d'inverno**: modulo dedicato acceso per default, sezione `[Snow]` (chiavi facoltative) in
  `QuattroStagioni.ini`; alza `SnowStart`/`SnowFull` solo in inverno con rampa e ripristino.
  Dettagli e righe di log in `docs/dev/season_engine.md`; test `tests/test_snow.cpp`. Da provare in gioco.
- **B5** corretto (override `Active=` rilasciato da menu e calendario), da riprovare in gioco.
- Obiettivi meteo di Stefano per la prossima tappa annotati in `docs/dev/season_engine.md`.

## Aggiornamento sessione cloud (30/10, notte)
- **CI**: era rossa da tre esecuzioni. Cause: `test_gen` si aspettava 3688 voci (dopo B2 sono 3686) e
  `tools/make_release.sh` non copiava la cartella `lang/`. Corrette entrambe; B1 e B3 (uniti con
  `wip/pacchetti-per-texture`) sono "corretti, da riprovare in gioco" in `docs/dev/BUGS.md`.
- **Applier uniti** (`wip/agganci-gioco`): `src/core/apply/`, `src/win/apply/`, `tests/test_apply.cpp`
  (149 controlli), `docs/dev/appliers.md`, `targets.ini`. Collegati in `src/win/main.cpp`
  (`applyStart()` dopo `probeStart()`, `applyStop()` prima di `probeStop()`); in
  `src/win/season_tick.cpp` con `DaySource=gameclock` il calendario usa le ore del gioco quando
  `applyReadGameClock()` le fornisce. `targets.ini` esce TUTTO SPENTO: in gioco non cambia nulla
  finché non si inseriscono gli indirizzi trovati con i probe.
- **GPU swap**: unito solo il nucleo portabile (`src/core/gpu/`, `tests/test_gpu.cpp`, 142 controlli),
  non chiamato da nessuno. La parte Windows resta su `wip/gpu-swap` (compila, mai provata). Stato e
  passi in `docs/dev/gpu_swap.md`.
- **Menu F5, scheda Info**: stato vero dei pacchetti (aperti, texture servite, stadi mancanti, ultimo
  problema), stringhe inglesi in `lang/en.txt`.

**Prossimo per la sessione locale:** merge di `origin/claude/crimson-desert-handoff-to1i0s` in
`local/test-in-gioco`; nuovo artifact, `qs_install.exe --check` poi installazione; `qs_gen --stage A,W`;
riprova Autunno e Inverno (controllare nella scheda Info che compaiano A e W e nessuno stadio
mancante); tappa 5 (probe, `docs/recon/probe_plan.md`), con gli indirizzi trovati in `targets.ini`
secondo `docs/dev/appliers.md` (un target alla volta, partendo dall'orologio).

## Prossimi passi (sessione LOCALE sul PC di Stefano, con i file del gioco)
1. **Verifica inventario**: `qs_gen.exe --game "<Crimson Desert>" --verify-inventory --report verify.txt`.
   Controllare anche se il DLC ha vegetazione propria (nell'inventario v0 il DLC ha solo tile di dati
   del terreno) e se mancano gruppi.
2. **Anteprime vere**: estrarre qualche texture per profilo (`tools/index_browser.py extract`) e
   lanciare `qs_preview` con `--fit` per misurare il tasso di riuscita reale e tarare
   `data/recipes/vegetation.ini`. Le anteprime restano sul PC (mai nel repo).
3. **Primo pacchetto**: `qs_gen.exe --game ... --stage A --out "<bin64>\scripts\QuattroStagioni\packs"`,
   poi in gioco con il menu F5 in modalità Manuale su Autunno. Guardare il log (`pack A: ...`).
   Tenere d'occhio i rallentamenti dello streaming (`WaitForTargetReads=1`).
4. **Menu in gioco**: prima con frame generation spenta. Verificare F5, input bloccato, salvataggio.
5. **Probe (A1-b)**: `probes.ini` con `Enabled=1`, `AllowHooks=0`, seguire `docs/recon/probe_plan.md`:
   orologio, velocità giorno/notte, meteo, neve, luce. Scrivere `tools/sigscan.py` (non consegnato:
   la scrittura è stata interrotta da un controllo automatico) o verificare le firme con i probe.
6. **Poi**: hook di meteo/neve/luce/durata del giorno con i valori di `SeasonState`; temperatura sulla
   `climate_texture_1.dds` per stagione; sostituzione GPU delle texture già caricate quando cambia lo
   stadio (oggi il cambio vale solo per le texture caricate dopo); pacchetti per giorno con la
   varietà per albero e le regioni (oggi un pacchetto = uno stadio globale); particelle.

## Note pratiche
- Build: `./build.sh test` su Linux (g++, mingw-w64; wine per i test Windows facoltativi). Circa 3 minuti.
- Il repo è PUBBLICO: mai file del gioco o derivati (texture estratte, anteprime da texture vere).
- Installazione: contenuto di `dist/bin64/` in `bin64\` più `QuattroStagioni.asi` in `bin64\scripts\`.
  `winmm.ini` con `LoadFromScriptsOnly=1` evita di caricare il vecchio `Seasons.asi`.
- Versioni exe riconosciute (src/win/setup.cpp, kKnownExes): 333779352/1793246886 (2.04.00) e
  336264088/1793296370 (hotfix 30/10). Dopo ogni patch ricontrollare exe e hash dei pamt.
- La consegna file sul PC ha restituito una volta copie in cache: verificare lo sha256 dopo l'installazione.

## Sessione locale 30/10 sera (interrotta per budget)
- Installato sul PC: build `claude/neve-inverno` (8574a5b), neve d'inverno + B5. Da testare in gioco.
- Generati sul PC tutti i 19 pacchetti (S U L A W + 15 transizioni), 0 errori.
- `wip/gpu-live`: cambio in diretta su GPU interrotto a metà (non compilato).
- `claude/meteo-probe`: correzione B4 (scansione heap a fette di tempo, riprendibile, salta regioni enormi; chiavi ScanSliceMs, ScanRegionMaxMB) e probe dei simulatori meteo in probes.ini. NON compilato né testato: prossimo passo `./build.sh test`, poi giro di probe in gioco (probes.ini con Enabled=1, AllowHooks=0) mentre cambia il tempo.
- Spunto: Seasons controllava due funzioni (query meteo HUD, update meteo) con 18-20 byte di prologo; quei byte (codice del gioco, non della mod) possono servire come firma per ritrovarle nella build nuova (vedi docs/recon/recon_B_report.md).
- Regole meteo di Stefano: primavera soleggiata e poco piovosa; estate poca pioggia ma temporali; autunno pioggia frequente e temporali; neve solo in inverno e mai nel deserto.
- TEST NEVE 30/10 (Stefano, in gioco): con WinterSnowStart=10, WinterSnowFull=0, MaxSnowStart=10 la neve compare nelle zone temperate in inverno ed è ASSENTE nel deserto ("da manuale"). Con 2 / -6 non si vedeva (la mappa del clima resta calda). Prossimo: rendere 10 / 0 il default invernale (tetto 10) oppure abbassare la mappa del clima per stagione; verificare il deserto di notte.
- Richiesta di Stefano: in inverno il cielo non deve essere azzurro. Va con il meteo: cielo coperto/nuvoloso prevalente d'inverno (probabilità ini).
- PROBE METEO 30/10 (build claude/meteo-probe, sola lettura): B4 risolto, un giro completo dell'heap (circa 14,9 GB, 6.500 regioni) in circa 10 minuti senza bloccare le altre probe. Trovate istanze VERE di ServerWeatherManager, PAClimateTexture, weatherTimeSimulator, WeatherCloudSimulator (e simulatori meteo/superficie). Falso positivo noto: indirizzo 0xC31FE00 = buffer della scansione stessa (scartato in automatico quando cambia; da escludere per intervallo di indirizzi, non per base della regione). Nessun cambio meteo durante la sessione: i dump sono in `bin64\scripts\QuattroStagioni\probe.log` sul PC di Stefano. Prossimo: giro con cambio di tempo (anche facendo passare le ore), confronto dei dump, poi test di scrittura con il setter del meteo manuale (`_isManualWeather`, vedi recon_C).
- CAMBIO METEO OSSERVATO 30/10 circa 16:05-16:10 (ora reale; probe.log circa t=1100-1400 s): HUD da "vento" a "nuvoloso". Nei primi 0x200 byte di ServerWeatherManager e WeatherCloudSimulator NESSUN valore è cambiato (1645 campioni): lo stato meteo non è lì, probabilmente in sotto-oggetti puntati. WeatherSurfaceSimulator +0x3C varia (forse bagnato/umidità). Prossimo: seguire i puntatori dei due oggetti (dump più ampi, catene), oppure catturare chi chiama le query meteo dell'HUD (spunto: prologo usato da Seasons).
- TEST RAIN NECKLACE 30/10 (probe.log): riferimento t=1532 s, collana attivata a t=1562 s, nebbia visibile a t=1595 s (pioggia non ancora). Nei dump 0x200 di ServerWeatherManager, WeatherCloud e WeatherSimulator nessuna variazione; varia solo WeatherSurface +0x3C, che cresce in modo lineare (sembra un contatore/tempo, non lo stato meteo). Conferma: lo stato sta in sotto-oggetti. La collana usa probabilmente ChangeWeatherBuff (Client/CommonChangeWeatherBuffProcessor, ChangeWeatherBuffData): ottimo punto per un hook di cattura (AllowHooks=1) che riveli la funzione che imposta il meteo.
- PIOGGIA iniziata a t=1633 s (Stefano: "pioggia ora" a t=1641). WeatherSurfaceSimulator (istanza vera) reagisce: +0x20 e +0x30 (f32, uguali) salgono da 0 (0,00003 -> 0,034 in 8 s: bagnato?), +0x40 f32 circa 19,96 -> 19,55 in calo (temperatura superficie in gradi?), +0x4C e +0x50 salgono (0,076 -> 0,086 e 0,30 -> 0,34; +0x50 = 4 x +0x4C circa: accumulo/pozzanghere?). Effetti a valle della pioggia, utili come "sensore" per verificare i futuri comandi meteo; la causa va cercata nel percorso ChangeWeatherBuff.
- TEMPORALE (tuoni e fulmini) segnalato da Stefano a t=1723 s, dopo la pioggia iniziata a t=1633: utile per distinguere pioggia e temporale nei dati.
- FINE PIOGGIA segnalata da Stefano a t=1914 s. Ciclo completo nel probe.log: collana 1562, nebbia 1596, pioggia 1633, temporale 1723, fine 1914.

## Pista meteo e clima (dalle note di ricerca pubbliche di Seasons, solo fatti; NON copiare codice né testo)
Stefano ha condiviso le note di ricerca di Seasons 1.0.5 (porting sulla build 6AE387F2, la nostra). Fatti utili da verificare con le nostre probe:
- Neve: i valori (+0x38) dei nostri cvar coincidono con quelli citati (0x71A8D48 / 0x71A8D98); esistono altre due soglie per la neve profonda a 0x71A8DE8 e 0x71A8E38.
- Meteo visibile: il gestore del clima compone ogni tick i livelli meteo in un WeatherConstant raggiungibile da [[climate mgr + 0x60] + 0x20]; nella build nuova pioggia a +0x170, neve a +0x16C, copertura nuvolosa a +0xAC. Scrivere lì DOPO la composizione cambia pioggia, neve, nuvole e HUD. Il blocco toccato dalla collana (precipitation/cloudiness/humidity) NON è il meteo visibile: spiega perché le nostre probe non vedevano cambiamenti.
- Radice del clima a 0x71AEA58 (catena +0x50 -> +0xF70 -> manager), giorno a 0x6E9C998, vtable PAClimateTexture 0x5E8B7C0, query meteo HUD 0x1933C70 (flag: piovoso, nevoso, ventoso, nebbia, nuvoloso, sereno, tempesta), update meteo 0x40B71E0.
- Temperature: la mappa del clima in memoria (PAClimateTexture, 512x512, canale B = temperatura massima del giorno) si può scrivere dal vivo: temperatura e neve profonda cambiano subito. Serve un limite minimo per le notti (il freddo sotto circa -40 ferisce).
- Il controllo dell'indice del gioco hasha il pamt dal byte 12 (hashlittle 0xC5EDE); papgt ora firmato: mai toccarlo (coerente con il nostro design).
Prossimo passo meteo: probe che legge [[mgr+0x60]+0x20] +0xAC/+0x16C/+0x170 durante pioggia/temporale (verifica), poi applier "dopo la composizione" con le regole di Stefano per stagione e il cielo coperto d'inverno; poi temperature stagionali sulla mappa del clima (deserto escluso).

## Branch `claude/meteo` (31/10): meteo stagionale
- Porting adattato del meteo di Seasons of Pywel (permesso scritto di Khione95): `src/core/apply/weather.*`
  (regole, periodi, composizione, test `tests/test_weather.cpp`) e `src/win/apply/weather_win.*` (hook
  update climate manager + query HUD per firma unica, scrittura dopo la composizione, deserto dalla mappa
  del clima CPU). Menu F5: scheda Meteo e luce, blocco "Meteo nel gioco"; riga in Info. Dettagli in
  `docs/dev/season_engine.md`.
- Default neve: WinterSnowStart=10, WinterSnowFull=0, MaxSnowStart=10 (testati in gioco).
- ATTENZIONE build: il 31/10 l'exe sul PC di Stefano era 0x6AE46DD0 (332.786.072 byte) con
  meta/0.paver 2.04.02: aggiunti a `known_builds.h` (prima la mod si sarebbe spenta da sola). Le firme
  meteo e neve sono uniche su 0x6AE387F2 e 0x6AE46DD0. Dopo una patch: rifare la verifica.
- Da testare in gioco: vedi INSTALL.txt della consegna (collana della pioggia, F5, righe `weather:` del log).
  Temporali: per ora pioggia fortissima, fulmini non controllati. Deserto: soglia `DesertTopTempC=28`
  da verificare (anche la valle di Tashkalp, terra normale dentro il deserto).

## Branch `claude/temperature` (31/10): temperature stagionali
- Da `claude/meteo`. Porting adattato di climate.cpp di Seasons (permesso di Khione95):
  `src/core/apply/climate.*` (differenze per fase sfumate nella coda, deserto con bordo, pavimento
  delle notti -30, regola 3x3 dei luoghi freddi; test `tests/test_climate.cpp`) e
  `src/win/apply/climate_win.*` (vtable da RTTI, scansione heap, copia dell'originale, scrittura,
  ripristino). Dettagli e scelte (neve profonda TODO, soglie della neve tenute) in
  `docs/dev/season_engine.md`.
- Default `[Climate]`: primavera -6, estate +4, fine estate +2, autunno -8, inverno -24; interruttore
  F5 "Temperature stagionali" (`ClimateControl`).
- NON provato in gioco. Sul build 0x6AE46DD0 nulla è verificato: la vtable si cerca per nome RTTI
  (il log dice se coincide con 0x5E8D890, l'indirizzo di Seasons 1.0.6 per quel build).
- Da provare: F5 Info/Meteo e luce, termometro in inverno (circa -24 rispetto all'estate del gioco),
  deserto invariato, montagne innevate invariate, nessun danno da freddo di notte nei forti, ripristino
  spegnendo l'interruttore (temperature del gioco subito).

## Branch `claude/gpu` (31/10): tappa 3, cambio in diretta su GPU
- Da `claude/temperature` + `wip/gpu-live` completato (approccio di Seasons, permesso di Khione95):
  le texture già in VRAM passano al nuovo stadio al tick del calendario o con F5, distribuite sui frame
  (`[GpuSwap] MaxMBPerFrame=4`). Impronte lette in ExecuteCommandLists (buffer completi). A qualunque
  errore si spegne da solo (`gpu: DISABLED: <motivo>`). Riga F5 Info "Cambio texture su GPU".
  Dettagli e cosa guardare nel log: `docs/dev/gpu_swap.md`.
- Stato delle tre tappe: meteo (`claude/meteo`), temperature (`claude/temperature`), GPU (`claude/gpu`,
  contiene le altre due): compilano e i test sono verdi, NESSUNA provata in gioco.
- Il PC ha la build 2.04.02 (exe 0x6AE46DD0): i pacchetti `.qsp` potrebbero dover essere rigenerati con
  `qs_gen` se la verifica della build li rifiuta (scheda Info: stadi mancanti; log `pack ...`).

## Branch `claude/funzioni-finali` (31/10): funzioni finali prima della v0.1.0
Da `claude/gpu` (1e52bba). Un commit per tappa: base comune, fulmini, luce, durata del giorno,
particelle; poi preparazione della release (versione 0.1.0 nel plugin e in qs_gen, CHANGELOG, guide
utente, LEGGIMI, sezione `[Climate]` nell'ini, kit di test). Compila (`./build.sh test` verde) e i test unitari coprono la logica pura; NULLA è provato
in gioco. Il tag v0.1.0 NON è creato: lo decide Stefano dopo il test.

**Fatto**
- Base comune: le funzioni scrivono dopo la composizione del gioco usando l'hook del meteo già
  verificato su 2.04.02 (nessuna firma nuova). Con `[Weather] Enabled=0` l'hook resta e il meteo non
  scrive. Stato di ogni funzione nella scheda Info del menu F5; al primo errore la funzione si spegne
  da sola con la riga `<funzione>: DISABLED: <motivo>`. Probe nuove `Instance=provided`
  (`Provider=climate_manager`, `Chain=`): leggono i blocchi senza un secondo hook. Guida:
  `docs/dev/probe_funzioni_finali.md`. `ExpectTimestamp` di probes.ini e targets.ini ora 0x6AE46DD0.
- Fulmini (`[Lightning]`, F5 "Fulmini"): temporali con fulmini e lampi per ora di gioco per stagione
  (estate e autunno alti, inverno quasi zero, deserto zero). Il tuono resta quello del gioco. Guida
  `docs/dev/fulmini.md`.
- Luce e cielo (`[Lighting]`, F5 "Luce stagionale"): moltiplicatori per S U L A W di sole, ambiente,
  nebbia e cielo, interpolati sugli stadi (15 transizioni), estate = gioco. Guida `docs/dev/luce.md`.
- Durata del giorno (`[DayLength]`, F5 "Durata del giorno"): alba e tramonto per stagione rimappando
  l'ora del sole, senza toccare la velocità dell'orologio; la tabella del sole si impara in gioco
  (`QuattroStagioni\sun_table.ini`, solo sul PC). Guida `docs/dev/durata_giorno.md`.
- Particelle (`[Particles]`, F5 "Particelle"): fiocchi più fitti d'inverno sul campo neve +0x16C già
  verificato (ACCESO, boost 1.25, minimo 0.35, solo quando nevica già). Guida `docs/dev/particelle.md`.

**Spento in attesa delle probe** (sezioni in `bin64\scripts\QuattroStagioni\targets.ini`, tutte `Enabled=0`)
| Funzione | Sezione targets.ini | Cosa manca | Probe |
|---|---|---|---|
| Fulmini | `[lightning]` Field, Mode | campo che fa scattare il lampo | lightning_*, layer_weather, layer_table, layer_slot_*, list_lightning |
| Luce | `[lighting]` SunColor, SunIntensity, AmbientColor, AmbientIntensity, FogDensity, SkyColor | offset nell'atmosfera | layer_atmosphere(_tail), list_light/sky/fog |
| Durata del giorno | `[daylength]` Field, Kind | direzione del sole (o ora del cielo); anche GameSunrise/GameSunset reali | layer_atmosphere |
| Foglie e pollini | `[particles]` Leaves, Pollen | un campo pilotabile (forse non esiste) | list_particle/effect/leaf/wind/foliage, layer_weather |
Senza campo ogni modulo gira lo stesso e scrive nel log cosa farebbe (utile per controllare frequenze e orari).

### CHECKLIST DI TEST IN GIOCO (unica, su tutto)
Versione breve da spuntare: `dist/dev/TEST_IN_GIOCO.txt`. Nell'artifact della CI (build senza tag) la
cartella `sviluppo/` contiene questo file e `probes_funzioni_finali.ini` (giro di probe pronto, sola
lettura); nelle release con tag la cartella non c'è.
Prima: `qs_install.exe --check`, installare l'artifact, `controlla.bat` (pacchetti), poi gioco. Tutto si
legge in `bin64\scripts\QuattroStagioni.log`.
1. **Avvio**: `version check: ... 0x6AE46DD0` accettato; riga `startup in ... ms: ... weather on, lightning on,
   lighting on, day length on, particles on, climate on, gpu swap on`. Nessun `DISABLED` inatteso.
2. **Pacchetti**: `packs: A.qsp verified ...` per ogni stadio; nessun `refused: pack was generated for game build`.
   F5 Info: nessuno stadio mancante.
3. **GPU**: `gpu: hooked ...`, `gpu: uploader ready on the game queue ...`, `gpu: upload prints read at ExecuteCommandLists`;
   cambiando stagione da F5 (Manuale) le piante già visibili cambiano in pochi secondi, senza scatti.
   `gpu: DISABLED:` = da segnalare con la riga intera.
4. **Meteo**: `weather: climate update found ...`, `weather: hooks on; after-compose consumers ...`, ogni 60 s
   `weather: <tipo> NN% (... season <fase> ...)`. Estate limpida, autunno piovoso, inverno coperto (cielo non azzurro).
5. **Collana della pioggia**: indossata, il gioco fa piovere: nel log `game rain` sale, `weather: HUD rainy`; con
   temporale del gioco `weather: HUD storm ... Storm bit set` e `lightning: storm begins (...)`.
6. **Temperature**: `climate: PAClimateTexture vtable rva ... from RTTI`, `climate: attached map ...`,
   `climate: map ... at -24.00 C ...` in inverno; termometro circa -24 rispetto all'estate; nessun danno da freddo
   di notte nei forti; spegnendo "Temperature stagionali" le temperature tornano subito quelle del gioco.
7. **Neve**: `snow: winter SnowStart 10.0 SnowFull 0.0 ...`, `snow: attached, originals ...`; neve nelle zone
   temperate d'inverno.
8. **Deserto** (anche di notte, e la valle di Tashkalp): `weather: entering the desert (hottest day top ...)`,
   `lightning: no strikes: in the desert`; nel deserto meteo, luce e particelle del gioco, nessuna neve.
9. **Fulmini**: d'estate o d'autunno, a temporale: `lightning: storm begins (summer, chance 0.90): with lightning`,
   righe `lightning: strike #N (...)`, `lightning: storm ends`; d'inverno quasi nessun lampo. Con il campo impostato:
   lampi visibili, `writes` cresce solo durante i temporali; all'uscita `lightning: stop (N storms, N strikes, N writes)`.
10. **Luce**: `lighting: start, N field(s) ...`; senza campi `lighting: no light field ... in targets.ini [lighting]`.
    Con i campi: `lighting: state active`, ogni 60 s `lighting: stage <stadio> on | sun ... | fog ... | writes N`;
    inverno più freddo e tenue, autunno dorato, estate uguale al gioco; spegnendo F5 "Luce stagionale" torna
    gradualmente la luce del gioco. `lighting: DISABLED: ... non ricomposto dal gioco` = campo sbagliato.
11. **Durata del giorno**: `daylength: seasonal sunrise H:MM, sunset H:MM (... h of day, phase ...)`; con il campo:
    `daylength: sun table loaded/saved (N/96 bins known)`, `daylength: active; ... game hour 16:00 -> sun hour ...`;
    d'inverno il sole sorge tardi e tramonta presto, l'orologio del HUD va alla stessa velocità di prima;
    nessuno scatto dopo aver dormito. Annotare alba e tramonto visibili del gioco in estate (GameSunrise/GameSunset).
12. **Particelle**: `particles: after-compose callback on; winter flakes on, leaves off, pollen off`; d'inverno
    con nevicata `particles: winter 100%, flakes 100% (snow X -> Y)` con Y > X e neve più fitta ma non eccessiva;
    fuori inverno Y = X. `particles: leaves off: campo non ancora trovato (probe)` è atteso.
13. **Menu F5**: scheda Meteo e luce con gli interruttori Meteo, Fulmini, Temperature, Neve, Luce, Durata del giorno,
    Particelle; scheda Info con una riga di stato per ciascuno (motivo se spento). Salvataggio dell'ini dal menu.
14. **Uscita**: righe `... stop` di ogni modulo, nessun crash in chiusura.
Poi un giro di probe (`docs/dev/probe_funzioni_finali.md`) per i campi mancanti, uno alla volta.

### Pacchetti `.qsp` e 2.04.02
Ogni pacchetto è legato all'exe esatto (dimensione + timestamp) e agli hash dei pamt: il plugin rifiuta quelli
di un'altra build (`packs: ... refused: pack was generated for game build ...`). I 19 pacchetti del 30/10 sera
vanno rigenerati se sono stati creati prima che l'exe diventasse 0x6AE46DD0 (molto probabile). Controllo:
`controlla.bat` (scrive "da rigenerare"). Rigenerazione di tutti i 19 stadi con `genera_pacchetti.bat` seguito
dagli stadi (U è l'estate del gioco: niente da generare, 4 piani + 15 transizioni = 19):
`genera_pacchetti.bat S L A W "S>U.1" "S>U.2" "S>U.3" "U>L.1" "U>L.2" "U>L.3" "L>A.1" "L>A.2" "L>A.3" "A>W.1" "A>W.2" "A>W.3" "W>S.1" "W>S.2" "W>S.3"`
(elenco: `qs_gen.exe --list-stages`).
