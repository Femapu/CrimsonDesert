// QuattroStagioni - seasonal day length in the game (Win32 side of src/core/apply/daylength.h).
//
// Reads [DayLength] from QuattroStagioni.ini and the [daylength] section of QuattroStagioni\targets.ini
// (Enabled=0 by default, Field=<LayerRef>, Kind=sundir|hour). The game's clock speed is never touched:
// only the sun field is rewritten right after the game composed its atmosphere (weatherAddPostCompose,
// game thread), with the value the game itself has at the remapped "sun hour". Kind=sundir learns
// that value per 15 minute bin from the game's own compose and keeps the table in
// QuattroStagioni\sun_table.ini (tied to the exe TimeDateStamp, local data, never in the repo).
// A thread (250 ms) computes the seasonal sunrise and sunset, logs them and saves the table.
// Safety: a field the game does not recompose, non-finite values or failing writes disable this
// module only ("daylength: DISABLED: <reason>"); unknown game hour = no write. Log lines start with
// "daylength:". Guide: docs/dev/durata_giorno.md.
#pragma once
#include <cstdint>
#include <string>

#include "../../core/apply/daylength.h"

namespace qs {

bool daylengthStart(const std::wstring& scriptsDir, bool versionOk);
void daylengthStop();

struct DayLengthInfo {
    bool started = false;
    bool fieldSet = false;            // [daylength] Enabled=1 with a valid Field
    std::string field;                // "atmosphere:0x1A0:3 sundir"
    apply::SunTimes game, seasonal;   // the game's own times, the seasonal target
    apply::SunTimes current;          // ramped values in use
    int tableBins = 0;                // known bins (sundir)
    uint64_t writes = 0;
    float hour = -1, sunHour = -1;    // last game hour and sun hour seen by the compose callback
};
bool daylengthGetInfo(DayLengthInfo& out);  // false if never started

}  // namespace qs
