# Demo dell'overlay (menu F5 fuori dal gioco)

Piccolo programma Windows che carica `QuattroStagioni.asi` come farebbe Ultimate ASI Loader
(`LoadLibrary` + `InitializeASI`), avvia l'overlay con l'export `QuattroStagioniOverlayStart` e apre
una finestra D3D12 con un paesaggio finto. Serve a provare il menu senza avviare Crimson Desert.
Il controllo di versione del plugin fallisce (non c'è l'exe del gioco), quindi gli hook sui file
restano spenti: gira solo l'overlay.

## Compilare

```
./build.sh                       # crea build/QuattroStagioni.asi
tools/overlay_demo/build.sh      # crea build/win/overlay_demo.exe
```

## Usare su Windows

Copiare `overlay_demo.exe` e `QuattroStagioni.asi` nella stessa cartella e avviare
`overlay_demo.exe`. Dopo qualche secondo compare l'avviso in alto a destra; F5 apre e chiude il menu
(anche Esc lo chiude). Il log è `QuattroStagioni.log` accanto all'ASI.

Opzioni: `--open` (preme F5 da solo), `--frames N` (esce dopo N frame), `--click X,Y@F` (clic
simulato al frame F, per gli screenshot), `--late` (crea la swapchain prima dell'overlay, per provare
il percorso di riserva: l'aggancio avviene dopo 10 s se esiste già un device D3D12, altrimenti dopo 2 min;
la variabile d'ambiente `QS_OVERLAY_FALLBACK_MS` accorcia l'attesa). Sotto wine questo percorso si blocca dentro
`D3D12CreateDevice` (wined3d crea un contesto GL che su Xvfb non risponde), quindi va provato su Windows.

## Usare su Linux (wine + vkd3d + lavapipe)

Serve `wine` (9.x, il suo d3d12 usa vkd3d), un driver Vulkan software (`apt install mesa-vulkan-drivers`)
e, per gli screenshot senza schermo, `xvfb` e `imagemagick`:

```
tools/overlay_demo/build.sh run              # finestra normale
tools/overlay_demo/build.sh shot out.png     # Xvfb, menu aperto, screenshot dopo SHOT_DELAY secondi
```

In wine non c'è Segoe UI: lo script copia Carlito (metriche di Calibri) nel prefisso come sostituto.
`docs/img/menu_f5.png` e `docs/img/menu_f5_tabs.png` sono stati fatti così (lavapipe, 1600x900).
