# Texture e ricette (flusso texture + ricette)

Stato al 30/10/2026. Codice in `src/core/texture/`, `src/core/recipe/`, dati in `data/recipes/`,
strumento `src/cli/qs_preview.cpp`, test `tests/test_texture.cpp` e `tests/test_recipe.cpp`.

![deciduous](../img/preview_deciduous.png)
![grass](../img/preview_grass.png)
![flower](../img/preview_flower.png)

(Anteprime da immagini SINTETICHE generate dal codice, nessun dato del gioco.)

## Codec (texture.h)

- Decodifica BC1, BC3, BC4, BC5, RGBA8, BGRA8, R8. In BC1 il selettore 3 dei blocchi a 3 colori è
  trasparente (alfa 0): il fogliame DXT1 usa l'alpha test.
- Codifica con rgbcx (MIT, Rich Geldreich) per i blocchi opachi e BC4/BC5, encoder nostro per i blocchi
  BC1 con trasparenza (la sagoma dell'alpha test resta identica: alfa < 128 diventa trasparente).
- `reference` (i blocchi originali della mip): ogni blocco i cui pixel non sono cambiati riprende i byte
  originali, così le zone non toccate (sfondo trasparente, corteccia) restano identiche anche per LZ4.
- Mip: filtro box in luce lineare (tabelle intere), colore pesato per l'alfa (i texel trasparenti neri
  del BC1 non scuriscono i bordi delle foglie), copertura alpha test preservata su ogni mip.
- Determinismo: solo operazioni IEEE di base, `-ffp-contract=off`, niente funzioni libm trascendenti
  (`src/core/texture/dmath.h`), parser numerico nostro. Verificato: stessa uscita byte per byte tra build
  Linux e build mingw eseguita con wine (anteprima con codifica BC3 e test di fit).

### fitBias e dimensione esatta

Il generatore deve salvare ogni mip LZ4 (mip 0..3) esattamente nella dimensione originale, e
`lz4CompressExact` può solo allungare un blocco. `fitBias` (0..10) attiva un passaggio rate/distortion
pensato per LZ4: un blocco può copiare un blocco recente (match da 8 byte), continuare il match del
blocco precedente (match più lungo, quasi gratis) o copiarne metà (stessi endpoint con selettori
ricalcolati, oppure stessi selettori con endpoint ai minimi quadrati). Errori interi, blocchi lisci
protetti (lì la perdita si vede prima).

Misure su immagini sintetiche 256x256 (LZ4-HC, PSNR sui texel visibili):

| immagine | formato | fitBias 0 | fitBias 10 | PSNR |
|---|---|---|---|---|
| foglie | BC1 | 10222 | 8503 (83%) | 40.1 -> 36.8 |
| foglie | BC3 | 12327 | 8415 (68%) | 40.6 -> 34.5 |
| erba | BC1 | 20338 | 12888 (63%) | 39.5 -> 34.6 |
| erba | BC3 | 23960 | 13598 (57%) | 40.0 -> 33.7 |
| terreno | BC1 | 24146 | 7505 (31%) | 44.2 -> 36.8 |

Test di fit simulato (512x512, originale = codifica semplice compressa con LZ4 standard): tutte le look
di foglie, erba, fiori e terreno rientrano già con fitBias 0, perché LZ4-HC comprime meglio del
compressore veloce. Caso peggiore (originale compresso con LZ4-HC): tutte rientrano con fitBias 0..5.
Ultima rete di sicurezza: `recolorSurface` (ricolora solo gli endpoint e tiene i selettori, quindi la
struttura LZ4 dell'originale resta quasi identica).

### Come lo usa il generatore

```cpp
qs::tex::Texture t;                       // decodeDds() del DDS originale (decompresso)
book.apply(ctx, stage, t.mips[0]);        // ricetta sulla mip 0
qs::tex::rebuildMips(t, true);            // sRGB per albedo/subsurface
qs::recipe::PaletteBinding pb{&book, ctx, stage};
qs::tex::RecolorFallback rf{&qs::recipe::paletteThunk, &pb};
for (size_t i = 0; i < t.mips.size(); ++i) {
    size_t target = i < 4 ? info.storedMip[i] : info.mipRawSize[i];
    qs::tex::FitResult r = qs::tex::fitMip(t, i, target, origMip[i], origMipSize[i], {}, &rf);
    if (!r.ok) { /* tenere i byte originali di tutta la texture, scrivere r.error nel log */ }
    // r.stored = byte da scrivere (blocco LZ4 di esattamente `target` byte, o mip raw)
}
```

`ctx.seed = qs::recipe::seedForAsset(asset)`; se `book.affects(ctx, stage)` è falso la texture resta
vanilla (byte originali). Le mip con dimensione salvata uguale a quella raw vengono restituite raw.

## Ricette (recipe.h, data/recipes/vegetation.ini)

Spazio colore OKLab/OKLCh, tre maschere morbide calcolate dal colore originale: `green` (fogliame),
`petal` (colori saturi non verdi), `other` (corteccia, terra, grigi). Ogni look ha parametri per gruppo;
le transizioni interpolano i parametri (con il colore che anticipa la caduta: le foglie diventano
marroni prima di cadere e tornano verdi mentre sono ancora rade). Il formato del file è descritto in
testa a `vegetation.ini`.

- Autunno latifoglie: la tinta del verde va verso la palette scelta dal seme (giallo, oro, arancio,
  rosso, ruggine), le parti scure virano più verso il rosso (`lum_hue`), corteccia quasi intatta.
- Inverno latifoglie: i pixel verdi diventano trasparenti. Gruppi di foglie connessi e piccoli (fino
  all'8% dell'immagine) cadono interi; chiome connesse più grandi cadono a chiazze (rumore con seme).
  Un pixel caduto resta caduto in tutti gli stadi successivi. Solo texture che usano già l'alfa.
- Erba: primavera fresca, fine estate paglierina, autunno bruno oliva, inverno grigia e brinata.
- climate_temperature: `mode = channels`, solo offset del canale R (valori da calibrare in gioco).

## Limiti noti

- Le maschere si basano solo sul colore: rami verdastri o muschio sui rami cadono con le foglie in
  inverno; fiori gialli molto vicini al verde oliva sono trattati in parte come foglie.
- Le texture di tipo impostor con chiome dense formano un unico grande gruppo connesso: lì la caduta è
  a chiazze, non per foglia.
- Gli offset di temperatura e le intensità sono tarati su immagini sintetiche: vanno rivisti con
  `qs_preview` sulle texture vere.
- Velocità (512x512 BC3): un fit che ha bisogno di fitBias alto fa più tentativi e richiede fino a circa 2 s; il caso tipico (fitBias 0) circa
  0,1 s.

## qs_preview sul PC

```
qs_preview --list
qs_preview foglia.dds --profile deciduous --seed 1,2,3,4 --stages 3 --encode --fit
qs_preview --synth leaves --stages 3
```

Accetta PNG e DDS (decompresso oppure nella forma salvata nel .paz, con le mip LZ4). Con `--fit` su un
DDS usa come obiettivo le dimensioni salvate scritte nell'header e stampa il fitBias necessario per
ogni look e mip: è la misura del tasso di riuscita del generatore sulle texture vere.
