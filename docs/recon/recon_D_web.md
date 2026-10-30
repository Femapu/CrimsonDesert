# Recon D: ricerca web (30/10/2026)

Flusso RESEARCH di QuattroStagioni. Integra `recon_A_research.md` (non ne ripete i contenuti) e serve a
decisioni di design concrete (vedi la sezione finale).

Legenda: **[C]** confermato, con link alla fonte letta; **[S]** probabile / dedotto; **[?]** non verificato o
fonti in conflitto.

Limiti di questa sessione (importante per pesare le fonti): la rete della sessione permetteva solo
`raw.githubusercontent.com` e WebFetch su `github.com`; Nexus, Reddit, Steam, Wikipedia, thegameswiki,
powerpyx, 8bittoast, DSOGaming **non erano raggiungibili** in lettura diretta. Per quei siti ho solo gli
estratti del motore di ricerca: li tratto come [S] o [?] salvo concordanza fra più fonti. Quasi tutte le
"wiki" di Crimson Desert (thegameswiki.com) sono generate con IA e si contraddicono da una revisione
all'altra: affidabilità bassa. Le fonti più solide restano i README e le note dei modder su GitHub.

---

## 1. Tempo di gioco

### Velocità dell'orologio
- Le guide concordano su **1 minuto reale = 12 minuti di gioco**, cioè 1 ora di gioco = 5 minuti reali,
  giorno completo = **circa 2 ore reali** [?]. Fonti: [wccftech](https://wccftech.com/how-to/crimson-desert-how-to-change-time-of-day/amp/),
  [thegameswiki how-to-pass-time](https://thegameswiki.com/crimson-desert/wiki/how-to-pass-time),
  [thegameswiki weather-system](https://thegameswiki.com/crimson-desert/wiki/weather-system).
  Probabilmente si copiano a vicenda; nessuna misura pubblicata.
- Contrasto: l'autore di Seasons of Pywel (misura diretta, già in recon A) indica circa 2,5 minuti reali per
  ora di gioco, cioè ~60 minuti reali per giorno [?]. Le due cifre differiscono di un fattore 2.
- CrimsonWeather espone **scala del tempo separata giorno/notte**: "Day Time Scale" dalle 03:00 alle 18:59,
  "Night Time Scale" dalle 19:00 alle 02:59, `NATIVE` = velocità normale; ini `[RealGameTime] DayScale/NightScale` [C]
  ([README CrimsonWeather](https://github.com/Nostyxx/CrimsonWeather/blob/HEAD/README.md)). Questo conferma che
  l'orologio del gioco ha un moltiplicatore scrivibile e che la "giornata" è divisa in due fasce. Possibile
  spiegazione della discrepanza 60/120 min: velocità diversa fra giorno e notte nel vanilla [S].
- **Raccomandazione**: non fidarsi di nessuna delle due cifre; il plugin deve misurare (ore di gioco per
  secondo reale, separatamente di giorno e di notte) e loggare. Vedi Implicazioni.

### Calendario e contatore dei giorni
- L'HUD mostra **"Day N", giorno della settimana e ora** (es. "Day 76 Sat 12:06 PM") [C]
  ([research-notes Seasons](https://github.com/Khione95/Seasons-of-Pywel/blob/HEAD/docs/research-notes.md)).
  Non esistono mesi o stagioni nel vanilla [S] (nessuna fonte ne parla).
- Il giorno HUD può cambiare "un attimo dopo" l'ora; durante il caricamento l'orologio va a 0.00 e poi
  all'ora del mondo; nel menu principale sta fermo alle 12.00 [C] (stesse note). Conseguenza: un salto
  dell'orologio va classificato (caricamento, sonno, attesa) prima di farlo avanzare il calendario.
- Seasons usa di default **30 giorni di gioco per stagione** (scelte 3, 7, 14, 45, 60, 90), anno di 120
  giorni che inizia in estate [C] ([README Seasons](https://github.com/Khione95/Seasons-of-Pywel/blob/HEAD/README.md)).
  Solo idea, non copiare.

### Sonno, attesa, salti di tempo
- Letti (anche locande e letti liberi nelle case), falò e pentole da cucina con "Wait": salti di **3, 6 o
  12 ore** [S] (concordano più guide: [thegameswiki how-to-sleep](https://thegameswiki.com/crimson-desert/wiki/how-to-sleep),
  [games.gg](https://games.gg/crimson-desert/guides/crimson-desert-how-to-skip-time/),
  [nerdschalk](https://nerdschalk.com/crimson-desert-how-to-wait-or-pass-time-during-missions-and-quests/)).
- Cooldown fra un riposo e l'altro (messaggio "cannot sleep right now"); una guida parla di 10 ore di gioco
  [?] ([allthings.how](https://allthings.how/crimson-desert-infinite-time-skip-trick-bypass-the-rest-cooldown/)).
- Il riposo si sblocca nel capitolo 3 (Greymane Camp, Howling Hill) secondo alcune guide [?].
- Alcune missioni a orario fanno saltare il tempo automaticamente fino all'evento [?] (wccftech).
- Non si può cambiare l'ora dal menu [S] (wccftech).
- Mod esistenti che saltano il tempo: Rest Anywhere (1-72 ore), DesertLink Time and weather control (già in
  recon A). Seasons "gioca" il tempo dormito (simula il meteo delle ore saltate) [C] (README Seasons).
- **Il plugin deve gestire salti fino a 12 ore (vanilla) e 72 ore (mod) senza saltare stadi visivi in modo
  brusco.**

### Menu, mappa, cutscene
- Nessuna fonte dice se l'orologio scorre con menu o mappa aperti [?]. Da misurare in gioco.
- Le cutscene sono in tempo reale e ereditano meteo e luce del momento [?]
  ([thegameswiki weather-system](https://thegameswiki.com/crimson-desert/wiki/weather-system)). Implica che le
  nostre texture stagionali si vedranno anche nelle cutscene (bene) [S].

### Console / comandi di debug
- **Nessuna console o menu di debug noto ai giocatori** [S] (ricerche senza risultati). Le cvar del motore
  esistono ma sono solo per hash, senza nomi [C] (research-notes Seasons: "Snow cvars are hash-only").
  Il controllo del tempo passa solo da hook (CrimsonWeather: `GameTimeGetter/Setter`, già in recon A).

---

## 2. Meteo e regioni

### Tipi di meteo osservati
- Nel motore: sequenze naturali `sunny, cloudy, rainy (+desert, jungle), heightfog, mie, wind, winddegree,
  lightning`; **nessuna sequenza di neve**; effetti globali `rain, heavyrain, snow, heavysnow, snowdust,
  lightning, desertdust`; eventi globali `Drought, HeatWave, ColdWave, HeavySnow, Prolonged_Rainy_Season,
  Storm_*` [C] (research-notes Seasons). La variante "jungle" della pioggia indica che **esiste un bioma
  giungla** [S].
- HUD: flag sunny, cloudy, rainy, snowy, windy, fog (recon A) [C].
- Giocatori/guide: tempeste di sabbia nel deserto, bufere in alta quota, nebbia e pioggia sulle coste [?]
  ([thegameswiki weather-system](https://thegameswiki.com/crimson-desert/wiki/weather-system)). Fulmini con
  danni bonus su nemici bagnati [?]. Il meteo "migliora l'immersione più che introdurre sfide" (attribuito a
  Will Powers, Pearl Abyss) [?].
- CrimsonWeather permette di forzare clear, rain, dust, snow, thunder e regola "snow accumulation
  boundaries" e "snow coverage threshold" [C] (README). Quindi **la copertura nevosa è parametrica** nel
  motore, non solo legata alla texture climatica.

### Neve
- Neve vanilla: nelle montagne di Pailune (nord, freddo, conifere, laghi ghiacciati, bufere) [S] (molte guide
  concordano: [ludo.guide](https://www.ludo.guide/guide/crimson-desert/game-mechanics-and-tips/white-mountains-frozen-soul-region-guide-survival-tips-and-h),
  [thegameswiki pailune](https://thegameswiki.com/crimson-desert/wiki/pailune-region-guide)). Una revisione
  della wiki descrive Pailune come "autunnale" con valli dai colori d'autunno [?]: Pailune potrebbe avere sia
  alture innevate sia valli temperate.
- Accumulo: esiste uno **strato di neve profonda** costruito dalla texture climatica (soglie DEEP_SNOW) con
  effetto di camminata nella neve e **impronte/scie** generate entro ±32 m dalla camera (NPC lontani non
  lasciano tracce) [C] (research-notes Seasons). La neve vanilla è quindi statica per luogo (dipende dalla
  temperatura della mappa climatica), non cresce nel tempo; l'accumulo dinamico l'ha aggiunto Seasons [S].
- La neve fa danni da freddo sotto circa -40 °C percepiti senza equipaggiamento; con l'inverno di Seasons
  1.0.1 giocatori e guardie morivano assiderati, corretto con un "pavimento" notturno di -30 °C [C]
  (research-notes Seasons). **Rischio di gameplay reale da evitare.**

### Regioni di Pywel

| Regione | Posizione (guide) | Bioma per noi | Note | Tag |
|---|---|---|---|---|
| Hernand | cuore iniziale, centro/ovest | `shared_temperate` | campi verdi, foreste, castelli, fiumi; città di Hernand | [S] |
| Demeniss | centro (alcune: costa) | `shared_temperate` | centro politico, porti, chiese, tenute | [?] |
| Delesyia | est (alcune: ovest o sud-est) | `shared_temperate` + aree industriali | altopiani, pianure industriali con smog, valli boscose (Chittering Forest centro-est, Hushwind Hills sud-ovest, Mount Benus a sud, Dewhaven) | [?] |
| Pailune | nord (Seasons: ovest della texture) | `north_cold` | Kingshield Mountains, Silver Wolf Mountain, Five-Finger Mountain, Vannstein Lake, Wayward Woods; terra dei Greymane | [S] |
| Crimson Desert | sud (Seasons: nord-est della texture) | `desert_dry` | sabbia rossa, caldo estremo, notti fredde, tempeste di sabbia; contiene la valle di **Tashkalp** che è terra "ordinaria" | [S] |
| Giungla | fra Demeniss e Delesyia (est di Bordig Ruins) e fra Delesyia e il deserto | `swamp_jungle` | confermata dalla sequenza meteo "rainy jungle"; posizione esatta incerta | [?] |
| Abyss | strato separato "in cielo" | `abyss` | escluso | [S] |
| Great Ocean of Pywel (DLC 2.04) | oceano attorno a Pywel | `aquatic` / isole da classificare | isole sparse, rovine sommerse, immersioni, navi | [S] |

Fonti tabella: [beebom](https://beebom.com/crimson-desert-map/), [crimsondesert.app](https://crimsondesert.app/en/guides/all-regions),
[consolepulse](https://consolepulse.com/multiplatform/crimson-desert/guides/crimson-desert-locations-guide),
[thegameswiki delesyia](https://thegameswiki.com/crimson-desert/wiki/delesyia-region-guide),
[PSU DLC](https://psu.com/news/crimson-desert-charting-the-unknown-dlc-trailer-explores-a-richer-life-in-pywel),
[vulkk DLC](https://vulkk.com/2026/10/21/crimson-desert-charting-the-unknown-dlc-new-features-preview/),
regioni ufficiali di preset in CrimsonWeather (Hernand, Demeniss, Delesyia, Pailune, Crimson Desert, Abyss) [C].

**Conflitto sulle posizioni**: le guide mettono Pailune a nord e il deserto a sud; le note di Seasons, che
hanno sovrapposto la texture climatica (nord in alto) alla mappa del gioco, trovano le montagne innevate di
Pailune a **ovest** e il Crimson Desert a **nord-est** (texel Tashkalp ~(196,229); area giocabile x 64-268,
y 178-383 su 512) [C per la misura dell'autore, ? per l'interpretazione]. La misura diretta pesa più delle
guide. **Non serve una tabella con coordinate scritte a mano**: la classificazione deve venire dalle texture
del gioco (vedi Implicazioni).

Indicatori nei dati del gioco (da note Seasons, [C]): canale B della climate texture ~0 nel deserto rosso,
176-192 terra ordinaria, 240+ zona chiara centrale, ~25 montagne innevate; R ~60 nel nord nevoso, 85-106 nel
deserto, ~3 altrove; `global_regionmap` NON contiene indici di regione utilizzabili direttamente.

### Mappa del DLC "Charting the Unknown"
- Nuova regione "Great Ocean of Pywel", isole, immersioni subacquee, navi, casa [S] (PSU, vulkk, playnews).
  Nessun nome di bioma ufficiale per le isole [?].
- 2.04 ha cambiato il layout di `meta/0.papgt` ("4-byte header, 64 unexplained trailing bytes, no public
  tool parses it yet"), build Steam **26308950** [C]
  ([Eidos PR #109, 29/10/2026](https://github.com/Project-Colony/Eidos/pull/109)). Rilevante per chiunque
  legga o patchi la lista gruppi (Seasons lo fa; noi per design no).

---

## 3. Mod esistenti rilevanti

| Mod | Cosa fa | Come (se descritto) | Licenza | Stato 2.04 / lamentele |
|---|---|---|---|---|
| **Seasons of Pywel** (Khione95, Nexus 3630, v1.0.4) | 4 stagioni live, neve profonda, temperatura, meteo | vedi recon A. Default 30 giorni/stagione; offset temperatura estate +4, primavera -6, autunno -8, inverno -24 (più neve profonda); deserto escluso; meteo "a periodi" tirati sull'orologio; sonno simulato; F5 Direct2D [C] (README) | **nessuna** (solo idee) | Nessun update per 2.04 [C, recon A]. Guida giapponese: due loader ASI insieme crashano al caricamento, Windows Defender mette in quarantena l'`.asi`, segnalazioni di crash con OptiScaler [S] ([gaming-st](https://gaming-st.com/pc-games/crimson-desert-seasons-of-pywel-mod/)). Bug storico: assideramento di giocatori e guardie (1.0.1) [C]. Peso ~1,2 GB. |
| **Crimson Weather** (Nostyxx, Nexus 632) | meteo, nuvole, nebbia, atmosfera, luna/via lattea, tempo reale, scala giorno/notte, preset per regione, programma orario con blend, preset comunitari online | Ramo main = **addon ReShade** (`.addon64`, overlay nel pannello ReShade, niente ASI loader); ramo `legacy` = ASI con **ImGui su DX12** (F9), input XInput/GameInput/DualSense [C] ([README main](https://github.com/Nostyxx/CrimsonWeather/blob/HEAD/README.md), [README legacy](https://github.com/Nostyxx/CrimsonWeather/blob/legacy/README.md)). Hook AOB (recon A). | **nessuna** (solo idee) | Ultimo update noto 2.03.02 (recon A); 2.04 non verificato [?]. Il passaggio da overlay ImGui proprio a addon ReShade suggerisce che l'overlay DX12 proprio dava problemi (compatibilità con ReShade/FG/HDR) [S]. |
| Time and weather control / All-in-One (DesertLink, Nexus 3557/3638) | ora, salto, blocco orologio, meteo incl. neve | closed source | ? | ? |
| Rest Anywhere (Fyreon87, Nexus 3904) | salta 1-72 ore | ASI | ? | aggiornata il 29/10 (recon A) |
| Dynamic Body Weather (Nexus 3659) | effetti sul corpo da neve/temperatura | ASI + ReShade | ? | ? |
| RenoDX Crimson Desert | tone mapping HDR/SDR, grading | addon ReShade (team Forge, Fyren, Jon, Shortfuse) [S] ([wiki RenoDX](https://github.com/clshortfuse/renodx/wiki/mods), [discussione #535](https://github.com/clshortfuse/renodx/discussions/535)) | MIT (progetto RenoDX) [?] | build aggiornate sul Discord RenoDX [S] |
| Preset ReShade (es. Nexus 256 "MartinSpielt", "+RenoDX") | color correction | ReShade **con supporto addon** [S] ([Nexus 256](https://www.nexusmods.com/crimsondesert/mods/256)) | ? | I preset ReShade di solito sopravvivono alle patch [S] |
| UI Passthrough ReShade Addon (Nexus 487) | applica ReShade senza toccare la UI | addon ReShade | ? | rotto dopo una patch; fix aneddotico "Hunting Code (collect shaders)" [?] ([post](https://www.nexusmods.com/crimsondesert/mods/487?tab=posts)) |
| Texture mod (Better Trees di KhainOne, pack pietra Hernand di nopoPak, overhaul "Folder 0000", Grass and LOD) | upscale texture (anche con IA), meno pop-in, ombre erba | "CD Texture Overlay Builder" (gruppo overlay) [S] ([DSOGaming](https://www.dsogaming.com/mods/new-hd-texture-pack-for-crimson-desert-overhauls-all-trees/)) | ? | **Conflitto potenziale**: se un utente sostituisce le texture degli alberi, le nostre ricette (basate sui byte vanilla) non corrispondono [S] |
| Nessuna mod "autunno" o "neve" statica trovata oltre Seasons [S] | | | | |
| **CDUMM** (faisalkindi) | mod manager, overlay, PATHC, ASI loader incluso | Python | **MIT** [C] ([LICENSE](https://github.com/faisalkindi/CrimsonDesert-UltimateModsManager/blob/HEAD/LICENSE)) | ultima release 3.17.13 del 28/10, prima della 2.04 [C] ([CHANGELOG](https://github.com/faisalkindi/CrimsonDesert-UltimateModsManager/blob/HEAD/CHANGELOG.md)). Nota 3.17.9: "un plugin ASI è legato a una build e rifiuta le altre"; build Steam a ottobre = FileVersion **1.0.0.2976** [C] |
| **Definitive Mod Manager (DMM)** (Nexus 633) | mod manager con loader ASI proprio | avvisa quando Steam scarica un update con mod montate, pulsante "Revert to vanilla" [S] ([Nexus 633](https://www.nexusmods.com/crimsondesert/mods/633)) | parser CDMTL restrittivo (recon A) | ri-codifica texture a mip singola (recon A) |
| **CrimsonForge** (hzeemr) | repack completo | Python | **MIT** [C] ([LICENSE](https://github.com/hzeemr/crimsonforge/blob/HEAD/LICENSE)) | ultimo commit maggio (recon A) |
| **DetourModKit** (tkhquang) | toolkit C++23: scansione memoria, hook funzioni/vtable, input, config, ciclo di vita DLL | MSVC 2022+ / MinGW 14+ [C] ([README](https://github.com/tkhquang/DetourModKit/blob/HEAD/README.md)) | **MIT** [C] | base riusabile per hook AOB |
| **pycrimson** (LukeFZ) | lettori formati | Python | file `LICENSE.txt` presente (MIT per recon A) [C] | |
| OptiScaler | upscaler/FG alternativi | `dxgi.dll` o `OptiScaler.asi`; con ReShade: ReShade rinominato `ReShade.asi` + `version.dll` di Ultimate ASI Loader [C] ([wiki OptiScaler](https://github.com/optiscaler/OptiScaler/wiki/Crimson-Desert)) | GPL [?] | conflitti di nome DLL con altri loader (recon A) |

---

## 4. Compatibilità tecnica

- **Loader ASI**: funzionano molti mod via Ultimate ASI Loader (`winmm.dll`, alternative `version.dll`,
  `dinput8.dll`, `dsound`); CDUMM e DMM includono un loader [C] (recon A, [Eidos PR](https://github.com/Project-Colony/Eidos/pull/109)
  elenca anche `version` e `dsound` come nomi proxy). Ultimate ASI Loader è **MIT** [C]
  ([license](https://github.com/ThirteenAG/Ultimate-ASI-Loader/blob/master/license)). **Due loader insieme
  causano crash al caricamento** [S] (gaming-st). Windows Defender può mettere in quarantena gli `.asi` [S].
- **Denuvo**: ancora presente; patch del gioco spostano tutti gli indirizzi (recon A). Nessun report trovato di
  crash causati dall'iniezione DLL in sé [S]. Crack con hypervisor e "voices38" non ufficiali, irrilevanti per
  noi [?] ([gamegpu](https://en.gamegpu.com/news/igry/poyavilsya-nestabilnyj-vzlom-denuvo-dlya-crimson-desert-cherez-hypervisor)).
  Nessun anti-cheat (recon A).
- **Integrità file**: le mod overlay e CDUMM convivono con il gioco; DMM avvisa degli update Steam. Il gioco
  non sembra verificare l'integrità dei `.paz` all'avvio oltre a checksum interni di PAMT/PAPGT [S]. Il
  nostro design (nessun file scritto) evita il problema.
- **ReShade**: largamente usato (preset, RenoDX, CrimsonWeather, UI Passthrough); serve la build **con
  supporto addon** per gli addon [S]. Nessun report di incompatibilità ReShade specifica di Crimson Desert
  [S] ([ricerca](https://steamcommunity.com/app/3321460/discussions/0/832745759814323449/)). Su Linux/Proton
  ReShade addon + DLSS crasha in DX12 (bug generico ReShade, corretto e poi tornato in 6.7.3) [?]
  ([forum reshade](https://www.reshade.me/forum/troubleshooting/10203-solved-games-crash-when-reshade-addon-and-dlss-are-enabled-under-proton-linux)).
- **Frame generation**: il gioco ha DLSS (via **Streamline**, DLSSG), XeSS e XeFG nativi, FSR, Anti-Lag 2,
  Ray Reconstruction; 2.04 rende upscaling e FG selezionabili separatamente (recon A). **DLSSG via Streamline
  non fornisce una risorsa "HUDless"** e causa ghosting dell'HUD [C] ([wiki OptiScaler](https://github.com/optiscaler/OptiScaler/wiki/Crimson-Desert)).
  Implicazione: un overlay disegnato prima della generazione dei frame verrà interpolato (ghosting del menu
  F5); conviene disegnare sulla swapchain finale (dopo Streamline) o mettere in pausa l'FG col menu aperto [S].
  Una patch di settembre 2026 ha corretto crash all'avvio di una nuova partita con DLSS FG attivo [S]
  ([gamingbolt](https://gamingbolt.com/crimson-desert-patch-fixes-dlss-frame-generation-crashes-on-start-up-and-more)).
- **HDR**: Seasons disegna il menu "HDR aware" e "ReShade aware" [C]; patch 1.12.02 correggeva un crash
  Photo Mode + HDR [S] ([DSOGaming](https://www.dsogaming.com/patches/crimson-desert-patch-1-12-02-released-detailed/)).
  Il nostro overlay deve gestire swapchain HDR10/scRGB.
- **Steam overlay**: nessuna informazione specifica trovata [?]. Il consiglio generico è chiudere overlay
  extra (NVIDIA App ecc.) se ReShade DX12 dà problemi [S].
- **Versioni**: build Steam 26308950 = 2.04 [C, Eidos]; FileVersion 1.0.0.2976 a ottobre (pre-2.04) [C,
  CDUMM]. Ogni plugin che rifiuta build sconosciute è la prassi della comunità [C, CDUMM 3.17.9].

---

## 5. Riferimenti visivi per le ricette (colori reali)

Principi fisiologici [C] ([USDA Forest Service](https://www.fs.usda.gov/visit/fall-colors/science-of-fall-colors),
[Iowa State](https://yardandgarden.extension.iastate.edu/article/1997/9-12-1997/leafcol.html)): i
**carotenoidi** (gialli/arancio) sono presenti tutto l'anno e emergono quando la clorofilla cala; gli
**antociani** (rossi/porpora) si formano a fine estate con zuccheri, giornate soleggiate e notti fresche; i
**tannini** danno i bruni (querce, faggio). Tempo nuvoloso e piovoso = meno rossi; alberi in ombra = colori
meno vivi. Quindi: un autunno piovoso nel nostro motore meteo dovrebbe spostare la tavolozza verso giallo e
bruno [S].

Tempi [C] ([Climate Research 2006](https://int-res.com/abstracts/cr/v32/n3/p253-267),
[UAntwerp 2022](https://www.uantwerpen.be/en/research-groups/global-change-ecology/news/2022/leaf-senescence-climate-change/),
[EGU 2017 Svezia](https://opac.geologie.ac.at/ais312/dokumente/EGU/EGU2017-18275.pdf)): l'inizio della
colorazione è stabile; il riscaldamento allunga la durata fra inizio e caduta; un inizio tardivo accorcia
la colorazione (circa 5 giorni persi per giorno di ritardo, Svezia); settembre caldo ritarda, maggio-giugno
caldi anticipano. Ordine tipico in Europa centrale [S, conoscenza generale]: betulla e castagno (fine
settembre) prima, poi acero e faggio (metà ottobre), poi querce e larice (fine ottobre-novembre); il salice
resta verde-giallo fino a tardi; il ginkgo vira in pochi giorni e perde le foglie quasi tutte insieme.
Durata indicativa dal primo viraggio alla caduta: 3-5 settimane per individuo, 4-8 per un bosco misto [S].
Proporzione per il nostro calendario (A = 8 giorni): viraggio giorni 1-4, picco 4-6, caduta 5-8; le querce
(e il faggio giovane) possono **trattenere foglie secche brune** in inverno (marcescenza) [S].

Tabella colori obiettivo (valori indicativi sRGB per albedo di foglia illuminata, OKLCh calcolato) [S, ricavati
da descrizioni USDA/Iowa State e fotografie tipiche; da tarare in gioco]:

| Specie | Colore d'autunno | Hex (range) | OKLCh indicativo |
|---|---|---|---|
| Acero (Acer saccharum / rubrum) | arancio-rosso, varia per individuo da giallo a scarlatto | `#F2B134` -> `#E07B20` -> `#B5301E` | L0.80 C0.15 h79 -> L0.68 C0.16 h56 -> L0.51 C0.17 h31 |
| Quercia (rossa) | rosso vino, poi ruggine e bruno | `#8E2A22`, `#A0522D`, `#8B5A2B` | L0.44 C0.14 h29; L0.53 C0.12 h45; L0.51 C0.09 h62 |
| Quercia (bianca/europea) | bruno-ruggine, giallo-bruno | `#9E6B3A`, `#C98B2B` | L0.57 C0.09 h63; L0.68 C0.13 h73 |
| Faggio | giallo-ocra poi bronzo/tan chiaro (tannini) | `#D4A017` -> `#C98B2B` -> `#C9B07A` | L0.73 C0.15 h84 -> L0.68 C0.13 h73 -> L0.77 C0.08 h86 |
| Betulla | giallo caldo luminoso | `#F4D03F`, `#E8C547` | L0.86 C0.16 h94; L0.83 C0.15 h93 |
| Castagno | giallo-bruno, poi bruno | `#E3B23C`, `#9E6B3A` | L0.79 C0.14 h85; L0.57 C0.09 h63 |
| Salice | giallo-verde pallido, tardivo | `#A8C94A` -> `#D8C27A` | L0.79 C0.16 h122 -> L0.82 C0.10 h93 |
| Hickory (Carya) | giallo dorato | `#E3B23C`, `#D4A017` | L0.79 C0.14 h85; L0.73 C0.15 h84 |
| Ginkgo | giallo limone/burro uniforme | `#F4D03F` | L0.86 C0.16 h94 |
| Larice (conifera decidua) | giallo-oro poi arancio-bruno, poi spoglio | `#E3B23C` -> `#C98B2B` | L0.79 C0.14 h85 -> L0.68 C0.13 h73 |
| Conifere sempreverdi (inverno) | più scure e fredde | da `#3B4A2F` verso `#2F3D2E` | L0.39 C0.05 h132 -> L0.34 C0.03 h143 |

Prati e suolo [S]:

| Look | Hex | OKLCh | Nota |
|---|---|---|---|
| S primavera, erba giovane | `#8DB33A`, `#6B8E23` | L0.72 C0.15 h125; L0.60 C0.14 h126 | più saturo e più giallo del vanilla estivo (h ~125-130) |
| U estate (riferimento) | `#4F7A2A` | L0.53 C0.12 h133 | solo riferimento: U = vanilla |
| L fine estate, erba secca/paglia | `#E2CF8C`, `#D8C27A`, `#B8A26A` | L0.85 C0.09 h94; L0.82 C0.10 h93; L0.72 C0.08 h89 | paglierino; mescolare con chiazze verde oliva `#7D7A4A` (L0.57 C0.07 h105) per non uniformare |
| A autunno, prato bruno oliva | `#7D7A4A`, `#9C8B5A` | L0.57 C0.07 h105; L0.64 C0.07 h91 | |
| Foglie a terra | `#A0522D`, `#C98B2B`, `#8B5A2B` | vedi sopra | lettiera: perde croma in 1-2 settimane verso `#6B4E2E` (L0.45 C0.06 h68) |
| W erba morta | `#5C5A3C`, `#4A4636` | L0.46 C0.05 h105; L0.39 C0.03 h96 | |
| W brina | `#D9E2E8`, `#BFCAD3` | L0.91 C0.013 h237; L0.83 C0.017 h242 | brina = alza L e porta la tinta verso blu freddo (h ~235-245) con croma quasi nulla; si applica a erba e bordi, non come pittura piatta |
| W neve fresca (luce diretta) | `#EEF3F6` | L0.96 C0.007 h234 | in ombra blu: L ~0.80, h ~250 [S] |

Regole pratiche per la ricetta [S]: lavorare in OKLCh ruotando la tinta (h 125-135 verde -> 95 giallo ->
60-75 arancio -> 30-45 rosso) e regolando croma e luminosità separatamente; preservare la variazione di
luminosità della texture originale (dettaglio) e cambiare soprattutto h e C; per la variabilità per asset
usare il seme per scegliere fra le famiglie "giallo / arancio / rosso / ruggine" con pesi per specie (es.
betulla 90% giallo, acero 40% arancio 40% rosso, quercia 60% ruggine).

---

## 6. Licenze librerie (lette alla fonte)

| Libreria | Licenza | Fonte |
|---|---|---|
| Dear ImGui | **MIT** (c) 2014-2026 Omar Cornut [C] | [LICENSE.txt](https://github.com/ocornut/imgui/blob/master/LICENSE.txt) |
| rgbcx.h (v1.13) e bc7enc_rdo | **MIT oppure Public Domain (Unlicense)**, a scelta; attribuzione richiesta ma non obbligatoria. Eccezioni nel repo: `bc7e.ispc` è **Apache 2.0** (Binomial), LodePNG ha licenza propria (zlib) [C] | [LICENSE](https://github.com/richgel999/bc7enc_rdo/blob/master/LICENSE), header `rgbcx.h` |
| stb_image | **MIT oppure Public Domain**, a scelta [C] | [LICENSE](https://github.com/nothings/stb/blob/master/LICENSE) |
| MinHook | **BSD 2-clause** (c) 2009-2017 Tsuda Kageyu [C]; contiene HDE (BSD) | [LICENSE.txt](https://github.com/TsudaKageyu/minhook/blob/master/LICENSE.txt) |
| LZ4 (libreria `lib/`) | **BSD 2-clause** (c) Yann Collet [C]. Attenzione: CLI e programmi di test nel repo sono GPLv2, non usarli | [lib/LICENSE](https://github.com/lz4/lz4/blob/dev/lib/LICENSE) |
| picojson | **BSD 2-clause** (Cybozu Labs, Kazuho Oku) [C] | [LICENSE](https://github.com/kazuho/picojson/blob/master/LICENSE) |
| Ultimate ASI Loader | **MIT** (ThirteenAG) [C] | [license](https://github.com/ThirteenAG/Ultimate-ASI-Loader/blob/master/license) |
| ReShade | BSD 3-clause (Patrick Mours) [C] | [LICENSE.md](https://github.com/crosire/reshade/blob/main/LICENSE.md) |
| DetourModKit, CDUMM, CrimsonForge | **MIT** [C] | link in sezione 3 |

Tutte compatibili con un repo pubblico; serve un file `THIRD_PARTY.txt` con i testi di MIT/BSD (obbligo di
riportare copyright e licenza nelle distribuzioni binarie per BSD e MIT). Se usiamo bc7enc_rdo, evitare
`bc7e.ispc` o aggiungere il testo Apache 2.0.

---

## Implicazioni per il design

1. **Calendario misurato, non presunto.** Le fonti danno 1 giorno = 2 ore reali (guide) oppure ~1 ora
   (misura di Seasons); CrimsonWeather mostra scale giorno/notte distinte. Il plugin deve: leggere il giorno
   HUD + ora; misurare la velocità dell'orologio nelle due fasce e loggarla; usare il giorno HUD come fonte
   primaria (A1-b). Con i default del DESIGN (anno 38 giorni) e 1-2 ore per giorno, un anno dura 38-76 ore
   reali: ragionevole. Offrire preset "anno breve" (~20 giorni) e "anno lungo" (~120 giorni, come Seasons).
2. **Classificare i salti dell'orologio**: caricamento (orologio a 0.00 poi salto), sonno/attesa (+3/6/12 h,
   fino a 72 h con mod), cambio manuale (mod tempo). Avanzare il calendario solo per sonno/attesa reale;
   attraversare gli stadi intermedi in modo coerente; mai regredire su caricamento di salvataggi vecchi senza
   ricalcolo dal giorno HUD.
3. **Stato per salvataggio**: il calendario deve derivare dal giorno HUD (deterministico) così non serve
   salvare nulla nei save; l'unico stato non derivabile è la neve accumulata: tenerla in un file per
   profilo, chiave giorno*24+ora (idea già usata da Seasons, da reimplementare in modo nostro).
4. **Regioni dai dati, non da coordinate a mano.** Costruire nel generatore una maschera di bioma a 512x512
   dalla climate texture (B ~0 deserto, ~25 montagna innevata, 176-192 terra ordinaria) + `global_colormap`
   (rosso = deserto) + rilevamento giungla (da trovare: candidati tile di terreno e regione della sequenza
   "rainy jungle"). Riempire i buchi (Tashkalp è terra ordinaria dentro il deserto). Le posizioni delle guide
   sono inaffidabili e in conflitto con la misura su texture.
5. **Pailune = `north_cold` con eccezione**: le alture restano innevate tutto l'anno (vanilla), le valli
   seguono un ciclo accorciato. Non abbassare mai la temperatura dei luoghi già freddi: rispettare un
   pavimento notturno (circa -30 °C) per evitare assideramento di giocatore e NPC (bug reale di Seasons 1.0.1).
6. **DLC 2.04**: trattare l'oceano e le isole come `aquatic` + `shared_temperate` solo dopo averle
   classificate; finché il layout `0.papgt` 2.04 non è capito, **non leggere né patchare la lista gruppi**
   (coerente con il vincolo "dimensione esatta" e indici intatti del DESIGN). Il generatore deve leggere i
   dati DLC (`dlc/dlc_01`) prima di poter colorare le isole: fase successiva.
7. **Neve**: la neve vanilla dipende dalla mappa climatica e dalle soglie (cvar), e il motore ha già scie e
   impronte (±32 m dalla camera) e parametri di copertura (visti in CrimsonWeather). Puntare a pilotare
   copertura e soglie esistenti invece di disegnare neve nelle texture; nelle texture solo brina e
   desaturazione.
8. **Overlay F5 e frame generation**: con DLSSG via Streamline non c'è risorsa HUDless, quindi un overlay
   disegnato nel posto sbagliato fa ghosting. Agganciare il Present della swapchain reale (dopo Streamline),
   supportare HDR10/scRGB, e prevedere una modalità alternativa "addon ReShade" (CrimsonWeather è migrato lì,
   probabilmente per stabilità). Testare le combinazioni: vanilla, ReShade addon, RenoDX, OptiScaler,
   DLSS FG, XeFG.
9. **Loader**: documentare un solo loader ASI (`winmm.dll` di Ultimate ASI Loader, MIT) e rilevare all'avvio
   altri loader noti (`version.dll`, `dinput8.dll`, `dsound.dll`, `dxgi.dll` di OptiScaler, loader di DMM e
   CDUMM) scrivendo un avviso nel log: due loader insieme crashano. Avvisare nel README di Windows Defender.
10. **Controllo build**: rifiutare (pass-through) build sconosciute, identificando la build con FileVersion e
    dimensione/hash dell'EXE (2.04 = Steam build 26308950; 2.03.x = FileVersion 1.0.0.2976). Hook per firma
    AOB (DetourModKit, MIT) per sopravvivere alle patch.
11. **Compatibilità con altri mod**: rilevare Seasons of Pywel (`bin64\Seasons.asi`) e disattivarsi o
    avvisare (entrambi scrivono climate e meteo); con Crimson Weather lasciare all'utente la scelta di chi
    comanda il meteo (opzione "solo texture"). I pack di texture HD degli alberi (Better Trees) cambiano i
    byte sorgente: il generatore deve lavorare sui file effettivamente letti e verificare l'hash per asset.
12. **Ricette colore**: usare la tabella della sezione 5 come obiettivi OKLCh; ruotare h e C preservando L
    relativo; pesi per specie per le famiglie autunnali; autunno piovoso = meno rosso; querce con foglie
    brune trattenute in inverno; caduta foglie negli ultimi 3-4 giorni della fase A; brina come
    schiarimento freddo (h ~240, C ~0.015) su erba morta.
13. **Licenze**: tutte le librerie previste sono permissive; creare `THIRD_PARTY.txt` (ImGui MIT, rgbcx
    MIT/PD, stb MIT/PD, MinHook BSD-2, LZ4 lib BSD-2, picojson BSD-2, eventualmente DetourModKit MIT). Non
    usare `bc7e.ispc` senza Apache 2.0, né la CLI GPL di LZ4.
14. **Da verificare in gioco (nessuna fonte web)**: l'orologio scorre con menu/mappa aperti? velocità
    reale giorno e notte; dove si trova la giungla; bioma delle isole DLC; comportamento con Steam overlay.

Fonti non raggiungibili in questa sessione (da consultare a mano se servono conferme): pagine Nexus
(3630, 632, 3557, 3904, 633, 256), thegameswiki, powerpyx, 8bittoast, DSOGaming, Reddit, Steam community.
