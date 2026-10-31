// QuattroStagioni - seasonal temperatures in the game (Win32 side of src/core/apply/climate.h).
//
// Reads [Climate] from QuattroStagioni.ini (every key optional). One thread waits StartDelayMs, then
// finds the vtable of pa::PAClimateTexture by its RTTI name (no hard-coded address; the expected
// address of the known builds is only logged as a cross-check) and scans the game heap for its
// instances (the weather manager's and the climate manager's map). Each map is checked (512x512
// texels, 80 m, plausible temperatures) and copied at attach; the season's map (apply::applyClimate)
// is written with apply::GuardedMemory. If the game rebuilds a map (world load) the new content
// becomes the original. The originals are written back on menu toggle off, on disable and on unload.
// Any failure disables this module only ("climate: DISABLED: <reason>"). Log lines start with
// "climate:".
#pragma once
#include <cstdint>
#include <string>

namespace qs {

bool climateStart(const std::wstring& scriptsDir, bool versionOk);
void climateStop();  // restores the original maps

enum class ClimateModState : uint8_t { Waiting, Active, Off, Disabled };

struct ClimateInfo {
    bool started = false;
    ClimateModState state = ClimateModState::Waiting;
    std::string reason;
    int maps = 0;               // maps attached
    float offsetC = 0;          // offset in effect (ramped)
    float targetC = 0;          // the season's offset
    bool cameraKnown = false;
    int cameraKind = 0;         // apply::TexelKind at the camera
    float cameraGameTopC = 0, cameraTopC = 0;  // the game's day top and ours at the camera
    uint64_t writes = 0;
};
bool climateGetInfo(ClimateInfo& out);  // false if never started

}  // namespace qs
