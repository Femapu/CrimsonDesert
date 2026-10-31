// QuattroStagioni - seasonal weather in the game (Win32 side of src/core/apply/weather.h).
//
// Reads [Weather] from QuattroStagioni.ini (every key optional). One thread waits StartDelayMs, then
// finds by unique byte signature (checked on the 2.04.02 exe) the climate manager's weather update
// (rcx = climate manager, xmm1 = dt; it composes the weather layers) and the HUD's weather query, and
// hooks both through probe::GuardedMinHook. After the game composes its weather the hook writes the
// season's values (WeatherDirector::compose) with apply::GuardedMemory; it also reads the game's hour
// (the clock's virtual +0x100), so spells follow the game's clock (sleeping or waiting moves them on).
// The desert check reads the climate manager's CPU climate map around the camera: in the desert, or
// when "Meteo stagionale" is off, the values fade back to the game's own and nothing is written.
// Nothing is ever restored: the game composes its weather anew every tick, so stopping the writes is
// enough. Any failure disables this module only ("weather: DISABLED: <reason>"). Log lines start with
// "weather:".
#pragma once
#include <cstdint>
#include <string>

#include "../../core/apply/weather.h"

namespace qs {

bool weatherStart(const std::wstring& scriptsDir, bool versionOk);
void weatherStop();

enum class WeatherModState : uint8_t { Waiting, Active, Off, Disabled };
const char* toString(WeatherModState s);

struct WeatherInfo {
    bool started = false;
    WeatherModState state = WeatherModState::Waiting;
    std::string reason;
    bool hudHooked = false;
    int64_t hudFlags = -1;          // -1: not seen yet
    bool hasSpell = false;
    apply::WeatherSpell spell;
    float hoursLeft = 0;
    bool snowing = false;           // precipitation falls as snow (winter)
    bool desert = false, mapFound = false;
    float hottestC = 0;             // hottest climate day top around the camera
    float bypass = 1;               // 1 = all the game's own
    float hour = -1;                // game hour, -1 unknown
    bool clockFromGame = false;
    apply::WeatherLayer game, out;  // last composed values
    uint64_t writes = 0;
};
bool weatherGetInfo(WeatherInfo& out);  // false if never started
// The climate manager seen by the update hook (0 until the game ran one tick); used by the climate module.
uintptr_t weatherClimateManager();

}  // namespace qs
