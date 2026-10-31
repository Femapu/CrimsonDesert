// QuattroStagioni - seasonal light and sky in the game (Win32 side of src/core/apply/lighting.h).
//
// Reads [Lighting] from QuattroStagioni.ini and [lighting] from QuattroStagioni\targets.ini (which
// game fields: SunColor=, SunIntensity=, AmbientColor=, AmbientIntensity=, FogDensity=, SkyColor=,
// as "<layer>:<offset>[:<count>]"; off by default). A thread (250 ms) computes the multipliers of the
// Temperate region's visual stage into atomics; a weatherAddPostCompose consumer, on the game thread
// right after the game composed its atmosphere, writes game value * multiplier (desert fade included).
// Safety: a field the game does not compose anew every tick (the value read is still the one we wrote,
// tick after tick) would accumulate, so the feature turns itself off and puts the game's value back;
// a non-finite value or one outside <Key>Min/<Key>Max does the same. Status in the F5 Info tab
// (feature "lighting"), log lines start with "lighting:".
#pragma once
#include <cstdint>
#include <string>

#include "../../core/apply/lighting.h"
#include "feature_status.h"

namespace qs {

bool lightingStart(const std::wstring& scriptsDir, bool versionOk);  // false: not running (off, no fields, error)
void lightingStop();

struct LightingInfo {
    bool started = false;
    FeatureState state = FeatureState::Waiting;
    int fields = 0;                      // fields set in targets.ini [lighting]
    std::string stage;                   // visual stage id of the Temperate region ("A>W.2")
    bool on = false;                     // config + F5 menu + region enabled
    apply::LightingMultipliers mult;     // current (smoothed) multipliers
    uint64_t writes = 0;
};
bool lightingGetInfo(LightingInfo& out);  // false if never started

}  // namespace qs
