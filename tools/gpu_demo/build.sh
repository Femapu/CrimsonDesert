#!/usr/bin/env bash
# Builds the GPU live swap demo (Windows x64 exe, mingw-w64). Run ./build.sh first for build/QuattroStagioni.asi.
#   tools/gpu_demo/build.sh          -> build/win/gpu_demo.exe
#   tools/gpu_demo/build.sh run      -> also runs it with wine on a private Xvfb display (headless) and
#                                       prints the checks; exit code 0 = all passed. The plugin log is
#                                       build/QuattroStagioni.log (next to the ASI).
set -euo pipefail
cd "$(dirname "$0")/../.."
mkdir -p build/win
x86_64-w64-mingw32-g++ -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type \
  -D_WIN32_WINNT=0x0A00 -DWIDL_EXPLICIT_AGGREGATE_RETURNS -DNOMINMAX -Isrc/core -Ithird_party/lz4 \
  tools/gpu_demo/gpu_demo.cpp src/core/gpu/fingerprint.cpp src/core/pack/qsp_io.cpp src/core/pack/byte_source.cpp \
  src/core/lookup3.cpp src/core/dds.cpp third_party/lz4/lz4.c \
  -o build/win/gpu_demo.exe -ld3d12 -ldxgi -static -static-libgcc -static-libstdc++ -mwindows -Wl,--subsystem,console
echo "built build/win/gpu_demo.exe"

if [[ "${1:-}" == "run" ]]; then
  export WINEDEBUG=-all
  Xvfb :78 -screen 0 1024x768x24 >/dev/null 2>&1 &
  xpid=$!
  sleep 1
  rc=0
  (cd build && DISPLAY=:78 timeout 300 wine win/gpu_demo.exe QuattroStagioni.asi "${@:2}") || rc=$?
  wineserver -k 2>/dev/null || true
  kill $xpid 2>/dev/null || true
  echo "gpu_demo exit code $rc"
  exit $rc
fi
