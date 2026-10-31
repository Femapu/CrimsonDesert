# Motore stagioni (src/core/season)

Codice portabile e deterministico, senza Win32: lo usano il plugin, il generatore e lo strumento
`qs_season`. Contratto: `src/core/season/season.h`.

## Calendario
- Giorno 0 = primo giorno di primavera. L'anno è la somma delle cinque fasi (predefinito 8+8+5+8+9 = 38
  giorni) e si ripete all'infinito; `day` è un `double` continuo (anche negativo).
- Ogni regione ha il suo calendario: le fasi vengono allungate da `phaseScale` e poi rinormalizzate
  perché l'anno regionale resti di 38 giorni (le regioni non si sfasano mai), poi tutto è spostato di
  `dayOffset` (negativo = in anticipo). Nord: inverno da 21,9 a 35,0, estate di 5,4 giorni.
- Ogni fase: plateau (look pura) e coda di transizione (`transitionFraction`, 40%) divisa in
  `transitionSteps` (3) stadi uguali: `A`, `A>W.1`, `A>W.2`, `A>W.3`, poi `W`.
- Stadi distinti con i valori predefiniti: **20** (5 fasi x 4), di cui 19 da generare (`U` è il gioco
  originale). Con N stadi per transizione: 5 x (N+1). `SeasonConfig::stageCount()`.
- Varietà per asset: `assetJitter(cfg, seme)` sposta il calendario dell'asset di +-`assetJitterDays`
  (hash murmur3 del seme). `stageAt(cfg, regione, giorno, seme)` usa solo aritmetica semplice, quindi
  generatore nativo e plugin Windows danno sempre lo stesso stadio (verificato: `qs_season` nativo e
  `.exe` sotto wine producono la stessa tabella).
- Regioni disattivate (Deserto e Abisso per impostazione): sempre plateau estivo, niente neve.

## Modelli
- Temperatura: media + ampiezza x coseno annuo (minimo a metà inverno regionale) + ciclo giornaliero
  (minimo alle 03:00, massimo alle 15:00). Mai sotto `kMinTempC` (-30 C): il gioco infligge danni da
  freddo.
- Meteo: tabella per fase (estate limpida, autunno piovoso e ventoso, inverno coperto), sfumata nella
  coda di transizione, corretta dall'umidità; le precipitazioni diventano neve sotto 0 C (metà a 0 C,
  tutte a -2 C). Pesi normalizzati a 1, stessi nomi dei flag `WeatherState` del gioco.
- Neve: integrazione del valore atteso (nessun numero casuale) a passi di mezz'ora di gioco:
  accumulo = `snowAccumPerHour` x peso neve x freddo, scioglimento = `snowMeltPerHour` x (T/3).
  Dopo un salto (indietro, più di 4 giorni o più di 96 ore) si risimulano le due settimane
  precedenti: il risultato dipende solo dal calendario, quindi non va salvato.
- Luce: tinta e saturazione per fase (estate = 1), declinazione solare mappata sull'anno di gioco con
  l'intero plateau estivo come solstizio (estate = gioco originale) e il minimo a metà inverno.
- Durata del giorno: alba e tramonto dalla latitudine virtuale, applicati alla finestra del giorno del
  gioco (03:00-19:00). `dayScale` e `nightScale` sono i moltiplicatori per le due velocità
  dell'orologio del gioco (giorno e notte): il ciclo dura uguale, cambia la quota di luce
  (inverno a 45 gradi: 9,2 ore di luce, giorno x1,75, notte x0,54).
- Foglie che cadono: iniziano nella coda di fine estate, crescono in autunno, picco all'inizio del
  passaggio autunno -> inverno, spariscono a metà del plateau invernale.

## Neve d'inverno nel gioco (soglie SnowStart / SnowFull)
Codice: `src/core/apply/snow.*` (portabile, `tests/test_snow.cpp`), `src/win/apply/snow_win.*`.
Il gioco ha due variabili globali (`pa::GlobalVariableFloat` nella sezione dati dell'exe, valore a
+0x38, default a +0x3C, vanilla -5 e -20, trovate con le probe `cvar_snow_start` e `cvar_snow_full`):
mostra neve dove la temperatura del clima è <= SnowStart e copertura piena a <= SnowFull (significato
dedotto, da confermare in gioco). Con i valori vanilla nessuna zona è abbastanza fredda.
- `winterIntensity` (0..1, calendario della regione temperata): sale nella seconda metà della coda
  A>W, vale 1 in inverno, scende nella prima metà della coda W>S, 0 altrove (smoothstep, continua).
- Valori scritti = originali + intensità x (valori d'inverno - originali); intensità 0 = originali
  esatti. Velocità massima `MaxRate` (0,5 gradi al secondo reale): mai salti a metà frame.
- Default `[Snow]`: `WinterSnowStart=10`, `WinterSnowFull=0`, `MaxSnowStart=10` (limite di sicurezza,
  massimo assoluto 10), `SnowFull` sempre almeno 1 grado sotto `SnowStart`. Il deserto resta senza
  neve perché la sua temperatura di clima è molto sopra qualunque soglia ammessa.
- Sicurezze: exe in `known_builds.h` e controllo di versione riuscito, firma univoca, classe RTTI
  `GlobalVariableFloat`, valori in [-60, +30] e uguali al default al primo aggancio; se il gioco o
  un'altra mod cambiano i valori per 3 tick di fila, la neve si spegne e ripristina gli originali.
  Ripristino anche con "Neve che si accumula" spento nel menu F5 e allo scaricamento del plugin.
- Log: `snow: SnowStart found ... class ...`, `snow: attached, originals ...`, `snow: SnowStart a -> b,
  SnowFull c -> d (...), Inverno 30%, winter intensity 1.00` (al massimo ogni 60 s), `snow: DISABLED: ...`.

## Meteo stagionale (`[Weather]`, src/core/apply/weather.*, src/win/apply/weather_win.*)
Porting adattato del meteo di Seasons of Pywel (Khione95, con il suo permesso scritto): stessi punti
di aggancio e stessi campi, guidato dal nostro motore stagioni e dalle regole di Stefano.
- **Aggancio**: firme uniche (verificate sugli exe 0x6AE387F2 e 0x6AE46DD0) dell'update del
  climate manager (compone i livelli meteo) e della query meteo dell'HUD, via `GuardedMinHook`.
  Dopo la composizione del gioco: WeatherConstant `[[mgr+0x60]+0x20]` (+0x12C precipitazione,
  +0x130 nuvolosità, +0x134 umidità, +0x16C neve, +0x170 pioggia) e atmosfera `[[mgr+0x60]+0x28]`
  +0xAC (copertura nuvolosa del cielo), scritte con `GuardedMemory`. Nulla da ripristinare: il gioco
  ricompone ogni tick, basta smettere di scrivere.
- **Periodi**: tipo (gioco, sereno, nuvoloso, coperto, pioggia, temporale), intensità e durata in
  ore di gioco, tirati dai pesi della fase (`<Fase>.Weights`); nella coda di transizione entrano le
  regole della fase dopo. L'orologio è l'ora del gioco (`[mgr+0x100]`, virtuale +0x100): dormire o
  aspettare fa avanzare i periodi. Livelli sfumati a `RampPerHour`.
- **Inverno**: pioggia e pioggia del gioco diventano neve (stessa rampa `winterIntensity` dei cvar
  della neve); cielo coperto o nuvoloso prevalente. Fuori dall'inverno la mod non crea mai neve.
- **Deserto**: mappa del clima CPU del climate manager (512x512, 80 m, byte alto = massima del
  giorno); se un texel entro `DesertRadius` ha massima >= `DesertTopTempC` il meteo torna al gioco in
  `DesertFadeSec`. Mappa non trovata: modulo spento (mai scrivere senza sapere se è deserto).
- **Temporali**: il flag Storm dell'HUD (bit 6) esiste, ma nessun campo noto guida i fulmini: il
  temporale è il livello di pioggia più forte con cielo chiuso; il log lo dice.
- Log: `weather: spell ...`, `weather: HUD ...`, riepilogo ogni `LogIntervalSec`, `weather: DISABLED: ...`.

## Temperature stagionali (`[Climate]`, src/core/apply/climate.*, src/win/apply/climate_win.*)
Porting adattato di climate.cpp di Seasons of Pywel (Khione95, con il suo permesso scritto), guidato
dal nostro calendario.
- **Mappe**: il gioco tiene due mappe del clima CPU (`pa::PAClimateTexture`: quella del weather
  manager e quella del climate manager a +0xD8). Oggetto: +0x18 texel, +0x20 numero (512x512), +0x30/+0x34
  metri per texel (80), +0x38/+0x3C larghezza e altezza. Texel u32 = A | R<<8 | G<<16 | B<<24; B =
  massima del giorno `(2B/255-1)*50`, G = escursione (notte = massima - ((1-G/255)*18+7)), R = vento.
  Il gioco le campiona dal vivo: temperatura percepita, termometro, neve e neve profonda cambiano subito.
- **Ricerca**: vtable dal nome RTTI `.?AVPAClimateTexture@pa@@` (nessun indirizzo fisso; l'indirizzo
  noto per build, 0x5E8B7C0 su 0x6AE387F2 e 0x5E8D890 su 0x6AE46DD0, serve solo da controllo nel log),
  poi scansione dell'heap (regioni private RW, a blocchi di 4 MB). Nuova scansione quando una mappa
  sparisce o cambia buffer (caricamento del mondo), quando `[mgr+0xD8]` punta a una mappa non nota, ogni
  15 s se non ce ne sono, ogni 5 minuti comunque.
- **Controlli prima di scrivere**: vtable, 512x512, 262144 texel, 80 m, buffer leggibile; dati
  plausibili (almeno un texel desertico, almeno il 10% di terra mite). Oggetti con la vtable ma un altro
  formato e nessuna mappa valida: `climate: DISABLED`. Nessuna mappa in 3 minuti: modulo spento.
- **Originale**: copiato all'aggancio. Se il gioco riscrive la mappa (contenuto diverso sia
  dall'originale sia dal nostro), il nuovo contenuto diventa l'originale. Ripristino (solo se la mappa
  contiene ancora i nostri valori) con l'interruttore F5 spento, regione temperata spenta, modulo
  disattivato e allo scaricamento del plugin.
- **Differenza**: valore della fase sul plateau (`SpringOffsetC=-6`, `SummerOffsetC=4`,
  `LateSummerOffsetC=2`, `AutumnOffsetC=-8`, `WinterOffsetC=-24`), sfumato con smoothstep nella coda
  di transizione (la stessa da cui escono i 20 stadi visivi, quindi coerente con le texture ma
  continuo, senza salti di 4-5 gradi tra uno stadio e l'altro). La differenza in vigore insegue quella
  della stagione a `MaxRate` (0,5 gradi al secondo reale) ed è arrotondata a 0,25 gradi; la mappa si
  riscrive solo quando cambia. Cambia solo il byte B (G, R, A restano del gioco).
- **Deserto**: texel con massima >= `DesertTopTempC` (28, come il meteo); entro `DesertRadius` (4
  texel) restano del gioco in ogni stagione, poi la differenza entra in `DesertFade` (4 texel). Un
  riscaldamento non porta mai un texel alla soglia del deserto: il controllo del deserto del modulo
  meteo, che legge la mappa dal vivo, resta uguale a quello sui valori del gioco.
- **Notti**: un raffreddamento non porta mai la notte sotto `NightFloorC` (-30; il freddo sotto circa
  -40 ferisce). Dove la notte del gioco è già più fredda (montagne innevate) quel texel e i suoi 8
  vicini restano del gioco (il campionamento bilineare mescola 2x2 texel). In estate il riscaldamento
  vale anche lì.
- **Neve profonda (TODO)**: le soglie della neve profonda (cvar a 0x71A8DE8 / 0x71A8E38 sul build
  0x6AE387F2, subito dopo SnowStart/SnowFull con passo 0x50; si troverebbero con lo stesso metodo dei
  nostri cvar della neve) restano quelle del gioco. Motivo: la GPU decide la neve profonda dalla texture
  del clima su disco, che non cambiamo; spostare la soglia solo sulla CPU darebbe neve profonda
  "logica" (test 3x3 del gioco) dove la GPU non la mostra. Con i default le zone temperate d'inverno
  stanno sopra -20, quindi la neve profonda resta dove la mette il gioco.
- **Neve e temperature insieme**: le soglie `[Snow]` (10 / 0 d'inverno) restano attive: le due cose
  spingono nella stessa direzione (con -24 la terra temperata è sotto 10 e quasi sempre sotto 0, quindi
  la neve piena è coerente col termometro) e il deserto resta escluso dal modulo meteo. Con
  `ClimateControl` spento le soglie bastano da sole (provato in gioco il 30/10). Se in gioco la neve
  sembra eccessiva con le temperature attive, provare `WinterSnowStart=2`, `WinterSnowFull=-6`.
- Menu F5, scheda Meteo e luce: interruttore "Temperature stagionali" (`ClimateControl` in
  `[Seasons]`), blocco "Temperature nel gioco" (stato, mappe, differenza in vigore, tipo di texel e
  massima sotto la camera); riga in Info.
- Log: `climate: PAClimateTexture vtable rva ... from RTTI (matches the known address)`, `climate:
  heap scan ...`, `climate: attached map ...`, `climate: map ... at -24.00 C: N texels changed ...`,
  `climate: season offset ...` (a ogni grado, all'arrivo e ogni `LogIntervalSec`), `climate: ... maps
  restored`, `climate: DISABLED: ...`.

## Come lo usa il plugin (ogni frame o ogni ~0,5 s)
```cpp
day = cfg.advance(day, secondiReali, oreDiGioco);  // secondo DaySource; salvare `day` (l'orologio del gioco riparte da 0 al caricamento)
engine.update(day, oreDiGioco);                    // oppure update(day, ore, oraDelGioco) se l'orologio è agganciato
const SeasonState& s = engine.state();            // stadi per regione, meteo, neve, luce, dayScale/nightScale
```
- `oreDiGioco` = ore di gioco trascorse dall'ultima chiamata (dormire 3..72 h va bene); se ignote, -1.
- Dopo una modifica dal menu: `engine.configure(cfg)`.
- Modalità manuale: `day = cfg.jumpToPhase(day, Phase::Autumn)`; etichetta: `cfg.dayToLabel(day)`.
- Prefetch dei pacchetti: `cfg.nextStageChangeDay(day, regione)` meno `assetJitterDays` = quando serve
  già il pacchetto dello stadio successivo (`nextStage`).
- File ini: `defaultIniText()` dà le sezioni `[Seasons]` e `[Region.*]` commentate in italiano, da
  accodare a `QuattroStagioni.ini`; `load()` riporta ogni errore e corregge i valori fuori intervallo.

## Strumento
`build/qs_season [--step 0.5] [--hour 12] [--years 1] [file.ini]` stampa la tabella dell'anno;
`--defaults` stampa il modello di ini, `--stages` l'elenco degli stadi.
