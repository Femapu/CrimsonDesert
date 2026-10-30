// QuattroStagioni - runtime probe framework (entry points for the integration in main.cpp).
//
// Reads bin64\scripts\QuattroStagioni\probes.ini (next to the plugin) and, if [General] Enabled=1,
// starts ONE background thread that waits StartDelayMs, describes the game module (sections,
// classification), builds the RTTI index and runs every probe: log vtables / class hierarchies,
// find instances (heap scan for the vtable pointer, global via signature + RIP resolution, or
// capture of `this` through a vtable slot swap / guarded MinHook), dump them periodically with
// change detection, watch typed fields, accumulate statistics. Output:
// bin64\scripts\QuattroStagioni\probe.log. Read-only unless [General] AllowHooks=1.
//
// Integration (main.cpp, owner: integration stream):
//   - call probeStart() at the end of initialize(), after qs::io::install(), also when the version
//     guard fails (probes only read; they help find signatures for a new exe). It only reads the ini
//     and creates the thread, so it is safe under the loader lock.
//   - call probeStop() in DLL_PROCESS_DETACH on the FreeLibrary path (reserved == NULL) BEFORE
//     qs::io::uninstall() (io_hooks disables all MinHook hooks and uninitialises MinHook).
//     Never call it at process exit.
#pragma once
#include <string>

namespace qs {

bool probeStart();  // false if probes.ini is missing or disabled (nothing started)
void probeStop();   // stops the thread (bounded wait), restores every hook, closes probe.log

// Same with explicit paths (tests, tools).
bool probeStartWith(const std::wstring& iniPath, const std::wstring& logPath);

}  // namespace qs
