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
