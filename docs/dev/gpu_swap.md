# Cambio texture su GPU (stato)

Obiettivo: quando cambia lo stadio stagionale, riscrivere anche le texture già residenti in VRAM.
Oggi gli hook sui file (`src/win/io_hooks.*`) cambiano solo le texture che il gioco legge dopo il
cambio di stadio.

## Cosa è nel branch principale

- `src/core/gpu/` (portabile, nessun header Win32):
  - `fingerprint.*`: impronte dei mip (hash lookup3 su campioni dei blocchi BC) e footprint delle
    copie, per riconoscere una texture del gioco dai dati che carica;
  - `library.*`: indice degli stadi (pacchetti `.qsp` e dati originali) per impronta;
  - `swap_model.*`: tracker delle risorse, scheduler con budget per tick, gestione dei casi ambigui.
- `tests/test_gpu.cpp` e `tests/gpu_synth.h`: 142 controlli su dati sintetici, tutti verdi.

Il nucleo viene compilato nel plugin (come tutto `src/core/`) ma nessuno lo chiama ancora: non ha
effetto in gioco.

## Cosa resta sul branch `wip/gpu-swap`

- `src/win/gpu/gpu_swap.*`: hook su `d3d12.dll` (D3D12CreateDevice e vtable della command list:
  CopyTextureRegion, CopyResource, ResourceBarrier), mai sull'eseguibile del gioco; upload sulla coda
  DIRECT del gioco presa dall'overlay.
- `src/win/overlay/overlay.*`: `overlayAcquireGameQueue()`.
- `tools/gpu_demo/`: demo D3D12 per provarlo sotto wine.

Compila senza errori né avvisi con mingw-w64 (controllo del 30/10), ma il lavoro è stato interrotto
per budget: la demo non è mai stata eseguita e nulla è stato provato in gioco. Per questo non è
unito e non è collegato a `src/win/main.cpp`.

## Prossimi passi

1. Eseguire `tools/gpu_demo/` sotto wine con D3D12 software e controllare le statistiche
   (`QuattroStagioniGpuSwapStats`: copie viste, impronte trovate, job completati).
2. Unire `src/win/gpu/` e `overlayAcquireGameQueue()`, poi collegare in `main.cpp`:
   `gpuSwapStart()` dopo `packsStart()` e `overlayStart()`, `gpuSwapStop()` prima di `overlayStop()`.
3. Prova in gioco con un interruttore nell'ini, spento per default.

Rischi principali: ordine delle code (upload del gioco più recente del nostro), memoria (ring e
cache di decodifica), compatibilità con ReShade e frame generation.
