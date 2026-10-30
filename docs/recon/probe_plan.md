# Piano di probe runtime (sessione sul PC di Stefano)

Scopo: trovare in gioco, senza ricompilare, i sistemi che servono a DESIGN.md sezioni 5-7 (orologio,
velocità del tempo, meteo, neve, mappa clima, luce, particelle) e decidere l'hook definitivo per
ciascuno. Strumento: il framework di probe in `src/win/probe/` guidato da
`bin64\scripts\QuattroStagioni\probes.ini` (esempio commentato in
`dist/bin64/scripts/QuattroStagioni/probes.ini`), output in `bin64\scripts\QuattroStagioni\probe.log`.

Riferimenti: `docs/recon/recon_C_report.md` (RTTI, vtable, stub Denuvo, handler debug del tempo),
`data/partial/signatures_2040_hotfix.json` (firme parziali per l'exe hotfix 30/10, da verificare),
`docs/recon/recon_D_web.md` (osservazioni da CrimsonWeather: solo idee, nessun codice).

## 0. Preparazione

1. Integrazione: `main.cpp` chiama `qs::probeStart()` a fine `initialize()` (anche se il controllo di
   versione fallisce: le probe in sola lettura aiutano proprio con un exe nuovo) e `qs::probeStop()`
   nel ramo FreeLibrary di `DLL_PROCESS_DETACH`, prima di `qs::io::uninstall()`.
2. Copiare `probes.ini` in `bin64\scripts\QuattroStagioni\`, mettere `Enabled=1`, lasciare
   `AllowHooks=0` per il primo giro.
3. Avviare, entrare in partita, restare almeno 2 minuti, uscire. Leggere `probe.log`.

Il ciclo di lavoro è sempre: modificare l'ini, rilanciare, leggere il log. Una probe con errori di
configurazione viene saltata con una riga `config error:` e le altre continuano.

## 1. Primo giro: inventario in sola lettura

- La riga `module ... timestamp` conferma l'exe (hotfix 30/10: 0x6AE387F2, 336.264.088 byte).
- Le righe `section` mostrano la classificazione: deve risultare una sola sezione `code` a RVA
  0x1000 (nome casuale, nell'hotfix `.impdata`) e la sezione Denuvo come `OBFUSCATED`. Se la
  classificazione è diversa, fermarsi: nessuna probe con hook va abilitata.
- `RTTI index: N classes` deve essere intorno a 16.000. Le probe `list_*` elencano classi, vtable e
  numero di slot: salvare l'elenco (nomi e RVA, niente byte) in `data/` se utile.
- `clock_a` / `clock_b`: se la firma `GameTimeGlobals_ref` trova una sola corrispondenza, il log
  mostra il GameTime pubblicato (32 byte). Controllare che `hour` e `minute` seguano l'ora del HUD.
- `cvar_snow_*`: i valori devono essere -5 e -20 (gradi). Conferma che le firme delle cvar valgono.

## 2. Sistema per sistema

### 2.1 Orologio e contatore dei giorni

Candidati: GameTime globale (firma `GameTimeGlobals_ref`, copie A e B), `GameTime_Get`
(RVA hotfix 0x22A1DA0), `GameTime_Publish` (0xB3EFB0), `SetTimeOfDay` (0xB3EE60).

- Cosa loggare: `clock_b` con `Watch` su day, hour, minute, rate e `Dump=0:0x20`.
- Come riconoscerlo: `hour` 0..23 e `minute` 0..59 coincidono col HUD; `day` cresce di 1 a
  mezzanotte. Il HUD mostra "Day N" e il giorno della settimana: `+0x14` (weekday) deve valere
  day % 7 e corrispondere al nome mostrato.
- Verifiche richieste: dormire in un letto o accamparsi (salti di 3/6/12 ore) e controllare che
  hour salti e day avanzi; caricare un salvataggio (secondo recon D l'orologio riparte da 0.00 al
  caricamento: verificare se riparte anche `day` o solo l'ora mostrata).
- Hook atteso: lettura periodica del GameTime globale (nessun hook) per il calendario; DESIGN.md
  usa `day` come sorgente `DaySource::GameClock`. Persistenza del giorno stagionale nostra (file
  accanto al salvataggio) se `day` riparte al caricamento.

### 2.2 Velocità del tempo, giorno e notte

Fatti noti: `GameTime_ToLinear` (0x228EFD0) codifica la notte (21:00-03:00) a velocità doppia;
`rate` a `+0x16` del GameTime (atteso 24, da verificare). Recon D: CrimsonWeather regola due
moltiplicatori separati, GIORNO (03:00-18:59) e NOTTE (19:00-02:59), quindi esistono campi di
velocità scrivibili raggiungibili da un ASI.

- Cosa loggare: `rate` in `clock_b`; poi gli handler debug del tempo (recon C:
  0x3982EE0 -0.2, 0x3982F10 +0.2, 0x3982F40 -2.0, 0x3982F70 x2, RVA del build di lancio da
  ritrovare nell'hotfix). Ogni handler fa `obj = ctx->[+0x30]->[+0x60]; obj->vtbl[+0x108](delta*dt)`:
  lo slot 33 di quell'oggetto è "avanza ora del giorno".
- Procedura: (a) capire la classe dell'oggetto: con `AllowHooks=1` una probe
  `Hook=minhook` su `GameTime_Publish` con `CaptureArg=rcx` registra il time manager e i chiamanti;
  la riga `instance #0 ... class X` dà il nome RTTI. (b) Con quella classe: `LogVtable=1` e
  `Instance=capture`, `Hook=vtable`, `Slot=33`; i chiamanti loggati mostrano chi fa avanzare il
  tempo ogni frame. (c) `Dump=0:0x200 Stats=1` sull'oggetto catturato: cercare float costanti
  vicini a 1 o a 24 (moltiplicatori) e una dword che cambia solo all'alba e al tramonto.
- Come riconoscerlo: cambiando il valore candidato (solo in una fase successiva, con scrittura
  esplicita) la velocità del HUD cambia; in sola lettura, il rapporto tra minuti di gioco e secondi
  reali misurato nel log (`stamp` a +0x18) deve coincidere col moltiplicatore.
- Verifica richiesta da recon D: l'orologio avanza o no con menu, mappa o inventario aperti?
  Aprire la mappa per 60 secondi reali e guardare `minute` nel log.
- Hook atteso (DESIGN.md 6): giornate più corte d'inverno variando i due moltiplicatori giorno e
  notte (o lo slot 33 come fallback) dal tick stagionale.

### 2.3 Weather manager e stato del meteo

Candidati: `WeatherManager_Update` (0x40B71E0, un solo chiamante, `this` = PAWeatherManager a
`[world+0xF70]`), `WeatherManager_Compose` (0x40B2E40), `WeatherTick` (0xE611E0, stati a +0xFC/+0x100),
classi `pa::ServerWeatherManager`, `pa::AICondition_CheckWeather`, enum flag `WeatherState`
(Rainy=1, Snowy=2, Windy=4, Cloudy=0x10, Sunny=0x20 secondo recon B, da riconfermare).

- Cosa loggare: probe `weather_manager` (capture via MinHook, sezione di codice, una sola
  corrispondenza) con i campi noti: +0x31 manual, +0x34 climateTemp, +0x44 currentTemp, +0x48
  humidity, +0x4C wind; `Dump=0x30:0x40 Stats=1`. In parallelo `server_weather` (scan).
- Come riconoscerlo: con pioggia in corso il WeatherConstant composto (`[this+0x60]->+0x20`,
  base = puntatore - 0x28) ha `_rain` (+0x198) > 0; con cielo limpido 0. Il puntatore al
  WeatherConstant cambia nel tempo: per osservarlo conviene una probe capture su
  `Weather_GetRainIntensity` (0x40B6380), che riceve lo stesso `this`, con `Dump` sui campi del manager.
- Hook atteso (DESIGN.md 5): detour post-chiamata su `WeatherManager_Update` che applica i pesi
  stagionali (meteo manuale tramite +0x31 o ritocco dei layer); alternativa più economica: detour su
  `Weather_GetRainIntensity` / `Weather_GetSnowIntensity`.

### 2.4 Neve: quantità e copertura

Candidati: WeatherConstant `_snowAmount` (+0x170), `_snowRate` (+0x174), `_snow` (+0x194),
`_iceRatio` (+0x178); cvar SnowStart/SnowFull/DeepSnow/DeepSnowFull (valore a +0x38);
`Weather_GetSnowIntensity` (0x40B6430); `pa::scriptComponent::WeatherSurfaceSimulatorScriptComponent`
(accumulo su superfici); `ConditionData_IsCoveredBySnow`. Recon D: CrimsonWeather espone copertura e
profondità della neve, quindi i parametri sono scrivibili.

- Cosa loggare: `Dump=0:0x200 Stats=1 StatsRanges=0:1` sul simulatore di superficie (scan) e sul
  WeatherConstant; andare in una zona innevata del nord e poi in pianura.
- Come riconoscerlo: un float 0..1 che sale durante la nevicata e scende lentamente dopo
  (accumulo), distinto da uno che segue subito l'intensità (nevicata istantanea).
- Hook atteso: scrittura del valore di copertura dal nostro modello (neve accumulata 0..1 di
  DESIGN.md 5) dopo `WeatherManager_Update`; soglie cvar spostate per stagione.

### 2.5 Mappa clima (`texture/climate_texture_1.dds`)

Candidati: `pa::PAClimateTexture` (slot 0 è uno stub Denuvo: non agganciarlo nel corpo),
campi +0x18 texel (512x512, byte A,R,G,B), +0x30 metri per texel, +0x38/+0x3C dimensioni; costruita in
0x29F24D0, 0x40AE360 (weather manager), 0x40B7A50.

- Cosa loggare: `climate_texture` (scan) con `Watch` sui campi; il puntatore `texels` indica la
  copia in RAM usata dalla CPU (`ClimateTextureCPURead`).
- Come riconoscerlo: w = h = 512, mPerTexel = 80.
- Hook atteso: la via dati resta principale (texture stagionale generata e servita da io_hooks); in
  più, se la copia CPU viene letta a ogni campionamento, si può scrivere l'offset di temperatura
  direttamente nei texel al cambio stagione (verificare prima che la GPU non usi una copia separata).

### 2.6 Luce, sole, atmosfera

Candidati nel WeatherConstant: `_sunLightIntensity` (+0x40), `_moonLightIntensity` (+0x48),
`_rayleighScatteringColor` (+0x58), `_sunDirX/Y` (+0x8C/+0x90), `_earthAxisTilt` (+0xA0),
`_latitude` (+0xA4).

- Cosa loggare: dump del WeatherConstant composto all'alba, a mezzogiorno e al tramonto.
- Come riconoscerlo: `_sunDirY` segue l'altezza del sole; `_latitude` e `_earthAxisTilt` sono
  costanti (sono le leve per DESIGN.md 6: sole più basso d'inverno).
- Hook atteso: dopo `WeatherManager_Update`, ritocco di latitudine o inclinazione e della tinta.

### 2.7 Particelle

Candidati: `WeatherEffectScriptComponent`, `worldWeatherEffectScriptComponent` (slot 0 e 9 stub),
stringhe `fx_weather_a__snow.system.effect`, `__snowdust`, `__rain`; `WeatherTick` guida gli effetti
tramite `[world+0xF78]`.

- Cosa loggare: `ListClasses=*Effect*` e `LogVtable=1` sui due componenti; capture dello slot
  non stub che viene chiamato a ogni frame per sapere quali oggetti esistono.
- Hook atteso (DESIGN.md 7): riuso degli effetti esistenti chiedendo l'effetto per nome al sistema
  che li crea; da progettare dopo aver visto chi chiama la creazione (chiamanti nel log).

## 3. Rischi: Denuvo e controlli di integrità

Da recon C: Denuvo ha un VEH, interroga `NtQuerySystemInformation`, ha ~20.000 funzioni spostate e
mutate nella sezione protetta (eseguibile, spesso RWX) e funzioni trigger che fanno controlli
inline. Il gioco controlla da sé solo `papgt`/`pamt` (lookup3), non l'exe.

Come le probe evitano i problemi:

- Mai lettura, scansione o scrittura nelle sezioni classificate `OBFUSCATED` (la vista in processo le
  marca non accessibili). Le firme si cercano solo nella sezione di codice.
- MinHook solo su bersagli nella sezione di codice; uno stub `jmp rel32` verso la sezione protetta è
  accettato (si sposta il jmp, il corpo protetto non viene toccato), un bersaglio dentro la sezione
  protetta viene rifiutato (`REFUSED` nel log). Verificato sul gioco finto sotto wine.
- Lo scambio di slot vtable scrive 8 byte nei dati di sola lettura, con protezione ripristinata
  subito; nessun byte di codice del gioco cambia. Rischio residuo: un eventuale checksum dei dati di
  sola lettura (non osservato in recon C). Per questo il primo giro è senza hook e gli hook hanno
  `DurationSec` e vengono rimossi alla fine.
- Nessun breakpoint hardware, nessun registro di debug, nessun VEH nostro, nessun thread sospeso.
- Letture con VirtualQuery più ReadProcessMemory: un offset sbagliato produce "unreadable" nel log,
  non un crash. Pagine `PAGE_GUARD` mai toccate.
- `StartDelayMs` (30 s di default) lascia finire avvio, caricamento risorse e controlli iniziali.
- Il callback di cattura non alloca, non blocca e non scrive log: registra puntatori e ritorna.
- Se dopo una patch le firme non hanno una sola corrispondenza, la probe si disattiva
  ("ambiguous" o "0 match(es)"); le probe `Rva=` valgono solo con `ExpectTimestamp` uguale all'exe.

## 4. Stato degli strumenti

- Fatto e verificato: framework di probe (scanner del modulo, RTTI, hook guardati, ini, log),
  test nativi `tests/test_probe.cpp`, test end-to-end `tests/probe_fake/run.sh` (gioco finto con RTTI
  MSVC vera, eseguito sotto wine).
- NON fatto: `tools/sigscan.py` (scanner offline dell'exe). Fino ad allora la stessa informazione si
  ottiene in processo: `ListClasses`/`LogVtable` per RTTI e vtable, probe `Signature=` per sapere se
  una firma ha una sola corrispondenza e dove punta.
