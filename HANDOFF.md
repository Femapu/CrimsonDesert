# Handoff QuattroStagioni (aggiornato 30 ottobre 2026, fine sessione cloud)

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
