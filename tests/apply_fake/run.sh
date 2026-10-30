#!/usr/bin/env bash
# QuattroStagioni - end-to-end applier test: fake game (MSVC RTTI via clang-cl) + real applier code, under wine.
#   tests/apply_fake/run.sh        -> builds into build/apply_fake/, runs, prints "N passed, M failed"
# Needs: clang-cl (any recent LLVM), mingw-w64, wine (64-bit). Not part of ./build.sh (needs wine).
# Same pattern as tests/probe_fake/run.sh (kept separate so the probe test is never affected).
set -euo pipefail
cd "$(dirname "$0")/../.."
OUT=build/apply_fake
mkdir -p "$OUT"
CLANGCL=$(command -v clang-cl || command -v clang-cl-18 || true)
[[ -n "$CLANGCL" ]] || { echo "clang-cl not found"; exit 2; }
CXX=x86_64-w64-mingw32-g++
CC=x86_64-w64-mingw32-gcc
DEFS="-D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DWIN32_LEAN_AND_MEAN -DNOMINMAX"
WARN="-Wall -Wextra -Wno-unused-parameter"

echo "== build fake game"
"$CLANGCL" --target=x86_64-pc-windows-msvc /c /GR /GS- /O1 /Zl /EHs-c- tests/apply_fake/fake_classes.cpp /Fo"$OUT/fake_classes.obj" >/dev/null
$CXX -std=c++17 -O1 $WARN $DEFS -municode tests/apply_fake/fake_game.cpp "$OUT/fake_classes.obj" -o "$OUT/fake_game.exe" \
  -static -static-libgcc -static-libstdc++

echo "== build qs_apply_test.dll (real applier, probe helpers, season engine)"
MH="third_party/minhook/src/hook.c third_party/minhook/src/buffer.c third_party/minhook/src/trampoline.c third_party/minhook/src/hde/hde64.c"
OBJS=()
for f in $MH; do
  o="$OUT/mh_$(basename "${f%.c}").o"
  $CC -O2 $DEFS -Ithird_party/minhook/include -c "$f" -o "$o"
  OBJS+=("$o")
done
$CXX -std=c++17 -O2 $WARN $DEFS -Ithird_party/minhook/include -Isrc/core -Isrc/win -Ithird_party/lz4 -shared -o "$OUT/qs_apply_test.dll" \
  tests/apply_fake/shim.cpp src/win/apply/*.cpp src/win/probe/*.cpp src/core/apply/*.cpp src/core/probe/*.cpp \
  src/core/season/*.cpp src/core/ini.cpp src/win/log.cpp src/win/winutil.cpp src/win/qs_state.cpp \
  "${OBJS[@]}" -static -static-libgcc -static-libstdc++

cp tests/apply_fake/targets_test.ini "$OUT/targets.ini"
rm -f "$OUT/apply.log"

echo "== run under wine"
rc=0
( cd "$OUT" && WINEDEBUG=-all timeout 180 wine fake_game.exe targets.ini apply.log ) | tr -d '\r' | tee "$OUT/fake_game.out" || rc=$?

echo "== check apply.log"
LOG="$OUT/apply.log"
pass=0; fail=0
expect() {  # expect <description> <grep -E regex>
  if tr -d '\r' < "$LOG" | grep -Eq -- "$2"; then pass=$((pass+1)); else fail=$((fail+1)); echo "LOG FAIL: $1 (/$2/)"; fi
}
expect "started" "apply: started, 6 object\(s\), 16 of 17 target\(s\) enabled"
expect "rtti" "apply: RTTI index [0-9]+ classes"
expect "class resolved" "apply: object weather: class pa::WeatherManager vtable rva"
expect "global chain" "apply: object time found \(global\): 1 instance\(s\).* class pa::TimeManager"
expect "heap scan" "apply: object weather found \(heap scan\)"
expect "capture" "apply: object light: capture hook installed"
expect "captured" "apply: object light found \(capture\)"
expect "capture removed" "apply: object light: capture hook removed \(object found\)"
expect "clock struct" "apply: object clock found \(global\)"
expect "attach original" "apply: target day_speed: attached at 0x[0-9A-F]+, original 1"
expect "active" "apply: target night_speed active \(time_scale_night, scale\)"
expect "clock reading" "apply: target hour: reading hour"
expect "clock found" "apply: game clock found"
expect "not sticking" "apply: target pinned DISABLED: value not sticking"
expect "rdata refused" "apply: target leaves_rdata DISABLED: write refused or not verified: page is not PAGE_READWRITE"
expect "object lost" "apply: object weather lost \(vtable changed"
expect "rebound" "apply: object weather bound: 1 instance"
expect "restored" "apply: stop, [0-9]+ original value\(s\) restored"
expect "clock lost at stop" "apply: (game clock lost|stop)"
if tr -d '\r' < "$LOG" | grep -q "apply: object weather found (heap scan)" && \
   [[ $(tr -d '\r' < "$LOG" | grep -c "apply: object weather found (heap scan)") -ge 2 ]]; then pass=$((pass+1)); else fail=$((fail+1)); echo "LOG FAIL: weather manager found twice"; fi
if grep -q "fake game: [0-9]* passed, 0 failed" "$OUT/fake_game.out"; then pass=$((pass+1)); else fail=$((fail+1)); echo "FAIL: fake game checks"; fi
if [[ $rc -eq 0 ]]; then pass=$((pass+1)); else fail=$((fail+1)); echo "FAIL: fake game exit code $rc"; fi
echo "apply_fake: $pass passed, $fail failed"
[[ $fail -eq 0 ]]
