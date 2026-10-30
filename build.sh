#!/usr/bin/env bash
# QuattroStagioni build script (Linux host, needs g++ and mingw-w64).
#   ./build.sh          -> native tools + unit tests, Windows ASI, Windows CLI tools
#   ./build.sh test     -> also runs every unit test binary (tests/test_*.cpp)
#
# Sources are collected by folder, so a new module only has to drop files in the right place:
#   src/core/**.cpp        portable code (native + Windows), no Win32 headers
#   src/win/**.cpp         the ASI plugin (Windows only)
#   src/cli/<name>.cpp     one command line tool per file, plain main() (native build/<name> and build/win/<name>.exe)
#   tests/test_<name>.cpp  one native unit test binary per file, run with the test data dir as argv[1]
#   third_party/imgui      compiled into the ASI when present (core + win32/dx12 backends)
# Extra flags for a module go in the EXTRA_* variables below (keep them grouped by module).
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p build build/win

# ---------------------------------------------------------------- module extras
EXTRA_CORE_FLAGS=""        # e.g. -Ithird_party/<lib>
EXTRA_WIN_FLAGS=""
EXTRA_WIN_LIBS=""          # e.g. -ld3dcompiler (prefer runtime loading to keep imports minimal)

# ---------------------------------------------------------------- sources
mapfile -t CORE < <(find src/core -name '*.cpp' | sort)
mapfile -t WIN < <(find src/win -name '*.cpp' | sort)
mapfile -t CLIS < <(find src/cli -maxdepth 1 -name '*.cpp' | sort)
mapfile -t TESTS < <(find tests -maxdepth 1 -name 'test_*.cpp' | sort)
LZ4="third_party/lz4/lz4.c third_party/lz4/lz4hc.c"
MH="third_party/minhook/src/hook.c third_party/minhook/src/buffer.c third_party/minhook/src/trampoline.c third_party/minhook/src/hde/hde64.c"
IMGUI=()
if [[ -f third_party/imgui/imgui.cpp ]]; then
  IMGUI=(third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp third_party/imgui/imgui_tables.cpp
         third_party/imgui/imgui_widgets.cpp third_party/imgui/backends/imgui_impl_win32.cpp
         third_party/imgui/backends/imgui_impl_dx12.cpp)
  EXTRA_WIN_FLAGS+=" -Ithird_party/imgui -Ithird_party/imgui/backends"
fi
WARN="-Wall -Wextra -Wno-unused-parameter"
INC="-Isrc/core -Ithird_party/lz4 $EXTRA_CORE_FLAGS"

echo "== native (Linux) core, CLI tools and tests"
gcc -O2 -c third_party/lz4/lz4.c -o build/lz4.o
gcc -O2 -c third_party/lz4/lz4hc.c -o build/lz4hc.o
# compile core once into an archive (objects named after their path to avoid collisions)
rm -f build/core.a build/core_*.o
for f in "${CORE[@]}"; do
  o="build/core_$(echo "${f#src/core/}" | tr '/' '_' | sed 's/\.cpp$/.o/')"
  g++ -std=c++17 -O2 $WARN $INC -c "$f" -o "$o"
done
ar rcs build/core.a build/core_*.o build/lz4.o build/lz4hc.o
for f in "${CLIS[@]}"; do
  n=$(basename "${f%.cpp}")
  g++ -std=c++17 -O2 $WARN $INC "$f" build/core.a -o "build/$n" -lpthread
done
for f in "${TESTS[@]}"; do
  n=$(basename "${f%.cpp}")
  g++ -std=c++17 -O2 $WARN $INC "$f" build/core.a -o "build/$n" -lpthread
done

echo "== Windows x64 (mingw-w64)"
CXX=x86_64-w64-mingw32-g++
CC=x86_64-w64-mingw32-gcc
DEFS="-D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DWIN32_LEAN_AND_MEAN -DNOMINMAX"
WSTATIC="-static -static-libgcc -static-libstdc++"
COBJS=()
for f in $MH $LZ4; do
  o="build/w_$(basename "${f%.c}").o"
  $CC -O2 $DEFS -Ithird_party/minhook/include -c "$f" -o "$o"
  COBJS+=("$o")
done
$CXX -std=c++17 -O2 $WARN $DEFS -Ithird_party/minhook/include $INC $EXTRA_WIN_FLAGS -Isrc/win \
  -shared -o build/QuattroStagioni.asi \
  "${WIN[@]}" "${CORE[@]}" "${IMGUI[@]}" "${COBJS[@]}" $EXTRA_WIN_LIBS \
  $WSTATIC -s -Wl,--exclude-all-symbols -Wl,--dynamicbase -Wl,--nxcompat -Wl,--high-entropy-va
for f in "${CLIS[@]}"; do
  n=$(basename "${f%.cpp}")
  $CXX -std=c++17 -O2 $WARN $DEFS $INC "$f" "${CORE[@]}" build/w_lz4.o build/w_lz4hc.o \
    -o "build/win/$n.exe" $WSTATIC -s
done

# Windows test harness (runs under wine if available)
$CXX -std=c++17 -O2 $WARN $DEFS tests/harness_win.cpp -municode -o build/harness_win.exe $WSTATIC

echo "== imports of QuattroStagioni.asi"
x86_64-w64-mingw32-objdump -p build/QuattroStagioni.asi | grep -E "DLL Name" | sort -u || true

if [[ "${1:-}" == "test" ]]; then
  echo "== unit tests"
  rc=0
  for f in "${TESTS[@]}"; do
    n=$(basename "${f%.cpp}")
    echo "-- $n"
    ./build/"$n" "${2:-tests/data}" || rc=1
  done
  exit $rc
fi
