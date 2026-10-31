# Cambio texture su GPU (stato al 31/10, branch `claude/gpu`)

Obiettivo: quando cambia lo stadio stagionale (calendario o F5), riscrivere anche le texture già
residenti in VRAM. Gli hook sui file (`src/win/io_hooks.*`) cambiano solo le texture che il gioco
legge dopo il cambio.

## Come funziona

- Approccio di Seasons of Pywel (`asi/src/gpu.cpp`, permesso di Khione95), adattato ai pacchetti `.qsp`
  e agli stadi per texture: si riconoscono le texture che il gioco carica dai byte dei mip nei suoi
  upload D3D12 e, al cambio di stadio, si ricaricano con i dati del nuovo pacchetto.
- Nucleo portabile `src/core/gpu/` (impronte, indice per stadio, tracker, scheduler, `live.*`:
  impostazioni `[GpuSwap]`, obiettivi per texture, controllo della destinazione), test `tests/test_gpu.cpp`.
- Parte Windows `src/win/gpu/gpu_swap.*`: hook solo su `d3d12.dll` (D3D12CreateDevice e vtable della
  command list: CopyTextureRegion, CopyResource, ResourceBarrier, Reset), mai sull'eseguibile.
- Impronte lette in ExecuteCommandLists (osservatore nell'hook dell'overlay), non in
  CopyTextureRegion: secondo le note di Seasons a quel punto il buffer di upload può essere ancora
  incompleto. Se l'hook non c'è si legge subito (riga di log `gpu: ExecuteCommandLists not observed`).
- Obiettivi: il worker segue `io::serveSnapshot()` (gli stadi per texture serviti dagli hook sui file).
- Caricamento: lotti registrati dal worker in un ring di upload, eseguiti nell'hook Present
  dell'overlay sulla coda DIRECT del gioco, al massimo `MaxMBPerFrame` per frame; nessuna attesa sulla GPU.
- Sicurezza: descrizione, formato, dimensioni e mip controllati prima di ogni copia; a qualunque errore
  D3D12, device rimosso o eccezione: `gpu: DISABLED: <motivo>` e il modulo resta spento per la sessione.

## Ini `[GpuSwap]` (chiavi facoltative)

`Enabled=1` (build di prova), `MaxMBPerFrame=4`, `LogLevel=1` (riepilogo ogni 30 s se qualcosa si
muove), più `RingMB`, `CacheMB`, `AssumeCommonState`, `PreloadAllPacks` (vedi `src/core/gpu/live.h`).

## Da provare in gioco (mai provato)

1. Log: `gpu: hooked ...`, `gpu: upload prints read at ExecuteCommandLists`, riepiloghi `gpu [periodic]`
   con `prints matched` maggiore di 0 e `tracked` che cresce.
2. F5 su un altro stadio: `pending` sale e torna a 0, `done` cresce, la vegetazione visibile cambia
   senza viaggio rapido; nessun calo marcato di frame (eventualmente `MaxMBPerFrame=2`).
3. Con frame generation XeSS accesa e spenta. In caso di problemi: `Enabled=0`.

Rischi: ordine delle code (upload del gioco più recente del nostro: gestito con il ricaricamento
ritardato), DirectStorage (upload non visibili: contatore `gpu-only source`), memoria (ring e cache).
`tools/gpu_demo/`: demo D3D12 per wine, mai eseguita.
