# Probe delle funzioni finali (fulmini, luce, durata del giorno, particelle)

Tutte le quattro funzioni scrivono nei due blocchi che il gioco ricompone a ogni tick, raggiunti
dall'hook del meteo già verificato su 2.04.02 (exe 0x6AE46DD0):

| Blocco | Catena dal climate manager | Noto |
|---|---|---|
| WeatherConstant | `[[mgr+0x60]+0x20]` | +0x12C precipitazione, +0x130 nuvolosità, +0x134 umidità, +0x16C neve, +0x170 pioggia |
| AtmosphereConstant | `[[mgr+0x60]+0x28]` | +0xAC copertura nuvolosa; sole, luce, nebbia, cielo: DA TROVARE |

Nessuna firma nuova: servono solo gli offset dei campi. Le probe leggono gli stessi blocchi senza un
secondo hook (`Instance=provided`, `Provider=climate_manager`, `Chain=`), quindi il modulo meteo deve
essere avviato (anche con `[Weather] Enabled=0`: l'hook resta e il meteo non scrive).

## Come si fa un giro
1. In `bin64\scripts\QuattroStagioni\probes.ini`: `[General] Enabled=1`, `AllowHooks=0`,
   `ExpectTimestamp=0x6AE46DD0`; nelle probe che servono `Enabled=1` (le probe vecchie senza
   `Enabled` girano tutte: per un log più corto metterle a `Enabled=0`).
2. Avviare il gioco, caricare il salvataggio, aspettare circa 30 s (StartDelayMs).
3. Annotare l'ora reale degli eventi (alba, tramonto, inizio pioggia, primo tuono, nebbia).
4. Leggere `bin64\scripts\QuattroStagioni\probe.log`: `probe layer_atmosphere: instance #0 at ... (provider chain)`,
   poi i dump con le sole dword cambiate e i riepiloghi Stats (min, max, media per offset).
5. Scrivere l'offset trovato in `bin64\scripts\QuattroStagioni\targets.ini`, nella sezione della funzione,
   con la forma `<blocco>:<offset>[:<quanti float>]` (es. `atmosphere:0x1A0:3`), e `Enabled=1`.
6. Rilanciare con `probes.ini` di nuovo `Enabled=0` e guardare le righe della funzione in
   `QuattroStagioni.log`.

## Probe pronte (spente)
- `layer_atmosphere` (0..0x400), `layer_atmosphere_tail` (0x400..0x800): luce, sole, nebbia, cielo.
- `layer_weather`: il blocco meteo (fulmini, particelle).
- `layer_table`, `layer_slot_30/38/40/48`, `climate_manager`: gli altri livelli (fulmini, effetti).
- `lightning_atmosphere`, `lightning_weather`, `lightning_manager`: campionamento a 100 ms durante i temporali.
- `list_*` (Lightning, Thunder, Atmosphere, Sky, Light, Fog, Particle, Effect, Leaf, Wind, Foliage,
  Vegetation): elenco delle classi RTTI, utile per capire dove stanno gli effetti.

Guide per funzione: [fulmini.md](fulmini.md), [luce.md](luce.md), [durata_giorno.md](durata_giorno.md),
[particelle.md](particelle.md).
