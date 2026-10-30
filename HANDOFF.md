# Handoff QuattroStagioni (30 ottobre 2026)

## Obiettivo
Mod pubblicabile di stagioni dinamiche per Crimson Desert 2.04 con DLC "Charting the Unknown".
Requisiti di Stefano (tutti):
1. Fase 3: estrazione fogliame e varianti stagionali (primavera, estate, fine estate secca, autunno, inverno spoglio).
2. Transizioni graduali (stadi intermedi nel corso dei giorni).
3. Varietà: fioriture, prati secchi, alberi spogli, differenze per regione (deserto escluso).
4. Meteo coerente con la stagione, neve che si accumula in inverno, estati limpide.
5. Menu F5 curato: calendario, stagione, lunghezza stagioni, opzioni per regione.
6. Installazione pulita con controllo di versione (dopo una patch la mod si spegne da sola).
7. Luce per stagione (sole più basso, toni freddi in inverno), se agganciabile.
8. Giornate più corte d'inverno, se agganciabile il ciclo giorno/notte.
9. Particelle: foglie che cadono, fiato visibile al freddo, riusando effetti del gioco.

## Architettura decisa (per la pubblicazione)
- La mod NON distribuisce texture di Pearl Abyss. Distribuisce: plugin ASI, manifest delle classi di texture, "ricette" (trasformazioni deterministiche per classe) e un GENERATORE in C++ che gira sul PC dell'utente finale, legge i suoi file del gioco e crea il pacchetto stagionale in locale (rigenerabile dopo le patch).
- Il plugin sostituisce texture intercettando le letture dei `.paz` (hook Win32 su KernelBase) con dati di dimensione IDENTICA (encoder LZ4 a dimensione esatta). Mai modificare o ri-hashare `.pamt`, `.papgt` (ha un trailer da 64 byte non ricreabile), `.pathc`, `.paver`.
- Cambio stagione in diretta: sostituzione texture lato GPU (hook D3D12), da fare.
- Sistemi di gioco: hook per firma (byte pattern), mai dentro la sezione Denuvo `.code`; vtable/RTTI in `.text1`.

## Stato
- FATTO: ricognizione (docs/recon/): formati 2.04 (lookup3 seed 0xC5EDE, pamt a trie, ChaCha20, LZ4, DDS per-mip), causa della rottura di Seasons, analisi exe (Denuvo, RTTI, controllo integrità all'avvio).
- FATTO: Fase 2, plugin base `QuattroStagioni.asi` (src/). Testato in gioco il 30/10: il log mostra "HIT ui/texture/cd_loading_00.dds ... patched", nessun errore. Conferma visiva (logo di caricamento arancione) ancora da vedere.
- Versioni exe riconosciute (src/win/setup.cpp, kKnownExes): 333779352/1793246886 (lancio 2.04.00) e 336264088/1793296370 (hotfix 30/10). Hash 0012/0.pamt dopo hotfix: 0x96C76FDD (in manifest).
- Installato sul PC: `bin64\scripts\QuattroStagioni.asi`, `.ini`, cartella `QuattroStagioni\` (manifest, test.bin, THIRD_PARTY) e `bin64\winmm.ini` con `LoadPlugins=1`, `LoadFromScriptsOnly=1` (così NON carica il vecchio `bin64\Seasons.asi`, rotto). Ultimate ASI Loader 9.7.4 come `winmm.dll`.
- INTERROTTO: tappa A1 (due agenti fermati per limite di utilizzo). `data/partial/` contiene output parziali NON verificati (inventario vegetazione, firme): rifare o verificare.

## Prossimi passi (fase A, a tappe con checkpoint di utilizzo)
- A1-a: inventario vegetazione (base + dlc/dlc_01) per classe e ricette v0 con anteprime; stime disco e tempi del generatore.
- A1-b: ricognizione dell'exe hotfix per giorno/ora, meteo, mappa clima (`texture/climate_texture_1.dds`), atmosfera/luce, durata giorno, particelle; firme in data/signatures_*.json e piano di "probe" runtime.
- A2: prototipo menu F5 (ImGui su D3D12, MIT) e cambio texture in diretta su GPU.
- Poi B (transizioni, varietà, calendario, meteo/neve), C (luce, durata giorno, particelle), D (pacchetto finale, test).

## Note pratiche
- Build: `./build.sh test` su Linux con mingw-w64 (crea build/QuattroStagioni.asi). I test che usano `tests/data` richiedono file del gioco non inclusi nel repo: rigenerarli dal PC.
- La texture di test (`test.bin`) è derivata da un asset del gioco: non è nel repo; si ricostruisce con `tools/build_replacement.py` partendo dall'entry originale letta dal PC.
- Accesso al PC di Stefano (cartella del gioco `C:\Program Files (x86)\Steam\steamapps\common\Crimson Desert`) solo se la sessione è collegata al suo computer. Attenzione: la consegna file sul PC ha restituito una volta copie in cache; inviare con un nome di origine nuovo e verificare lo sha256 sul PC dopo ogni installazione.
- Due patch in due giorni: dopo ogni aggiornamento ricontrollare dimensione/timestamp dell'exe e gli hash dei pamt usati.
