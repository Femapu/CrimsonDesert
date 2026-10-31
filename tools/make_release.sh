#!/usr/bin/env bash
# QuattroStagioni - assembles the end user release zip from build/ and dist/ (run ./build.sh first).
#
#   tools/make_release.sh [version] [out_dir]
#     version  default: the git tag on HEAD without the leading "v", else "<last tag or 0.0.0>-dev+<sha>"
#     out_dir  default: build/release
#   env ULTIMATE_ASI_LOADER=<path to Ultimate ASI Loader x64 dll>  optional: shipped as loader/winmm.dll
#       (MIT, github.com/ThirteenAG/Ultimate-ASI-Loader); qs_install copies it when bin64 has no loader.
#   env ULTIMATE_ASI_LOADER_LICENSE=<path to its license text>      required together with the dll
#
# Result: <out_dir>/QuattroStagioni-<version>/ (unpacked tree) and <out_dir>/QuattroStagioni-<version>.zip
# Layout (what qs_install expects next to itself):
#   qs_install.exe qs_gen.exe qs_preview.exe qs_season.exe   VERSION SHA256SUMS LEGGIMI.txt THIRD_PARTY.txt
#   controlla.bat genera_pacchetti.bat disinstalla.bat
#   bin64/winmm.ini (template)   bin64/scripts/QuattroStagioni.asi, QuattroStagioni.ini, QuattroStagioni/...
#   data/recipes/  data/vegetation_classes_v1.json   (qs_gen reads them relative to the current folder)
#   docs/utente/  user guides (Italian), docs/GENERATORE.md, CHANGELOG.md
# Never packs game files or anything derived from them: the script refuses known game extensions.
set -euo pipefail
cd "$(dirname "$0")/.."

VERSION="${1:-}"
OUT="${2:-build/release}"
if [[ -z "$VERSION" ]]; then
  if tag=$(git describe --tags --exact-match 2>/dev/null); then
    VERSION="${tag#v}"
  else
    last=$(git describe --tags --abbrev=0 2>/dev/null || echo "v0.0.0")
    VERSION="${last#v}-dev+$(git rev-parse --short HEAD 2>/dev/null || echo local)"
  fi
fi
NAME="QuattroStagioni-$VERSION"
DEST="$OUT/$NAME"

need() { [[ -f "$1" ]] || { echo "make_release: missing $1 (run ./build.sh first)" >&2; exit 1; }; }
need build/QuattroStagioni.asi
for t in qs_install qs_gen qs_preview qs_season; do need "build/win/$t.exe"; done
need data/vegetation_classes_v1.json
need dist/bin64/scripts/QuattroStagioni.ini

rm -rf "$DEST" "$OUT/$NAME.zip"
mkdir -p "$DEST/bin64/scripts/QuattroStagioni" "$DEST/data"

# plugin and its configuration (manifest.ini is the phase 2 test and needs a game derived file: not shipped)
cp dist/bin64/winmm.ini "$DEST/bin64/winmm.ini"
cp build/QuattroStagioni.asi "$DEST/bin64/scripts/"
cp dist/bin64/scripts/QuattroStagioni.ini "$DEST/bin64/scripts/"
for f in dist/bin64/scripts/QuattroStagioni/*; do
  [[ "$(basename "$f")" == manifest.ini ]] && continue
  cp -r "$f" "$DEST/bin64/scripts/QuattroStagioni/"  # lang/ is a folder
done

# Windows tools (qs_lz4exact is a developer tool: not shipped)
for t in qs_install qs_gen qs_preview qs_season; do cp "build/win/$t.exe" "$DEST/"; done
cp tools/installer/*.bat "$DEST/"   # double-click helpers: controlla, genera_pacchetti, disinstalla

# generator data: recipes and inventory (paths and numbers only, no game data)
cp -r data/recipes "$DEST/data/recipes"
cp data/vegetation_classes_v1.json "$DEST/data/"

# documents
cp THIRD_PARTY.txt "$DEST/"
# same layout as the repository, so the links between the guides keep working
mkdir -p "$DEST/docs/utente" "$DEST/docs/img"
cp docs/utente/*.md "$DEST/docs/utente/"
cp docs/GENERATORE.md "$DEST/docs/"
cp CHANGELOG.md "$DEST/"
cp docs/img/menu_f5_live.png docs/img/menu_f5_tabs.png "$DEST/docs/img/" 2>/dev/null || true

# optional Ultimate ASI Loader (MIT)
if [[ -n "${ULTIMATE_ASI_LOADER:-}" ]]; then
  [[ -f "$ULTIMATE_ASI_LOADER" && -f "${ULTIMATE_ASI_LOADER_LICENSE:-}" ]] || {
    echo "make_release: ULTIMATE_ASI_LOADER needs ULTIMATE_ASI_LOADER_LICENSE (its MIT license text)" >&2; exit 1; }
  grep -q IsUltimateASILoader "$ULTIMATE_ASI_LOADER" || { echo "make_release: $ULTIMATE_ASI_LOADER is not Ultimate ASI Loader" >&2; exit 1; }
  mkdir -p "$DEST/loader"
  cp "$ULTIMATE_ASI_LOADER" "$DEST/loader/winmm.dll"
  cp "$ULTIMATE_ASI_LOADER_LICENSE" "$DEST/loader/LICENSE-Ultimate-ASI-Loader.txt"
fi

# development builds (no tag on HEAD, version "...-dev+sha", e.g. the CI artifact): the test kit for the
# in-game test session; never in a tagged release
if [[ "$VERSION" == *-dev* && -d dist/dev ]]; then
  mkdir -p "$DEST/sviluppo"
  cp dist/dev/* "$DEST/sviluppo/"
fi

cat > "$DEST/LEGGIMI.txt" <<EOF
QuattroStagioni $VERSION - stagioni dinamiche per Crimson Desert 2.04 (PC, Steam)

1. Leggi docs\\utente\\INSTALLAZIONE.md (passo per passo).
2. Doppio clic su qs_install.exe: trova il gioco, controlla versione e altre mod, installa.
   Solo controllo, senza modifiche:   controlla.bat     (oppure qs_install.exe --check)
   Disinstallazione completa:         disinstalla.bat   (oppure qs_install.exe --uninstall)
3. Doppio clic su genera_pacchetti.bat: crea le texture stagionali sul tuo PC (passo 5).
4. Avvia il gioco e premi F5.

Cosa fa: vegetazione e terreno cambiano con le stagioni (anche le piante già visibili), meteo
stagionale (estati limpide, autunni piovosi, inverni coperti e nevosi), neve d'inverno, temperature
stagionali, fiocchi più fitti quando nevica. Pronti ma ancora fermi: fulmini, luce e cielo per
stagione, alba e tramonto stagionali, foglie e pollini (la scheda Info del menu F5 dice lo stato).
Il deserto resta sempre come nel gioco originale.

Nessun file del gioco viene modificato. Nessun asset di Pearl Abyss è incluso: le texture
stagionali vengono create sul tuo PC dai tuoi file del gioco e restano lì.
Problemi: docs\\utente\\PROBLEMI.md   Domande: docs\\utente\\FAQ.md   Licenze: THIRD_PARTY.txt
EOF
printf '%s\r\n' "$VERSION" > "$DEST/VERSION"

# safety net: nothing that looks like game data may end up in a public release
if find "$DEST" -type f \( -iname '*.paz' -o -iname '*.pamt' -o -iname '*.papgt' -o -iname '*.pathc' -o -iname '*.paver' \
     -o -iname '*.dds' -o -iname '*.qsp' -o -iname '*.bin' -o -iname '*.raw' \) | grep -q .; then
  echo "make_release: game data found in the release tree, aborting" >&2
  exit 1
fi

# CRLF for the text files players open with Notepad
find "$DEST" -type f \( -name '*.txt' -o -name '*.ini' -o -name '*.md' -o -name '*.bat' \) -print0 | while IFS= read -r -d '' f; do
  sed -i 's/\r$//; s/$/\r/' "$f"
done

(cd "$DEST" && find . -type f ! -name SHA256SUMS -print0 | sort -z | xargs -0 sha256sum | sed 's#  \./#  #' > SHA256SUMS)
(cd "$OUT" && zip -qr -X "$NAME.zip" "$NAME")
echo "release: $OUT/$NAME.zip"
(cd "$OUT" && sha256sum "$NAME.zip")
