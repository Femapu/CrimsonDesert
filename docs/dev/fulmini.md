# Fulmini: come trovare il campo e verificare le frequenze

Stato: la logica dei fulmini è pronta (`src/core/apply/lightning.*`, `src/win/apply/lightning_win.*`),
manca il campo del gioco da scrivere. Finché `Field=` è vuoto la mod non scrive nulla nel gioco: decide
lo stesso i temporali con fulmini e i singoli lampi e li scrive nel log, così si possono già controllare
le frequenze. Il tuono resta sempre quello del gioco: la mod guida solo il lampo visivo.

## Come funziona
- A ogni nuovo temporale (temporale della mod, oppure bit Storm dell'HUD del gioco) la mod tira a sorte
  se avrà fulmini, con `ChancePerStorm` della stagione (default 0.6, 0.9, 0.85, 0.85, 0.03 per
  primavera, estate, fine estate, autunno, inverno).
- Durante un temporale con fulmini i lampi arrivano a caso (processo di Poisson): in media
  `StrikesPerHour` per ora di gioco (default 8, 14, 12, 12, 1) per l'intensità del temporale, almeno
  `MinGapSec` secondi reali l'uno dall'altro. Nelle transizioni tra stagioni i valori si mescolano come
  le texture.
- Mai nel deserto (stessa regola del meteo, anche finché il deserto non è ancora noto), mai con
  "Fulmini" spento nel menu F5, mai con la regione spenta. Dormire o aspettare non produce lampi.
- Un temporale finisce 20 secondi reali dopo che spariscono sia il temporale della mod sia il bit Storm.

## 1. Avviare le probe
In `bin64\scripts\QuattroStagioni\probes.ini`:
1. `[General]`: `Enabled=1`, `AllowHooks=0` (sola lettura, nessun hook in più).
2. Accendere (`Enabled=1`): `layer_weather`, `layer_atmosphere`, `layer_table`, `list_lightning`
   (e, se si vuole, `list_thunder`).
3. Per vedere i lampi servono anche le probe veloci (100 ms): `lightning_atmosphere`,
   `lightning_weather`, `lightning_manager`. Se `layer_table` mostra puntatori negli slot +0x30, +0x38,
   +0x40, +0x48 accendere anche `layer_slot_30` ... `layer_slot_48`.

Le probe `Provider=climate_manager` funzionano solo con il modulo meteo avviato (anche con
`[Weather] Enabled=0`): partono dopo circa 20 secondi (StartDelayMs del meteo) più quello delle probe.

## 2. Provocare un temporale
- Usare la collana della pioggia (o aspettare un temporale del gioco: estate e autunno sono le stagioni
  migliori).
- Restare fuori dal deserto, all'aperto, e aspettare i tuoni e i lampi del gioco. Annotare l'ora reale
  di qualche lampo ben visibile (serve per confrontarla con il log).
- Restare almeno 2 o 3 minuti nel temporale, poi uscire dal gioco.

## 3. Cosa cercare nel probe.log
- `list_lightning` / `list_thunder`: le classi con Lightning o Thunder nel nome (nome, vtable). Se ne
  esiste una per lo stato del lampo, è una candidata da seguire con `Class=` in una probe a parte.
- Nei dump veloci (`lightning_*`, `layer_*`) cercare le dword che:
  - restano ferme (di solito 0) fuori dai lampi;
  - saltano a un valore e tornano indietro in pochi campioni (100 a 500 ms) proprio nei momenti dei lampi;
  - non cambiano nei temporali senza lampi e fuori dal temporale.
  Le dword che cambiano piano (pioggia, nuvole, ora) non sono il campo giusto: quelle sono già note
  (+0x12C precipitazione, +0x16C neve, +0x170 pioggia nel blocco weather, +0xAC nuvole in atmosphere).
- Nei riepiloghi `Stats` un campo dei lampi ha minimo e massimo diversi ma media vicina al minimo.
- Annotare: blocco (weather, atmosphere o manager), offset, tipo (di solito f32), valore a riposo e
  valore durante il lampo.

## 4. Scrivere il campo in targets.ini
In `bin64\scripts\QuattroStagioni\targets.ini`, sezione `[lightning]`:

```
[lightning]
Enabled=1
Field=atmosphere:0x1A0
Mode=pulse
Value=1
```

- `Field=<livello>:<offset>[:<quanti f32>]`, livello `weather`, `atmosphere` o `manager` (offset
  multiplo di 4). Per tre float consecutivi: `atmosphere:0x1A0:3`.
- `Mode=pulse`: il campo vale `Value` per `PulseMs` (default 250 ms) a ogni lampo, poi torna il valore
  del gioco. Adatto a un campo che "lampeggia".
- `Mode=level`: campo = valore del gioco + (Value - gioco) * livello, con il livello 0..1 che sale
  durante un temporale con fulmini (intensità del temporale per la frequenza della stagione) e scende
  alla fine. Adatto a un campo tipo "probabilità o intensità dei fulmini" che il gioco usa da solo.
- Non c'è Restore: il gioco ricompone i livelli a ogni tick. Se il campo non viene ricomposto la mod
  rimette lei il valore del gioco quando smette di scrivere.
- Alla prima lettura o scrittura fallita il modulo si spegne da solo ("lightning: DISABLED: ...").

## 5. Righe di log da controllare (QuattroStagioni.log)
- `lightning: start (chance ..., strikes/game hour ..., ...); field ...`: configurazione letta. Con
  `field not set (targets.ini [lightning]): log only` la mod non scrive nel gioco.
- `lightning: strikes allowed` / `lightning: no strikes: in the desert` (o altro motivo).
- `lightning: storm begins (summer, chance 0.90): with lightning` oppure `without lightning`, e
  `lightning: storm ends`.
- `lightning: strike #N (summer 35%, game's storm, intensity 100%, rate 14.0/game hour, level 0.80)`:
  un lampo. Compare sempre finché il campo non è impostato; con il campo impostato serve
  `[Lightning] LogStrikes=1`. Contare i lampi per ora di gioco e confrontarli con `StrikesPerHour`.
- `lightning: DISABLED: ...`: il modulo si è spento (il motivo dice quale campo e quale indirizzo).
- `lightning: stop (N storms, N strikes, N writes)`: riepilogo all'uscita. Con il campo impostato,
  `writes` deve crescere durante i temporali e restare fermo fuori.
- Nel menu F5, scheda Info, la riga "Fulmini" mostra stato, motivo e una riga dal vivo (temporale,
  livello, lampi, ultimo lampo).

## Impostazioni ([Lightning] in QuattroStagioni.ini)
`Enabled`, `ChancePerStorm`, `StrikesPerHour`, `MinGapSec`, `PulseMs`, `RampSec`, `LogStrikes`: tutte
facoltative, documentate nel file. Valori fuori intervallo vengono corretti e segnalati nel log.
