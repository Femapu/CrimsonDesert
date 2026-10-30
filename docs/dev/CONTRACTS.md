# Contratti tra moduli e flussi di lavoro

Regola generale: ogni flusso lavora SOLO nelle sue cartelle. Gli header di contratto elencati qui
esistono già. Si possono AGGIUNGERE funzioni, ma non cambiare le firme esistenti senza annotarlo in
fondo a questo file (sezione "Modifiche ai contratti"). Build: `./build.sh test` deve restare verde.
Codice: C++17, namespace `qs::<modulo>`, stile dei file esistenti (commento d'intestazione che spiega
il perché, 4 spazi, nomi camelCase). Mai file del gioco nel repo (repo pubblico).

| Flusso | Cartelle | Header di contratto |
|---|---|---|
| texture + ricette | `src/core/texture/`, `src/core/recipe/`, `data/recipes/`, `tests/test_texture.cpp`, `tests/test_recipe.cpp`, `src/cli/qs_preview.cpp`, `third_party/<codec>` | `texture/texture.h`, `recipe/recipe.h` |
| generatore + pacchetti | `src/core/pack/`, `src/core/gen/`, `src/core/crypto/`, `src/cli/qs_gen.cpp`, `tests/test_gen.cpp`, `src/win/io_hooks.*`, `src/win/setup.*` | `pack/qsp.h` |
| motore stagioni | `src/core/season/`, `tests/test_season.cpp` | `season/season.h` |
| overlay + menu F5 | `src/win/overlay/`, `third_party/imgui/` | usa `season.h`, `src/win/qs_state.h` |
| probe runtime | `src/win/probe/`, `tools/sigscan.py`, `docs/recon/probe_plan.md` | - |
| integrazione (sessione principale) | `src/win/main.cpp`, `src/win/qs_state.*`, `build.sh`, `.github/`, `docs/`, `data/` | `qs_state.h` |

Solo l'integrazione modifica `src/win/main.cpp`: gli altri flussi espongono una funzione
`bool <modulo>Start(...)` / `void <modulo>Stop()` in un loro header e scrivono nel report finale
come va chiamata.

## Profili stagionali (data/vegetation_classes_v1.json, campo `profile`)

| Profilo | Look modificate | Comportamento atteso |
|---|---|---|
| deciduous | S L A W | S verde chiaro; L spento/giallastro; A giallo-arancio-rosso-ruggine per seme; W spoglio (alfa foglie verso 0, rami restano) |
| conifer | W | W più scuro, freddo, leggermente desaturato |
| evergreen | L W | variazioni leggere di saturazione |
| grass | S L A W | S verde fresco; L paglierino; A bruno oliva; W secco, brinato, più chiaro e desaturato |
| flower | S L A W | S fioritura piena (colori saturati); L sbiadito; A seccato; W quasi assente (alfa ridotta) |
| crop_cereal | S L A W | S verde giovane; L dorato; A stoppia; W terra nuda/ridotto |
| crop_veg | S L A W | S verde; L maturo; A ingiallito; W ridotto |
| shrub | S L A W | come deciduous ma più moderato, W non completamente spoglio |
| vine | S L A W | edera: A rossa (vite americana) per seme, W più scura |
| wetland | L A W | canne: L/A dorate, W secche |
| bark | W | W leggermente più scura e fredda |
| ground_grass, ground_decal | S L A W | come grass, sul terreno |
| ground_soil, moss_surface | L W | L più asciutto/chiaro; W più scuro, umido |
| leaf_litter | S L A W | A più foglie arancio |
| terrain_color | S L A W | tinta del terreno vista da lontano, come grass ma più delicata |
| global_colormap | S L A W | mappa colore globale della vegetazione: la leva più grande |
| climate_temperature | S L A W | canale R = temperatura: offset per stagione (G umidità, B vento invariati) |

Campi utili per riga: `path, group, paz, offset, stored, size, fmt, w, h, mips, mip_stored, role, cls,
profile, looks, tier, region, asset`. Le texture con lo stesso `asset` (colore, subsurface, impostor)
devono usare lo stesso seme: `seed = lookup3(asset, 0xC5EDE)`.

## Modifiche ai contratti
- `season.h` (motore stagioni, 30/10): nessuna firma cambiata, solo aggiunte (vedi
  `docs/dev/season_engine.md`). Valori predefiniti cambiati: `realMinutesPerDay` 48 -> 90 (un giorno
  del gioco dura circa 60-120 minuti reali), finestra del giorno vanilla 03:00-19:00 (prima 6-20).
  `update()` accetta `inGameHours < 0` (ore derivate dal giorno).
- 30/10, flusso texture + ricette: solo AGGIUNTE, nessuna firma esistente cambiata.
  `texture.h`: `blockBytes`, `surfaceSize`, `mipDim`, `decodeSurface`, `encodeSurface`, `encodeMipRef`,
  `MipOptions` + `rebuildMips(Texture&, const MipOptions&)`, `downsample`, `alphaCoverage`,
  `matchAlphaCoverage`, `recolorSurface`, `fitMip` (con `FitResult`, `RecolorFallback`), `lz4BestSize`.
  `recipe.h`: `RecipeBook::applyPalette`, `profiles`, `hasProfile`, `changesLook`, `pureStage`,
  `PaletteBinding` + `paletteThunk`, `seedForAsset`, `lookFromString`.
  Dettagli e uso per il generatore: `docs/dev/texture_recipes.md`.
