// QuattroStagioni - applies the season engine to the game (entry points for main.cpp, the season
// tick and the F5 menu).
//
// Reads bin64\scripts\QuattroStagioni\targets.ini (next to the plugin). If [General] Enabled=1, ONE
// background thread waits StartDelayMs, finds the [object.*] sections with the probe helpers
// (src/win/probe/: RTTI index, signature scan, heap scan by vtable, guarded capture hooks) and runs
// the portable controller (src/core/apply/applier.h) every IntervalMs (10 Hz) with the SeasonState
// of qs::shared(). Writes go only to committed PAGE_READWRITE pages (heap, or the exe's writable
// data section; never code, never read-only data, never the protector section) and are verified by
// reading them back. Originals are restored when a feature is switched off and on applyStop().
// Messages go to QuattroStagioni.log with the "apply:" prefix. Guide: docs/dev/appliers.md.
//
// Integration (owner: integration stream, src/win/main.cpp):
//   - applyStart() at the end of initialize(), after seasonStart() and probeStart() (it only reads
//     the ini and creates the thread: safe under the loader lock). Like the probes it may also run
//     when the version guard fails: every object is found by RTTI/signature at runtime and an Rva=
//     is honoured only with a matching [General] ExpectTimestamp.
//   - applyStop() in DLL_PROCESS_DETACH on the FreeLibrary path, BEFORE probeStop() and
//     io::uninstall() (it may own capture hooks made through MinHook). Never at process exit.
//   - season tick (src/win/season_tick.cpp), when daySource == GameClock:
//         double gameDay, gameHours;
//         if (applyReadGameClock(gameDay, gameHours)) {
//             day = cfg.advance(day, dt, gameHours);          // game hours drive the calendar
//             float h; applyGameHourOfDay(h);
//             engine.update(day, float(gameHours), h);
//         } else { ...playtime fallback as today... }
//     qs::shared().gameClockHooked is set by this module while a clock target is active.
//   - F5 menu: applyGetStatus(status) for per-feature state (inactive / not found / active / error)
//     and live values; see apply::ApplyStatus.
#pragma once
#include <string>

#include "../../core/apply/applier.h"

namespace qs {

bool applyStart();  // false if targets.ini is missing or disabled (nothing started)
void applyStop();   // restores every original, removes capture hooks, stops the thread (bounded wait)
bool applyStartWith(const std::wstring& iniPath);  // explicit path (tests, tools)

// Game clock, once a "hour" and/or "day_counter" target is active. day = game day counter +
// hour/24 (-1 when only the hour is known); hours = game hours elapsed since the previous call
// (0 on the first call; a backward jump such as loading a save counts as 0). False while unknown.
bool applyReadGameClock(double& day, double& hours);
bool applyGameHourOfDay(float& hour);

// Snapshot for the F5 menu (cheap: a copy under a mutex). False if the module never started.
bool applyGetStatus(apply::ApplyStatus& out);

}  // namespace qs
