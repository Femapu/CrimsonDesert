// QuattroStagioni - winter snow thresholds in the game (Win32 side of src/core/apply/snow.h).
//
// Reads [Snow] from QuattroStagioni.ini (every key optional, defaults in SnowConfig). One thread waits
// StartDelayMs, then finds the game's SnowStart / SnowFull cvars (pa::GlobalVariableFloat in the exe's
// data section) by signature, verifies the RTTI class and the values (plausible, equal to their
// defaults) and runs the portable SnowController every 250 ms with the season of qs::shared().
// Never writes when the version guard failed or the exe timestamp is not a known build. Originals
// are restored when snow is switched off (F5 "Neve che si accumula" or [Snow] Enabled=0) and on
// snowStop(). Log lines start with "snow:".
#pragma once
#include <string>

#include "../../core/apply/snow.h"

namespace qs {

// versionOk: result of the plugin's version guard (setup). False -> nothing started.
bool snowStart(const std::wstring& scriptsDir, bool versionOk);
void snowStop();  // restores the originals, stops the thread (bounded wait)

struct SnowInfo {
    bool started = false;
    bool configEnabled = false;
    apply::SnowStatus status;
    float winterStart = 0, winterFull = 0;  // effective configured winter values
};
bool snowGetInfo(SnowInfo& out);  // cheap copy under a mutex; false if never started

}  // namespace qs
