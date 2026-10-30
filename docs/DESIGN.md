# QuattroStagioni: documento di design (v1, 30/10/2026)

Questo documento descrive COSA fa la mod e COME è divisa in moduli. È la fonte di verità per tutti
i flussi di lavoro (sessioni, agenti). I dettagli tecnici dei formati sono in `docs/recon/`.

## 1. Visione

Pywel attraversa un anno vero. La primavera porta prati verde chiaro e fioriture; l'estate è quella
del gioco originale, limpida; la fine estate ingiallisce i prati e secca i campi; l'autunno accende le
latifoglie di arancio e rosso e fa cadere le foglie; l'inverno spoglia gli alberi, porta neve che si
accumula, luce bassa e fredda, giornate corte e fiato visibile. Il cambiamento è graduale (giorno per
giorno), diverso per regione e mai uniforme (alberi singoli che cambiano prima o dopo).

Principi:
- **Sicura**: nessun file del gioco viene mai scritto. Qualsiasi dubbio porta al pass-through.
- **Legale da pubblicare**: non distribuiamo asset di Pearl Abyss. Distribuiamo codice, regole
  ("ricette") e un generatore che crea le varianti sul PC dell'utente dai suoi file.
- **Resistente alle patch**: controllo di versione all'avvio; hook per RTTI o firma; se qualcosa non
  torna la parte interessata si spegne (non il gioco) e il log spiega perché.
- **Configurabile**: menu F5 in gioco e file `.ini`.

## 2. Le cinque "look" stagionali (keyframe)

| Id | Look | Carattere |
|---|---|---|
| S | Primavera | verde giovane e saturo, fioriture, erba fresca e corta |
| U | Estate | vanilla (il gioco originale, nessuna modifica) |
| L | Fine estate secca | prati paglierini, campi dorati, latifoglie leggermente spente |
| A | Autunno | latifoglie gialle, arancio, rosse, ruggine; prati bruno oliva; foglie a terra |
| W | Inverno | latifoglie spoglie (alfa delle foglie a zero o quasi), erba morta e brinata, conifere più scure e fredde, neve |

Ciclo: S -> U -> L -> A -> W -> S.

## 3. Calendario e transizioni

- L'anno è fatto di 5 fasi con lunghezza configurabile in giorni di gioco (default: S 8, U 8, L 5, A 8, W 9).
- Sorgente del giorno, in ordine di preferenza: (1) orologio del gioco agganciato (A1-b);
  (2) tempo di gioco reale con "minuti reali per giorno" configurabile; (3) manuale dal menu F5.
- Ogni fase ha un "plateau" (look pura) e una coda di transizione verso la fase successiva
  (default: ultimo 40% della fase). Nella transizione si passa per **stadi discreti**: con qualità
  transizione N (default 3) gli stadi sono t = 1/(N+1) ... N/(N+1).
- Uno **stadio visivo** è la terna (from, to, step). Le texture di uno stadio sono generate dalla
  ricetta con interpolazione nello spazio dei parametri (non mescolando pixel).
- **Varietà per asset**: ogni asset ha un seme (hash del percorso). Il seme sposta il suo calendario
  di +-K giorni (default 2) e la sua tonalità autunnale (giallo / arancio / rosso / ruggine), così un
  bosco non cambia tutto insieme. Con stadi discreti lo spostamento si traduce in "questo asset usa lo
  stadio precedente o successivo".

## 4. Regioni

Regioni (da `region_hint` dell'inventario, poi dalla mappa regioni/clima del gioco per i tile):
- `shared_temperate`: ciclo completo.
- `north_cold`: inverno più lungo e anticipato (offset -3 giorni, inverno +50%), estate più corta.
- `swamp_jungle`: sempreverde, solo variazioni di umidità (S e L leggere), niente spoglio.
- `aquatic`: piante acquatiche, variazioni minime.
- `desert_dry` e `abyss`: escluse (sempre vanilla).
Ogni regione è attivabile/disattivabile e ha un offset e una scala configurabili.

## 5. Meteo, temperatura, neve

Modello interno (indipendente dal gioco, poi applicato tramite hook o dati):
- Temperatura = media annua regionale + ampiezza * coseno(giorno dell'anno) + ciclo giornaliero.
- Pesi meteo per stagione (Sunny, Cloudy, Rainy, Storm, Snowy, Windy, come l'enum `WeatherState`
  del gioco): estate limpida, autunno piovoso e ventoso, inverno nevoso.
- Neve accumulata 0..1: cresce quando nevica sotto zero, si scioglie sopra zero (velocità configurabili).
- Applicazione: pesi meteo e stato neve verso il `WeatherManager` del gioco (hook A1-b), offset di
  temperatura sulla `climate_texture_1.dds` (R = temperatura) generata per stagione dal generatore.

## 6. Luce e giornata (se agganciabili)

- Elevazione solare: offset per giorno dell'anno (latitudine virtuale configurabile).
- Tinta: più calda in autunno, più fredda e desaturata in inverno.
- Durata del giorno: alba e tramonto per giorno dell'anno; si applica cambiando la velocità del
  tempo del gioco di giorno e di notte (slot "advance time of day" visto in recon C).

## 7. Particelle

- Foglie che cadono (autunno, intensità in base allo stadio), fiato visibile sotto una soglia di
  temperatura, polvere di neve. Si riusano gli effetti del gioco (`fx_weather_a__snowdust` ecc.).

## 8. Architettura a moduli

```
             +-----------------------------+
 file gioco  |  qs_gen (generatore, PC     |  pacchetti stagionali
 (.pamt/.paz)|  utente): legge, decodifica,|-----> QuattroStagioni\packs\<stadio>.qsp
 ----------> |  applica ricette, ricodifica|
             +-----------------------------+
                                                       |
 +----------------------- QuattroStagioni.asi ---------v---------------------------+
 |  io_hooks (fase 2): servono le letture .paz con i byte del pacchetto attivo      |
 |  season engine: giorno -> stadio visivo per regione, meteo, neve, luce, giornata |
 |  overlay D3D12 + ImGui: menu F5 (calendario, stagioni, regioni, opzioni)         |
 |  gpu swap: sostituzione texture già caricate quando cambia lo stadio (A2)        |
 |  probes / game hooks: orologio, meteo, luce, particelle (A1-b, per RTTI/firma)   |
 +---------------------------------------------------------------------------------+
```

Cartelle e proprietari (vedi `docs/dev/CONTRACTS.md`):
- `src/core/texture/`: codec BC1..BC5 e DDS <-> RGBA.
- `src/core/recipe/`: ricette stagionali per classe (dati in `data/recipes/`).
- `src/core/season/`: motore delle stagioni (calendario, stadi, meteo, neve, luce).
- `src/core/pack/` e `src/core/gen/`: formato pacchetto `.qsp` e pipeline del generatore.
- `src/cli/qs_gen.cpp`: il generatore (anche `.exe` Windows).
- `src/win/`: plugin (io_hooks, overlay, probes).
- `data/`: inventario e classi (solo percorsi e numeri, mai dati del gioco).

## 9. Vincolo critico: dimensione esatta

Gli indici del gioco (`.pamt`, header DDS, `.pathc`) non si toccano, quindi ogni texture sostitutiva
deve avere ESATTAMENTE la stessa dimensione salvata dell'originale, mip per mip (le prime 4 mip sono
blocchi LZ4 di dimensione fissa, le altre sono raw). L'encoder LZ4 esatto (`lz4_exact`) allunga un
blocco fino alla misura voluta, ma non può accorciarlo: se la texture trasformata si comprime peggio
dell'originale serve un "fitter" che renda i dati BC più comprimibili (meno rumore negli indici,
endpoint quantizzati) fino a rientrare. Il tasso di riuscita del fitter è una metrica chiave.

## 10. Spazio disco

Un set completo non estivo è circa 1,2 GB (recon A). Con 4 look + 5 transizioni da 3 stadi sarebbero
19 set: troppo. Strategia: il generatore produce solo lo stadio corrente e il prossimo (cache con
limite in GB configurabile) e il plugin chiede in background il successivo quando serve.
