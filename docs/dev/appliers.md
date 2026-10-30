# Applicatori: dal motore stagioni al gioco (targets.ini)

Codice: `src/core/apply/` (logica portabile, testata in `tests/test_apply.cpp`), `src/win/apply/`
(ricerca degli oggetti con gli strumenti delle probe, scritture protette, thread a 10 Hz). Modello
commentato: `dist/bin64/scripts/QuattroStagioni/targets.ini`. Prova completa sotto wine:
`tests/apply_fake/run.sh`.

Idea: le probe (`docs/recon/probe_plan.md`) trovano dove stanno nel gioco orologio, velocità del
tempo, meteo, neve, luce e particelle. Una volta noti oggetto e offset, la sessione locale compila
`bin64\scripts\QuattroStagioni\targets.ini` e rilancia il gioco: niente ricompilazione.

## Ciclo di lavoro sul PC di Stefano

1. Con le probe (`probes.ini`, prima `AllowHooks=0`) trovare l'oggetto: classe RTTI, modo di
   trovarlo (scan, global, capture) e il campo (offset, tipo, valori tipici). Annotare nel log
   `probe.log` le righe `instance #0 ... class X` e i `Watch`/`Stats` del campo.
2. Copiare `targets.ini` in `bin64\scripts\QuattroStagioni\`, scrivere la sezione `[object.*]` con lo
   stesso metodo usato dalla probe e la sezione `[target.*]` con offset, tipo e semantica.
3. Accendere UN target alla volta: `[General] Enabled=1` e `Enabled=1` sul target. `LogLevel=2` per
   vedere ogni valore scritto.
4. Avviare, entrare in partita, guardare `QuattroStagioni.log` (righe `apply:`):
   `object X found`, `target Y: attached at ..., original ...`, `target Y active`.
   Se compare `DISABLED: value not sticking` il gioco riscrive quel campo a ogni frame: il campo
   giusto è un altro (di solito quello da cui il gioco lo ricalcola).
5. Verificare in gioco l'effetto, poi passare al target successivo. Dal menu F5 si possono spegnere le
   funzioni (meteo, neve, luce, durata del giorno, particelle): i valori originali tornano subito.

Corrispondenza probe -> targets.ini (stesse chiavi dove possibile):

| probes.ini | targets.ini `[object.*]` |
|---|---|
| `Class=` + `Instance=scan` | uguale |
| `Signature=`, `SigOffset=`, `Resolve=`, `Instance=global`, `Chain=` | uguale |
| `Rva=` + `[General] ExpectTimestamp` | uguale (ignorato se l'exe cambia) |
| `Instance=capture`, `Hook=vtable|minhook`, `Slot=`, `CaptureArg=` | uguale, serve `AllowHooks=1` |
| `Watch=0x34:f32:nome` | `[target.*] Offset=0x34`, `Type=f32` |

In più: `Check=offset:tipo:min:max` scarta i candidati non plausibili (copie vecchie nell'heap),
`Multiple=refuse|first|all` decide cosa fare con più candidati (default: rifiuta e scrive nel log gli
indirizzi), `KeepHook=0` toglie l'hook di cattura appena l'oggetto è trovato.

## Semantiche ed esempi

Valori del motore (`src/core/season/season.h`, regione `[General] Region`, default Temperate).
"v" è il valore stagionale; il valore scritto dipende da `Mode` (vedi sotto).

### Orologio (sola lettura, `Mode=read`)

| Semantica | v | Note |
|---|---|---|
| `hour` | ora 0..24 | float in ore, oppure intero con un target `minute` |
| `minute` | 0..59 | sommato a `hour`/60 |
| `day_counter` | contatore dei giorni | rende esatti i salti (sonno, caricamento) |

```ini
[object.gametime]
Instance=global
Signature=C5 FC 10 05 ?? ?? ?? ?? EB 08 C5 FC 10 05 ?? ?? ?? ?? 80 BB 7C 01 00 00
SigOffset=10
Resolve=rip:4:8
Check=0x4:u32:0:23

[target.clock_hour]
Enabled=1
Object=gametime
Semantic=hour
Type=u32
Offset=0x4
```

Quando un target dell'orologio è attivo il modulo mette `qs::shared().gameClockHooked=true` e
`applyReadGameClock()` dà al tick delle stagioni le ore di gioco trascorse (un caricamento che
riporta indietro l'orologio vale 0 ore).

### Durata del giorno (`dayLengthControl`)

| Semantica | v | Mode default |
|---|---|---|
| `time_scale_day` | `dayScale` (inverno circa 1,75) | `scale`: originale x v |
| `time_scale_night` | `nightScale` (inverno circa 0,54) | `scale` |
| `time_scale_current` | `dayScale` dalle 03:00 alle 19:00, altrimenti `nightScale` | `scale` |

L'originale si legge al primo aggancio e torna all'uscita. Se il gioco cambia da sé la velocità
(per esempio un'impostazione) il target la segue (nuovo originale) invece di combatterla.

```ini
[target.time_scale_day]
Enabled=1
Object=weather_time
Semantic=time_scale_day
Offset=0x40
Min=0.25
Max=4
MaxRate=0.5
```

### Meteo (`weatherControl`)

Il modulo calcola una sequenza meteo deterministica dai pesi stagionali (sezione `[Weather]`):
fasce di `HoldHours` ore di gioco, la prima parte (`BlendHours`) sfuma dal meteo precedente, con
probabilità `Persistence` il meteo resta uguale. Stesso calendario = stesso meteo.

| Semantica | v |
|---|---|
| `weather_manual_flag` | 1 (scritto SOLO quando un altro target del meteo è attivo) |
| `weather_state_flags` | bit `Flag<Stato>` del meteo corrente (`Type=flags32`, `Mask=` i bit nostri) |
| `weather_index` | `Index<Stato>` del meteo corrente |
| `weather_weight_<stato>` | peso stagionale normalizzato (il gioco sceglie da sé) |
| `weather_mix_<stato>` | quota 0..1 di quello stato nella sequenza (con sfumatura) |
| `precipitation` | pioggia + temporale + neve della sequenza, 0..1 |
| `rain_amount` | pioggia + temporale, 0..1 |
| `snow_rate` | neve che cade, 0..1 |
| `temperature_offset` | media giornaliera della regione meno il massimo estivo (<= 0, inverno circa -28) |
| `temperature` | temperatura della regione in gradi, mai sotto -30 |

Stati: `sunny cloudy rainy storm snowy windy`. Due strade, a seconda di cosa trovano le probe:
pilotare il meteo (flag manuale + flag di stato o indice) oppure lasciare scegliere il gioco con i
pesi stagionali (`weather_weight_*`).

```ini
[target.weather_manual]
Enabled=1
Object=weather_manager
Semantic=weather_manual_flag
Type=u8
Offset=0x31

[target.weather_flags]
Enabled=1
Object=server_weather
Semantic=weather_state_flags
Type=flags32
Mask=0x3F
Offset=0xFC

[target.climate_temperature]
Enabled=1
Object=weather_manager
Semantic=temperature_offset
Mode=offset
Offset=0x34
Min=-30
MaxRate=0.5
```

### Neve (`snowAccumulation`)

| Semantica | v |
|---|---|
| `snow_amount` | neve accumulata 0..1 (`snowCover` del motore: cresce quando nevica sotto zero, si scioglie sopra) |

```ini
[target.snow_amount]
Enabled=1
Object=weather_constant
Semantic=snow_amount
Offset=0x170
Min=0
Max=1
MaxRate=0.01
```
Se il gioco usa un'altra scala (per esempio 0..100) basta `Scale=100`.

### Luce (`lightControl`)

| Semantica | v | Mode default |
|---|---|---|
| `sun_elevation_offset` | gradi rispetto all'estate (inverno circa -47) | `write` (di solito `offset`) |
| `light_tint_r`, `light_tint_g`, `light_tint_b` | tinta moltiplicativa, 1 = originale | `scale` |
| `saturation` | saturazione moltiplicativa, 1 = originale | `scale` |

```ini
; sole più basso = latitudine più alta
[target.sun_latitude]
Enabled=1
Object=weather_constant
Semantic=sun_elevation_offset
Mode=offset
Scale=-1
Offset=0xA4
MaxRate=0.2

[target.tint_b]
Enabled=1
Object=weather_constant
Semantic=light_tint_b
Offset=0x68
MaxRate=0.02
```
Con `Mode=scale`, `Scale=0.5` dimezza l'effetto (intensità).

### Particelle (`particles`)

| Semantica | v |
|---|---|
| `leaves_particles` | intensità delle foglie che cadono 0..1 per la regione |
| `breath_visible` | 1 quando la temperatura è sotto la soglia del fiato (`breathThresholdC`) |

```ini
[target.breath]
Enabled=1
Object=weather_effect
Semantic=breath_visible
Type=u32
Bit=0
Offset=0x58
```

## Modi e limiti

- `Mode=write`: valore = v x `Scale` + `Bias`.
- `Mode=scale`: valore = originale x (1 + (v - 1) x `Scale`); `Scale` è l'intensità (1 = piena).
- `Mode=offset`: valore = originale + v x `Scale` + `Bias`.
- `Mode=read`: solo lettura (orologio); `Min`/`Max` sono l'intervallo plausibile.
- `Min`/`Max`: limiti sul valore finale scritto. `MaxRate`: variazione massima per secondo reale
  (anche all'aggancio: si parte dall'originale e si arriva al valore stagionale gradualmente).
- `Bit=n` (u8, u32, flags32): si scrive solo quel bit. `Mask=` (flags32): si scrivono solo quei bit.
  In entrambi i casi gli altri bit restano quelli del gioco, anche al ripristino.
- Tipi interi: arrotondati e saturati.

## Regole di sicurezza (non negoziabili)

- Si scrivono solo CAMPI DATI di oggetti del gioco: mai byte di codice, mai vtable, mai la sezione
  Denuvo. Il controllo è in `src/win/apply/guarded_memory.cpp`: ogni pagina deve essere impegnata,
  non di guardia, e o memoria privata `PAGE_READWRITE` (heap del gioco) o la sezione dati
  scrivibile dell'exe (`PAGE_READWRITE`/`PAGE_WRITECOPY`). Codice, dati di sola lettura, sezione
  protetta (RWX), file mappati e altri moduli vengono rifiutati. Ogni scrittura viene riletta.
- Un offset sotto 8 su un oggetto con classe viene rifiutato (sovrapporrebbe il puntatore vtable).
- Oggetto perso (vtable cambiata, memoria liberata): non si scrive più nulla lì, nemmeno il
  ripristino; l'oggetto viene cercato di nuovo e l'originale riletto.
- Anomalie: `AnomalyLimit` volte di fila (scrittura rifiutata, rilettura diversa, valore riscritto
  dal gioco, valore illeggibile o non plausibile) e il target si spegne da solo rimettendo
  l'originale. Il motivo è nel log e nello stato del menu.
- Il flag del meteo manuale non viene mai acceso da solo: solo se un altro target del meteo è attivo,
  così il gioco non resta con il meteo bloccato.
- Regione disattivata (Deserto, Abisso) o funzione spenta dal menu: originali ripristinati.
- Hook: solo con `AllowHooks=1` e solo per trovare oggetti (`Instance=capture`); sono gli stessi
  hook guardati delle probe (slot vtable o MinHook nella sezione di codice, mai nella sezione
  protetta) e vengono tolti appena l'oggetto è trovato.
- Primo giro con un target nuovo: un target alla volta, `MaxRate` basso, partita di prova.
- Gli Rva= valgono solo se `ExpectTimestamp` coincide con l'exe: dopo una patch servono firme o RTTI.

## Integrazione (per la sessione principale)

- `main.cpp`: `qs::applyStart()` a fine `initialize()` dopo `seasonStart()` e `probeStart()`;
  `qs::applyStop()` nel ramo FreeLibrary di `DLL_PROCESS_DETACH`, PRIMA di `probeStop()` e di
  `io::uninstall()`. Header: `src/win/apply/apply.h`.
- `season_tick.cpp`, con `daySource == GameClock`: chiamare `applyReadGameClock(day, ore)` a ogni
  giro; se risponde true usare `cfg.advance(day, dt, ore)` e `engine.update(day, ore, oraDelGioco)`
  (`applyGameHourOfDay`), altrimenti il ripiego attuale sul tempo di gioco reale.
- Menu F5: `applyGetStatus(status)` restituisce `apply::ApplyStatus`: per ogni funzione lo stato
  (`inactive`, `not found`, `active`, `error`) e il numero di target, per ogni target stato,
  originale, valore attuale, anomalie ed errore, il meteo della sequenza, l'ora del gioco.

## Test

- `./build.sh test` esegue `test_apply` (parser, sequenza meteo, orologio, mappatura, controller
  su memoria finta: originali, limiti, `MaxRate`, valore non mantenuto, scrittura rifiutata, oggetto
  perso, bit preservati, flag manuale, interruttori, stato).
- `tests/apply_fake/run.sh` (clang-cl, mingw, wine): gioco finto con RTTI MSVC vera. Trova un
  oggetto per catena di puntatori, uno per scan dell'heap (poi sostituito: deve accorgersene e non
  scrivere più nel vecchio blocco), uno per cattura di slot vtable, una struttura orologio nei dati
  dell'exe; scrive valori invernali nei limiti e con `MaxRate`, rinuncia al campo riscritto dal gioco,
  rifiuta `.rdata`, ripristina tutto all'uscita.
