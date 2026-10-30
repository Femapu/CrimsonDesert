#!/usr/bin/env bash
# QuattroStagioni build script (Linux host).
#   ./build.sh          -> native tools + unit tests, then the Windows ASI (needs mingw-w64)
#   ./build.sh test     -> also runs the unit tests
# Outputs: build/qs_lz4exact, build/test_core, build/QuattroStagioni.asi
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p build

CORE="src/core/lookup3.cpp src/core/pamt.cpp src/core/range_patch.cpp src/core/lz4_exact.cpp src/core/ini.cpp src/core/manifest.cpp src/core/dds.cpp"
LZ4="third_party/lz4/lz4.c third_party/lz4/lz4hc.c"
WARN="-Wall -Wextra -Wno-unused-parameter"

echo "== native (Linux) core, CLI and tests"
gcc -O2 -c third_party/lz4/lz4.c -o build/lz4.o
gcc -O2 -c third_party/lz4/lz4hc.c -o build/lz4hc.o
g++ -std=c++17 -O2 $WARN -Ithird_party/lz4 $CORE src/cli/qs_lz4exact.cpp build/lz4.o build/lz4hc.o -o build/qs_lz4exact
g++ -std=c++17 -O2 $WARN -Ithird_party/lz4 $CORE tests/test_core.cpp build/lz4.o build/lz4hc.o -o build/test_core

echo "== Windows x64 ASI (mingw-w64)"
CXX=x86_64-w64-mingw32-g++
CC=x86_64-w64-mingw32-gcc
DEFS="-D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DWIN32_LEAN_AND_MEAN -DNOMINMAX"
MH="third_party/minhook/src/hook.c third_party/minhook/src/buffer.c third_party/minhook/src/trampoline.c third_party/minhook/src/hde/hde64.c"
OBJS=()
for f in $MH $LZ4; do
  o="build/w_$(basename "${f%.c}").o"
  $CC -O2 $DEFS -Ithird_party/minhook/include -c "$f" -o "$o"
  OBJS+=("$o")
done
$CXX -std=c++17 -O2 $WARN $DEFS -Ithird_party/minhook/include -Ithird_party/lz4 \
  -shared -o build/QuattroStagioni.asi \
  src/win/main.cpp src/win/log.cpp src/win/winutil.cpp src/win/setup.cpp src/win/io_hooks.cpp $CORE "${OBJS[@]}" \
  -static -static-libgcc -static-libstdc++ -s \
  -Wl,--exclude-all-symbols -Wl,--dynamicbase -Wl,--nxcompat -Wl,--high-entropy-va

# Windows test harness (runs under wine if available)
$CXX -std=c++17 -O2 $WARN $DEFS tests/harness_win.cpp -municode -o build/harness_win.exe -static -static-libgcc -static-libstdc++

echo "== imports of QuattroStagioni.asi"
x86_64-w64-mingw32-objdump -p build/QuattroStagioni.asi | grep -E "DLL Name|^\s+\[.*\] InitializeASI|Export" | sort -u || true

if [[ "${1:-}" == "test" ]]; then
  echo "== unit tests"
  ./build/test_core "${2:-tests/data}"
fi
