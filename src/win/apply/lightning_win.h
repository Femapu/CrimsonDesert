// QuattroStagioni - lightning in storms, by season (Win32 side of src/core/apply/lightning.h).
//
// Reads [Lightning] from QuattroStagioni.ini and [lightning] from QuattroStagioni\targets.ini (off by
// default: Enabled=0, Field=<layer>:<offset>[:<count>], Mode=level|pulse, Value=<f32>). One thread,
// every 250 ms, feeds the LightningDirector from the weather module (season's Storm spell, HUD Storm
// bit, desert) and the season, and keeps the result in atomics. The weather hook's after-compose
// callback (weatherAddPostCompose, game thread) only writes:
//   Mode=level  field = game + (Value - game) * level   (level 0..1, smooth)
//   Mode=pulse  field = Value during a strike's flash (PulseMs), the game's value otherwise
// Nothing is restored on exit: the game composes the layers anew every tick. With no Field set the
// logic still runs and logs "lightning: strike ..." lines (to check the frequencies in game). The
// first read or write failure disables the feature ("lightning: DISABLED: <why>"). The thunder stays
// the game's own. Log lines start with "lightning:".
#pragma once
#include <cstdint>
#include <string>

#include "../../core/apply/lightning.h"

namespace qs {

bool lightningStart(const std::wstring& scriptsDir, bool versionOk);
void lightningStop();

struct LightningInfo {
    bool started = false;
    bool fieldSet = false;         // targets.ini [lightning] usable
    std::string field;             // "atmosphere:0x1A0" or ""
    apply::LightningMode mode = apply::LightningMode::Level;
    bool allowed = false;          // enabled, F5 on, region on, not in the desert
    bool storm = false, hasLightning = false;
    float level = 0, rate = 0, chance = 0;
    uint64_t strikes = 0, storms = 0, writes = 0;
    double sinceStrikeSec = 1e9;
};
bool lightningGetInfo(LightningInfo& out);  // false if never started

}  // namespace qs
