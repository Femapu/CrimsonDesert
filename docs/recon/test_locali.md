# Test locali sul PC di Stefano (branch `local/test-in-gioco`)

Risultati dei passi 1-5 di "Prossimi passi" in `HANDOFF.md`, eseguiti con i file veri del gioco.
Qui vanno solo esiti, conteggi e osservazioni: mai hash, offset, estratti o anteprime dei file del
gioco (repo pubblico). I bug trovati vanno in `docs/dev/BUGS.md`.

## 0. Ambiente (30/10/2026)

- Gioco: Steam, `C:\Program Files (x86)\Steam\steamapps\common\Crimson Desert`, build Steam
  aggiornata il 30/10 alle 10:47. L'exe corrisponde al hotfix del 30/10 già in `kKnownExes`
  (stessa dimensione e timestamp), `meta/0.paver` = 2.04.00 con DLC `dlc_01`.
- Gli archivi del DLC stanno in `dlc/dlc_01/<gruppo>/` (visto nel log del plugin), non solo nelle
  cartelle numerate della radice: da tenere presente per la verifica dell'inventario.
- Cartelle archivio in radice: da 0000 a 0035, senza 0018 e 0034.
- Strumenti: Git 2.56, Python 3.12.10. Gli eseguibili arrivano dall'artifact CI `QuattroStagioni-dist`
  in `dist-ci/` (ignorata da git).

### Mod manager DMM (`Desktop\Desert Mods`), controllo in sola lettura

- `config.json`: nessuna mod di archivio attiva (`activeMods` vuoto), nessuna texture
  (`activeTextures` vuoto). Unica ASI attiva: `Seasons` (il vecchio `Seasons.asi`, in `bin64\`).
- Ultima attività di DMM: 4/10. Il suo backup "vanilla" dei `.pamt` è di quella data, quindi
  precede le patch del 29-30/10: tutti i 37 file (pamt e meta) risultano diversi dal backup, ma
  hanno tutti data di modifica 29/10 sera o 30/10 mattina, cioè gli aggiornamenti Steam. Non sono
  modifiche di DMM.
- `winmm.ini` ha `LoadFromScriptsOnly=1`, quindi `bin64\Seasons.asi` non viene caricato.
- Conclusione: DMM non altera archivi né texture; non serve disattivarlo per i test. Per una
  conferma definitiva basta "Verifica integrità dei file" di Steam prima del passo 1.

### Stato di `bin64\scripts` trovato all'inizio

- È installato `QuattroStagioni.asi` 0.2.0 (fase 2, sostituzione della sola texture di test del
  logo di caricamento), installato il 30/10 alle 10:50.
- Log del 30/10: il controllo di versione è passato con l'exe del hotfix, gli hook sono stati
  installati in circa 170 ms e la texture di test è stata servita correttamente (HIT, patch in
  0,004 ms). Prima del riavvio, con l'elenco versioni vecchio, il plugin si era spento da solo
  come previsto (controllo di versione: MISMATCH).
- Al passo 3 questa installazione va sostituita con quella dell'artifact CI.

## 1. Verifica inventario (30/10/2026)

Strumenti: artifact CI `QuattroStagioni-dist` della build 9 (commit `7666317`), sha256 dello zip
uguale al digest pubblicato da GitHub. Verifica d'integrità di Steam non eseguita (scelta di
Stefano): la base è il gioco così com'è dopo il hotfix del 30/10.

Comando: `qs_gen.exe --game "<Crimson Desert>" --verify-inventory --report local-out\verify.txt`
(report e output restano in `local-out/`, ignorata da git).

| Voce | Esito |
|---|---|
| Voci controllate | 3688 |
| OK (indice, decodifica stretta, header DDS) | 3686 |
| Mancanti | 0 |
| Indice diverso | 2, solo l'offset nel paz (vedi BUGS.md, B1) |
| Errori di decodifica / header diversi | 0 / 0 |
| Coperte dal DLC (shadowed) / errori di gruppo | 0 / 0 |
| Tempo | 3 s (circa 980 MB letti, decodifica completa) |

Le due voci con offset diverso sono texture d'erba del terreno nel gruppo 0000. Il generatore le
salta (restano vanilla), quindi non è bloccante, ma l'inventario va aggiornato al hotfix.

### DLC e gruppi

- Tutti gli indici (`0.pamt`) della radice e di `dlc/dlc_01` hanno l'hash interno corretto.
- In radice mancano 0018 e 0034: nessuna voce dell'inventario li usa, nessun errore di gruppo.
- Il DLC ha 40 gruppi (`dlc/dlc_01/0000`-`0040`, alcuni vuoti) con circa 76.800 texture DDS:
  - circa 75.500 sono tile di settore del terreno in `leveldata/rootlevel` (colore, normali,
    speculare);
  - circa 750 in `character/texture`, circa 520 in `ui/texture`;
  - solo 34 in `object/texture`, tutte di oggetti e arredi (decorazioni, statue, gimmick), nessuna
    pianta.
- Cercando nomi di vegetazione (tree, grass, leaf, bush, plant, flower, moss, ivy, fern, reed, crop)
  nel DLC risultano solo 3 texture di alghe (`leaf_seaweed`, personaggi). **Il DLC non ha vegetazione
  propria**: conferma l'inventario v0. Le piante della regione del DLC riusano quelle del gioco base.
- Domanda aperta per la sessione cloud: i tile di settore del DLC possono contenere colore del
  terreno (prati) della nuova regione; oggi l'inventario include i tile `terrain` del gioco base ma
  non quelli del DLC.

## 2. Anteprime vere (30/10/2026)

Campione: 3 texture a caso (seme fisso) per ognuno dei 19 profili, 52 texture distinte, estratte
con `tools/index_browser.py extract`. Per ognuna `qs_preview --encode --fit --stages 1` con il seme
dell'asset. Texture, PNG e log restano in `local-out/` (script: `local-out/scripts/sample_previews.py`).

### Tasso di riuscita del fitter

- 52 texture su 52 elaborate, nessun errore di estrazione o di decodifica.
- **Nessun mip oltre la dimensione originale**: 91 mip ricompressi LZ4 tutti dentro il limite al
  primo tentativo (`fb=0`, mai servito il fitBias), gli altri sono mip salvati non compressi e
  rientrano per costruzione. Margine tipico tra il 10% e il 60% sotto la dimensione originale.
- Tempi: 80-370 ms per mip 1024, circa 1,2 s per il mip0 2048 della `global_colormap`.

### Osservazioni visive (prime 5 anteprime guardate)

- `deciduous` (impostor di betulla): inverno spoglio convincente (rami nudi, foglie tolte con
  l'alfa), autunno diverso per seme (seme 1 rosso, seme 10 giallo oliva spento). Primavera quasi
  identica all'estate e fine estate poco distinta: da tarare.
- `ground_grass` e `terrain_color` (erba del terreno): progressione buona, fine estate paglierina,
  autunno oliva scuro, inverno grigio slavato.
- Uno dei 3 tile `terrain_color` è quasi tutto nero già in vanilla (tile fuori mappa o di mare):
  lavoro e spazio sprecati, da escludere nell'inventario se sono tanti.
- `grass` con `weed_salvia_03` (salvia viola): nessun cambiamento visibile in nessuna stagione, la
  ricetta dell'erba non tocca i toni viola. Probabilmente va nel profilo `flower`.
- Una normal map classificata come colore: vedi BUGS.md, B2.

Taratura di `data/recipes/vegetation.ini`: in attesa del parere di Stefano sulla galleria
(`local-out/prev/index.html`).

## 3. Primo pacchetto in gioco (30/10/2026)

Installazione: contenuto di `dist-ci/bin64/scripts` in `bin64\scripts` (sha256 dell'asi verificato),
`winmm.ini` già identico, vecchia 0.2.0 salvata in `local-out/backup_0.2.0`. Nell'ini `Active=A`.
Restano `manifest.ini` e `test.bin` della fase 2 (logo di caricamento arancione), che funzionano ancora.

### Generazione dello stadio A

`qs_gen.exe --game ... --stage A --out "<bin64>\scripts\QuattroStagioni\packs"`, 43 s:

- 3688 voci, lo stadio cambia 3058 texture; 2963 a piena qualità, 8 con fitBias, 0 a forza
  ridotta, 0 lasciate vanilla per mancato fit: **tasso di fit 100%** (2971 su 2971 tentate).
- 630 non toccate dallo stadio, 85 identiche dopo la ricetta, 2 saltate per B1 (codice di uscita 3
  solo per queste).
- `A.qsp`: 2971 record in 20 archivi, 726 MB. `--check-pack`: tutto OK.

### Sessione di gioco (13:44-13:50, circa 5 minuti)

Dal log del plugin 0.3.0:

- Avvio in 41 ms: hook dei file, pacchetti, motore stagioni e overlay attivi; probe spenti.
  Riconosciuti Streamline, XeSS frame generation, overlay di Steam e l'Ultimate ASI Loader.
- Pacchetto A verificato in circa 10 ms a ogni attivazione.
- Menu F5: usato più volte (Autunno, Estate, Inverno, Autunno, Inverno), il config viene salvato
  dal menu ogni volta (`DaySource = manual`, `StartDay = 29` nell'ini).
- Lettura delle texture: circa 5.000 record serviti dal pacchetto, **0 rifiutati, 0 errori di
  payload, 0 letture in sospeso**; patch di una lettura tra 0,01 e 0,4 ms. Nessun segno di
  rallentamento dello streaming nel log (`WaitForTargetReads=1`).
- Cache: satura a 64 MB nei primi due minuti (prima del passaggio ad Autunno), poi 18-27 MB.
- Passaggio a Estate: pacchetto "none", corretto. Il cambio vale solo per le texture caricate
  dopo (limite noto, HANDOFF passo 6).
- Bug sulla scelta del pacchetto quando quello dello stadio non esiste: vedi BUGS.md, B3.

Osservazioni visive di Stefano: in attesa.

## 4. Menu F5 in gioco

Da fare.

## 5. Probe (A1-b)

Da fare.
