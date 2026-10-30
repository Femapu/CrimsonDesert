#!/usr/bin/env bash
# QuattroStagioni - end-to-end probe test: fake game (MSVC RTTI via clang-cl) + real probe code, under wine.
#   tests/probe_fake/run.sh        -> builds into build/probe_fake/, runs, prints "N passed, M failed"
# Needs: clang-cl (any recent LLVM), mingw-w64, wine (64-bit). Not part of ./build.sh (needs wine).
set -euo pipefail
cd "$(dirname "$0")/../.."
OUT=build/probe_fake
mkdir -p "$OUT"
CLANGCL=$(command -v clang-cl || command -v clang-cl-18 || true)
[[ -n "$CLANGCL" ]] || { echo "clang-cl not found"; exit 2; }
CXX=x86_64-w64-mingw32-g++
CC=x86_64-w64-mingw32-gcc
DEFS="-D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DWIN32_LEAN_AND_MEAN -DNOMINMAX"
WARN="-Wall -Wextra -Wno-unused-parameter"

echo "== build fake game"
"$CLANGCL" --target=x86_64-pc-windows-msvc /c /GR /GS- /O1 /Zl /EHs-c- tests/probe_fake/fake_classes.cpp /Fo"$OUT/fake_classes.obj" >/dev/null
$CXX -std=c++17 -O1 $WARN $DEFS -municode tests/probe_fake/fake_game.cpp "$OUT/fake_classes.obj" -o "$OUT/fake_game.exe" \
  -static -static-libgcc -static-libstdc++

echo "== build qs_probe_test.dll (real probe code)"
MH="third_party/minhook/src/hook.c third_party/minhook/src/buffer.c third_party/minhook/src/trampoline.c third_party/minhook/src/hde/hde64.c"
OBJS=()
for f in $MH; do
  o="$OUT/mh_$(basename "${f%.c}").o"
  $CC -O2 $DEFS -Ithird_party/minhook/include -c "$f" -o "$o"
  OBJS+=("$o")
done
$CXX -std=c++17 -O2 $WARN $DEFS -Ithird_party/minhook/include -Isrc/core -Isrc/win -shared -o "$OUT/qs_probe_test.dll" \
  tests/probe_fake/shim.cpp src/win/probe/*.cpp src/core/probe/*.cpp src/core/ini.cpp src/win/log.cpp src/win/winutil.cpp \
  "${OBJS[@]}" -static -static-libgcc -static-libstdc++

cp tests/probe_fake/probes_test.ini "$OUT/probes.ini"

echo "== run under wine"
rc=0
( cd "$OUT" && WINEDEBUG=-all timeout 120 wine fake_game.exe probes.ini probe.log ) | tr -d '\r' | tee "$OUT/fake_game.out" || rc=$?

echo "== check probe.log"
LOG="$OUT/probe.log"
pass=0; fail=0
expect() {  # expect <description> <grep -E regex>
  if tr -d '\r' < "$LOG" | grep -Eq -- "$2"; then pass=$((pass+1)); else fail=$((fail+1)); echo "LOG FAIL: $1 (/$2/)"; fi
}
expect "sections classified" "section \.prot .*OBFUSCATED"
expect "game code section" "game code section: \.text"
expect "rtti index" "RTTI index: [0-9]+ classes"
expect "class list" "probe list: 4 class\(es\) match"
expect "struct in list" "pa::ManagerBase"
expect "class resolved" "probe weather_scan: class pa::PAWeatherManager"
expect "hierarchy" "base pa::ManagerBase"
expect "vtable slots" "vtable rva 0x[0-9A-F]+: 3 slots"
expect "heap scan" "heap scan for vtable found [0-9]+ object"
expect "scan instance" "probe weather_scan: instance #0 .*heap scan\) class pa::PAWeatherManager"
expect "initial dump" "probe weather_scan: #0 initial dump"
expect "dump change" "probe weather_scan: #0 changed \+0x4"
expect "watch snow" "probe weather_scan: #0 .*snow@\+0x48:f32="
expect "stats 0..1" "floats in \[0, 1\] that changed .* \+0x48"
expect "vtable swap" "probe tod_capture: vtable slot 1 \(\+0x8\) swap installed"
expect "captured tod" "probe tod_capture: instance #0 .*captured this\) class pa::TimeOfDay"
expect "caller logged" "probe tod_capture: caller #0 return address rva"
expect "watch hour" "probe tod_capture: #0 .*hour@\+0x8:f32="
expect "minhook target ok" "probe weather_fn: minhook target rva .*ok \(code section\)"
expect "detour installed" "detour installed"
expect "captured wm via minhook" "probe weather_fn: instance #0 .*class pa::PAWeatherManager"
expect "duration over" "probe weather_fn: duration over, hook removed"
expect "clock global" "probe clock: instance #0 .*\(global\)"
expect "clock hour watch" "probe clock: #0 .*hour@\+0x4:u32="
expect "chain" "probe world_chain: instance #0 .*global chain\) class pa::PAWeatherManager"
expect "sim hierarchy" "base pa::TimeOfDay"
expect "missing class" "pa::DoesNotExist .*NOT FOUND"
expect "ambiguous" "probe ambiguous: .*match\(es\)"
expect "config error" "config error: probe 'badconfig'"
expect "summary" "summary\(periodic\) tod_capture: [0-9]+ calls"
expect "stopped" "probes stopped"
expect "hooks removed" "probe tod_capture: hook removed"
if grep -q "fake game: [0-9]* passed, 0 failed" "$OUT/fake_game.out"; then pass=$((pass+1)); else fail=$((fail+1)); echo "FAIL: fake game checks"; fi
if [[ $rc -eq 0 ]]; then pass=$((pass+1)); else fail=$((fail+1)); echo "FAIL: fake game exit code $rc"; fi
echo "probe_fake: $pass passed, $fail failed"
[[ $fail -eq 0 ]]
