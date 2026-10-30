#!/usr/bin/env bash
# Builds the overlay demo (Windows x64 exe, mingw-w64). Run ./build.sh first for build/QuattroStagioni.asi.
#   tools/overlay_demo/build.sh            -> build/win/overlay_demo.exe
#   tools/overlay_demo/build.sh run        -> also runs it with wine (needs a Vulkan driver for vkd3d)
#   tools/overlay_demo/build.sh shot FILE  -> headless run on Xvfb, menu opened, screenshot to FILE (png)
set -euo pipefail
cd "$(dirname "$0")/../.."
mkdir -p build/win
x86_64-w64-mingw32-g++ -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type \
  -D_WIN32_WINNT=0x0A00 -DWIDL_EXPLICIT_AGGREGATE_RETURNS -DNOMINMAX \
  tools/overlay_demo/overlay_demo.cpp -o build/win/overlay_demo.exe -ld3d12 -ldxgi \
  -static -static-libgcc -static-libstdc++ -mwindows -Wl,--subsystem,console
echo "built build/win/overlay_demo.exe"

case "${1:-}" in
  run)
    wine build/win/overlay_demo.exe build/QuattroStagioni.asi "${@:2}"
    ;;
  shot)
    out="${2:-build/menu_f5.png}"
    shift 2 || true
    export WINEDEBUG=-all
    # Segoe UI is not in a wine prefix: use Carlito (metric clone of Calibri) as the closest stand-in
    fdir="${WINEPREFIX:-$HOME/.wine}/drive_c/windows/Fonts"
    for f in Carlito-Regular.ttf Carlito-Bold.ttf; do
      [[ -f /usr/share/fonts/truetype/crosextra/$f && -d $fdir ]] && cp -n /usr/share/fonts/truetype/crosextra/$f "$fdir/" || true
    done
    Xvfb :77 -screen 0 1600x900x24 >/dev/null 2>&1 &
    xpid=$!
    sleep 1
    DISPLAY=:77 wine build/win/overlay_demo.exe build/QuattroStagioni.asi --open --frames 100000 "$@" &
    wpid=$!
    sleep "${SHOT_DELAY:-25}"
    DISPLAY=:77 import -window root "$out"
    kill $wpid 2>/dev/null || true
    wineserver -k 2>/dev/null || true
    kill $xpid 2>/dev/null || true
    echo "screenshot: $out"
    ;;
esac
