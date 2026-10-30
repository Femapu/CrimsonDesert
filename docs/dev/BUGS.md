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
