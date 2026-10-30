# Bug trovati nei test locali

Bug emersi provando la mod sul PC di Stefano con i file veri del gioco (branch `local/test-in-gioco`).
Dettagli dei test in `docs/recon/test_locali.md`. Mai incollare qui dati del gioco (hash, offset,
estratti).

## B1. Inventario v1 non allineato al hotfix del 30/10 (aperto)

- **Dove**: `data/vegetation_classes_v1.json` (generato con `tools/classify_inventory.py`).
- **Sintomo**: `qs_gen --verify-inventory` esce con codice 3: 2 voci su 3688 con "index mismatch",
  entrambe texture d'erba del terreno nel gruppo 0000 (`pa_terrain_grass_0086.dds`,
  `pa_terrain_grass_0095.dds`). Cambia solo l'offset nel paz; dimensioni, flag e decodifica sono ok.
- **Causa probabile**: l'inventario è stato costruito sull'indice della 2.04.00; il hotfix del 30/10
  ha riscritto `0000/0.pamt` e spostato queste due voci.
- **Effetto**: non bloccante. Il generatore salta le voci con indice diverso (restano vanilla), quindi
  in gioco queste due erbe non cambierebbero stagione.
- **Correzione proposta**: rigenerare l'inventario dall'indice attuale (o aggiornare i due offset) e
  far rileggere al generatore l'offset dal `0.pamt` vivo quando il resto della voce coincide, così una
  patch che sposta solo i dati non scarta le voci.

## B2. Normal map nell'inventario con profilo di colore (aperto)

- **Dove**: `data/vegetation_classes_v1.json`, regole di `tools/classify_inventory.py`.
- **Sintomo**: `leaf_pine_common_b_01_normal.dds` e `leaf_pine_common_b_02_normal.dds` hanno
  profilo `deciduous` (ruolo colore). Nell'anteprima la ricetta altera la normal map (puntini gialli
  e chiari in autunno e inverno).
- **Effetto**: in gioco l'illuminazione di quelle foglie sarebbe sbagliata. Sono le uniche 2 voci con
  suffisso da mappa non di colore (`_normal`, `_n`, `_sp`, `_mask`, `_height`).
- **Correzione proposta**: escluderle nel classificatore con una regola sul suffisso, e come rete di
  sicurezza far rifiutare al generatore le voci il cui nome indica un ruolo non di colore.

## B3. Stadio senza pacchetto: resta attivo il pacchetto precedente, ma il log dice "vanilla" (aperto)

- **Dove**: `src/win/season_tick.*` (scelta del pacchetto per stadio) e caricamento in `src/win/io_hooks.*`.
- **Sintomo** (sessione del 30/10, solo pacchetto `A.qsp` presente):
  - all'avvio `Active=A` carica A, ma 0,6 s dopo il calendario (giorno 0, Primavera) chiede lo
    stadio S: `S.qsp refused: cannot open ... (the previous pack stays active)` seguito da
    `season: no pack for stage S ...: vanilla textures`. In realtà A resta attivo e continua a
    servire texture (oltre 2400 record nei due minuti successivi): in Primavera si vede l'autunno;
  - stessa cosa passando da Autunno a Inverno dal menu: `W.qsp` non c'è, il log dice "vanilla
    textures" ma il riepilogo successivo mostra ancora `pack A` con letture servite.
  - Da Estate a Inverno invece resta "none", perché il precedente era già nessuno.
- **Effetto**: con pacchetti mancanti (installazione parziale, o generati solo alcuni stadi) il
  paesaggio mostra la stagione sbagliata, e il log descrive il contrario di ciò che succede.
  Inoltre `Active` nell'ini viene scavalcato subito dal calendario, quindi non serve a forzare uno
  stadio come dice il commento.
- **Correzione proposta**: decidere un comportamento unico (consiglio: stadio senza pacchetto ->
  nessun pacchetto, cioè vanilla, oppure il pacchetto disponibile più vicino nel ciclo, scritto
  chiaramente nel log) e far rispettare `Active` finché il calendario non cambia stadio o finché
  l'utente non sceglie dal menu.
