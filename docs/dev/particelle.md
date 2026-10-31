# Particelle stagionali: come provarle (per Stefano)

Il modulo (`src/core/apply/particles.*`, `src/win/apply/particles_win.*`) ha tre parti:

| Parte | Stato alla consegna | Dove si regola |
|---|---|---|
| Fiocchi più fitti d'inverno | ACCESA (valori prudenti) | `QuattroStagioni.ini`, sezione `[Particles]` |
| Foglie che cadono in autunno | SPENTA: campo del gioco non ancora trovato | `QuattroStagioni\targets.ini`, sezione `[particles]` |
| Pollini e petali in primavera | SPENTA: campo del gioco non ancora trovato | come sopra |

Tutto passa dall'aggancio del meteo (`weather:`): se il meteo è disattivato, anche le particelle lo sono.
Interruttore in gioco: F5, "Particelle". Nel deserto resta sempre il valore del gioco.

## 1. Fiocchi d'inverno

Usano solo il campo neve del WeatherConstant (+0x16C, verificato su 2.04.02, lo stesso che scrive
il modulo meteo). Regole: mai neve dal nulla (sotto `SnowMinActive` non si tocca), solo d'inverno
(intensità invernale del calendario), mai nel deserto. Quando nevica la neve diventa
`max(neve, min(1, neve * WinterFlakeBoost))`, e almeno `WinterFlakeMin` se c'è precipitazione.

Prova:
1. Porta il calendario in inverno (F5, giorno manuale) e aspetta una nevicata (o un periodo di neve del meteo).
2. Nel log controlla:
   - `particles: config Enabled=1 WinterFlakes=1 boost 1.25 min 0.35 ...`
   - `particles: after-compose callback on; winter flakes on, leaves off, pollen off`
   - `particles: on`
   - ogni 60 s: `particles: winter 100%, flakes 100% (snow 0.40 -> 0.50), ... N writes`
3. Se la neve sembra troppo fitta abbassa `WinterFlakeBoost` (minimo 1) o `WinterFlakeMin`; per spegnere
   solo i fiocchi `WinterFlakes=0`.
4. In caso di problemi compare una sola riga `particles: DISABLED: <motivo>` e il modulo smette di scrivere
   (il gioco ricompone il meteo a ogni tick, quindi non c'è nulla da ripristinare).

## 2. Foglie e pollini: trovare il campo

Non sappiamo ancora se il gioco abbia un valore che regola quante foglie cadono. Il codice è pronto,
ma finché non si trova un campo **le foglie e i pollini restano spenti** (nel log:
`particles: leaves off: campo non ancora trovato (probe), ...`). Può anche darsi che il campo non esista
(foglie legate a effetti fissi del bosco): in quel caso restano spenti per sempre.

### Cosa avviare
In `QuattroStagioni\probes.ini`:
- `[General] Enabled=1`, `AllowHooks=0` (sola lettura).
- `Enabled=1` su `list_particle`, `list_effect`, `list_leaf`, e se servono `list_wind`, `list_foliage`,
  `list_vegetation`: elencano le classi RTTI con quei nomi.
- `Enabled=1` su `layer_weather` e `layer_table` (blocchi composti dal gioco a ogni tick).

### Cosa cercare
- Nel probe.log, tra le classi elencate: effetti di foglie, vento o vegetazione (nomi tipo
  `*LeafEffect*`, `*WindParticle*`, `*FallingLeaves*`, `*Foliage*`).
- Nei dump di `layer_weather` e `layer_table`: float che cambiano quando si entra in un bosco o quando
  aumenta il vento (le statistiche `Stats=1` mostrano i campi che variano). Prova: fermo in un bosco
  con vento forte, poi con vento debole, poi fuori dal bosco.
- Un buon candidato sta tra 0 e 1 (o è una quantità piccola), cambia con il vento e non con l'ora.

### Come scriverlo
In `QuattroStagioni\targets.ini`, sezione `[particles]`:
```
[particles]
Enabled=1
Leaves=atmosphere:0x1A0     ; esempio: livello:offset[:quanti float]
Pollen=
Mode=scale                  ; scale = moltiplica il valore del gioco; write = scrive Value * intensità
Value=2                     ; scale: x2 in pieno autunno; write: valore con intensità 1
Max=1                       ; limite del valore scritto
```
I livelli sono `weather` ([[mgr+0x60]+0x20]), `atmosphere` ([[mgr+0x60]+0x28]) e `manager` (il climate
manager). Riavvia il gioco: nel log `particles: after-compose callback on; ... leaves atmosphere:0x1A0`.
Intensità: foglie dal calendario (regione temperata, o `LeavesPerPhase` in `[Particles]`), pollini da
`PollenPerPhase` (alti in primavera, zero altrove), con lo stesso passaggio graduale delle texture.

## Righe di log da controllare
- `particles: config ...` e `particles: start, waiting for the weather hook`: avvio.
- `particles: leaves off: ...` / `particles: pollen off: ...`: motivo per cui sono spente.
- `particles: on` / `particles: off (...)`: interruttore F5 o regione.
- `particles: winter ..., flakes ..., leaves ..., pollen ..., N writes`: ogni 60 s.
- `particles: DISABLED: <motivo>`: spento al primo errore di lettura o scrittura.
- Riepilogo di avvio: `startup in ... weather on, particles on, ...`.
