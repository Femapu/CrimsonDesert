# QuattroStagioni (fase 2: fondamenta)

Mod personale single-player per Crimson Desert 2.04.00 (Steam, con DLC "Charting the Unknown").
La fase 2 dimostra che si puo sostituire una texture in gioco in modo sicuro: nessun file del gioco
viene modificato (ne su disco ne in memoria: .pamt, .papgt, .pathc, .paver restano intatti).

## Cosa fa

- Agganciandosi solo ad API Win32 (CreateFile*, ReadFile*, GetOverlappedResult*, GetQueuedCompletionStatus*,
  CloseHandle, CreateFileMapping*), osserva le letture degli archivi `.paz`.
- Quando il gioco legge il range di `ui/texture/cd_loading_00.dds` in `0012\3.paz`
  (offset 24565408, 30119 byte), sostituisce nel buffer del gioco quei byte con `test.bin`:
  stessa dimensione, stesso header DDS (DXT5 512x512, 1 mip), blocco LZ4 ricodificato alla stessa
  dimensione esatta, quindi indici e .pathc restano validi.
- Effetto visibile: il logo rotante di caricamento in basso a destra (schermata del titolo e schermate
  di caricamento) diventa arancione autunnale con un anello arancione.
- Controllo versione all'avvio (meta\0.paver, dimensione e timestamp di CrimsonDesert.exe, hash di
  0012\0.pamt, record dell'entry, hash dei byte originali). Se qualcosa non torna: nessun hook, il gioco
  gira normale. Qualsiasi errore interno: pass-through.

## Installazione (cartella del gioco `...\Crimson Desert\bin64`)

1. Copiare il contenuto di `dist\bin64\scripts\` in `bin64\scripts\` (creare la cartella):
   - `QuattroStagioni.asi`, `QuattroStagioni.ini`
   - `QuattroStagioni\manifest.ini`, `QuattroStagioni\test.bin`, `QuattroStagioni\THIRD_PARTY.txt`
2. Sostituire `bin64\winmm.ini` con `dist\bin64\winmm.ini`:
   `LoadPlugins=1` e `LoadFromScriptsOnly=1` fanno caricare a Ultimate ASI Loader (9.7.4) solo gli
   .asi in `scripts\` (e `plugins\`), quindi `Seasons.asi` nella radice di bin64 NON viene caricato.
3. Avviare il gioco e guardare il logo di caricamento sulla schermata del titolo.

Verifica che Seasons non sia partito: `bin64\Seasons\Seasons.log` non deve avere una data nuova.

## Disinstallazione

- Rapida: `Enabled=0` in `QuattroStagioni.ini` (nessun hook), oppure `LoadPlugins=0` in `winmm.ini`
  (contenuto originale: `LoadPlugins=0`, `LoadFromScriptsOnly=0`, `FindModule=0`).
- Completa: cancellare `bin64\scripts\QuattroStagioni*` e ripristinare `winmm.ini`.
  Non c'e nulla da riparare: i file del gioco non vengono mai scritti.

## Leggere il log (`bin64\scripts\QuattroStagioni.log`, run precedente in `.previous.log`)

In caso di successo, in ordine:

- `version check: meta/0.paver OK` e `version check: exe size 333779352 timestamp 1793246886 ... OK`
- `[cd_loading_00] strict decode OK ...` e `[cd_loading_00] OK ui/texture/cd_loading_00.dds -> 0012/3.paz ...`
- `hooked KernelBase!ReadFile ...` (una riga per API) e `startup complete, hooks active`
- `open TARGET archive 0012/3.paz ... [overlapped]` (o `[synchronous]`)
- `target read issued: 0012/3.paz [..., +...) via ReadFile ...`
- `HIT ui/texture/cd_loading_00.dds: read [...] via ... -> patched` (una o piu righe)
- ogni 30 s un `summary` con letture per archivio e API usate; per il bersaglio
  `target reads N, patched N, ... mismatch 0`.

Problemi tipici:
- `version guard failed`: gioco aggiornato, il plugin si e disattivato da solo.
- `MISMATCH(untouched)` o `original bytes hash ... entry disabled`: dati diversi da quelli attesi, nessuna modifica.
- `memory-mapped access is NOT substituted`: il gioco legge quell'archivio via mapping (non gestito in fase 2).
- Nessuna riga `open TARGET archive`: il gioco ha aperto l'archivio prima del plugin o con un'API non agganciata;
  impostare `LogLevel=2` e inviare il log.

## Struttura del progetto

- `src/core/` logica indipendente dalla piattaforma (lookup3, parser .pamt 2.04, patch di range, encoder LZ4
  a dimensione esatta, validatore DDS per-mip, ini/manifest); `src/win/` hook, log, controlli di avvio.
- `tools/` script Python offline: `index_browser.py` (cerca/estrae entry), `build_replacement.py`
  (genera test.bin e manifest), `verify_replacement.py` (verifica completa), `cdformats.py`, `bc3.py`.
- `tests/` test nativi (`test_core`) e harness Windows (`harness_win.exe`, eseguito sotto wine).
- `build.sh` compila tutto (`./build.sh test` esegue anche i test). Licenze: `THIRD_PARTY.txt`.

Candidato di riserva: `ui/texture/cd_uitexture_loading_00.dds` (stesso archivio `0012\3.paz`, offset
28527904, 35669 byte, DXT5 512x512 1 mip, mip0 LZ4 35541 byte, hash 7030F8ED/28BB1E0F): animazione
di caricamento usata nella barra di compilazione shader del titolo e nelle schermate di caricamento.
