# Luce e cielo stagionali: come trovare i campi

La luce stagionale (`src/core/apply/lighting.*`, `src/win/apply/lighting_win.*`) moltiplica alcuni
valori che il gioco ricompone a ogni tick: colore e intensità del sole, colore e intensità della luce
ambiente, densità della nebbia, colore del cielo. I moltiplicatori per stagione stanno in
`QuattroStagioni.ini`, sezione `[Lighting]` (estate = 1, cioè il gioco originale); i passaggi tra le
stagioni seguono lo stadio visivo della regione temperata (le 15 transizioni, come le foglie). Nel
deserto la luce torna sempre quella del gioco.

Manca un solo pezzo: **dove** stanno quei valori. Il codice è pronto, ma finché la sezione `[lighting]`
di `QuattroStagioni\targets.ini` non ha gli offset la funzione resta spenta (nel menu F5, scheda Info:
"Luce stagionale: spento, campi della luce non ancora trovati (probe)").

Il candidato principale è il blocco atmosfera che il gioco compone a ogni tick
(`[[climate manager + 0x60] + 0x28]`, AtmosphereConstant): +0xAC è la copertura nuvolosa (verificato),
e lo stesso blocco contiene direzione del sole, Rayleigh/Mie, ozono, nebbia in quota e molti offset
ancora ignoti.

## 1. Cosa avviare

In `bin64\scripts\QuattroStagioni\probes.ini`:

```ini
[General]
Enabled=1
AllowHooks=0

[probe.layer_atmosphere]
Enabled=1

[probe.layer_atmosphere_tail]
Enabled=1

[probe.list_atmosphere]
Enabled=1
[probe.list_sky]
Enabled=1
[probe.list_light]
Enabled=1
[probe.list_fog]
Enabled=1
```

Le probe `layer_*` leggono il blocco che il modulo meteo vede già (Provider=climate_manager): serve
solo che il meteo sia avviato (anche con `[Weather] Enabled=0`), nessun hook in più. Utile anche
`[probe.layer_weather] Enabled=1`, per confrontare con pioggia e nuvole. Le `list_*` elencano le
classi con quei nomi (vtable, numero di slot): servono se il blocco atmosfera non basta.

Per non mescolare gli effetti, durante le prove mettere in `QuattroStagioni.ini`
`[Lighting] Enabled=0` (oppure togliere "Luce stagionale" dal menu F5).

## 2. Cosa fare in gioco

1. Caricare una partita in una zona temperata all'aperto, lontano dal deserto.
2. **Restare fermi** (nessun movimento della telecamera) e lasciar passare il tempo dall'alba al
   tramonto: aspettare, o dormire a tappe di un'ora o due. Il sole cambia colore e intensità in modo
   netto all'alba e al tramonto: sono i momenti più utili.
3. Poi, sempre fermi, aspettare (o provocare con il meteo stagionale) **nebbia** e **pioggia**: il
   HUD mostra l'icona del tempo; annotare l'ora reale in cui compare la nebbia.
4. Uscire dal gioco e aprire `bin64\scripts\QuattroStagioni\probe.log`.

## 3. Come leggere il probe.log

Il Dump scrive solo le dword cambiate, con l'offset; Stats riassume minimo, massimo e quante volte
ogni float è cambiato negli intervalli indicati (0:1, 0:10, ...).

- **Terne RGB** (colore del sole, dell'ambiente, del cielo): tre float **consecutivi** (offset
  +0, +4, +8, a volte un quarto a +0xC), tutti tra 0 e 10, che cambiano **insieme** all'alba e al
  tramonto. Al tramonto il rosso resta alto mentre verde e blu scendono; a mezzogiorno i tre sono
  vicini. Il cielo di solito ha il blu più alto degli altri due.
- **Intensità** (sole, ambiente): un float solo, più alto a mezzogiorno, che scende verso il tramonto
  e resta basso di notte; a volte vale molto più di 1 (unità fisiche, decine o migliaia).
- **Densità della nebbia**: un float piccolo (spesso 0..1 o meno) che **sale quando il HUD mostra la
  nebbia** e torna giù quando passa; con la pioggia può salire un poco.
- Scartare i campi che non cambiano mai, quelli che saltano a ogni campione (contatori, tempi) e
  quelli già noti (+0xAC copertura nuvolosa).
- Se nei primi 0x400 byte non c'è niente di adatto, guardare `layer_atmosphere_tail` (0x400..0x800).

Per confermare un candidato, aggiungere una riga Watch alla probe, rilanciare e ripetere l'alba:

```ini
Watch=0xAC:f32:cloudCover, 0x1A0:f32:sunR, 0x1A4:f32:sunG, 0x1A8:f32:sunB
```

## 4. Come scrivere gli offset in targets.ini

In `bin64\scripts\QuattroStagioni\targets.ini`, sezione `[lighting]` (gli offset qui sotto sono solo
un esempio):

```ini
[lighting]
Enabled=1
SunColor=atmosphere:0x1A0:3
SunIntensity=atmosphere:0x1AC
AmbientColor=
AmbientIntensity=
FogDensity=atmosphere:0x2F0
SkyColor=
; limiti di sicurezza facoltativi sul valore del gioco
SunColorMax=20
FogDensityMax=10
```

- Formato: `<livello>:<offset>[:<numero di float>]`, livello `atmosphere`, `weather` o `manager`.
- Colori: 3 float (o 4, il quarto non viene toccato). Intensità e nebbia: 1 float.
- Si possono mettere anche solo alcuni campi: gli altri restano al gioco.
- Rimettere `[Lighting] Enabled=1` in `QuattroStagioni.ini` e "Luce stagionale" nel menu F5.

## 5. Cosa controllare nel QuattroStagioni.log

- `lighting: field SunColor = atmosphere:0x1A0:3 (limits 0 .. 100000)`: un rigo per campo letto.
- `lighting: start, N field(s), ramp ...`: la funzione è partita.
- `lighting: state active`: il meteo è agganciato e la luce stagionale scrive.
- Ogni 60 s: `lighting: stage A>W.2 on | sun 0.915 rgb ... | fog 1.100 | ... | writes N`. Il numero
  di scritture deve crescere; con l'estate i moltiplicatori sono 1 e non si scrive nulla.
- `lighting: DISABLED: <campo> non ricomposto dal gioco: si accumulerebbe`: il campo scelto non viene
  ricomposto dal gioco a ogni tick (la mod rileggeva il proprio valore); è il campo sbagliato, o una
  copia che il gioco scrive solo ogni tanto. La mod ha rimesso il valore originale: scegliere un
  altro offset.
- `lighting: DISABLED: <campo> ha un valore non finito` o `fuori dai limiti Min/Max`: campo
  sbagliato o limiti troppo stretti.
- `lighting: no light field ... in targets.ini [lighting]`: sezione spenta o vuota.

In gioco, con l'inverno (menu F5, salto alla stagione) il sole deve apparire un poco più debole e
freddo, in autunno più caldo; tornando in estate tutto deve essere identico al gioco originale.
Entrando nel deserto la luce torna quella del gioco in pochi secondi.
