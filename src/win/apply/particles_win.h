// QuattroStagioni - seasonal particles in the game (Win32 side of src/core/apply/particles.h).
//
// Reads [Particles] from QuattroStagioni.ini and [particles] from QuattroStagioni\targets.ini. A thread
// (250 ms) works out the season's intensities (winter flakes, leaves, pollen) into atomics; a callback
// registered with weatherAddPostCompose (game thread, after the game composed its weather and after the
// weather module wrote the season's) re-reads the current values and writes:
//   - winter flakes: snow +0x16C of the WeatherConstant (offset checked on 2.04.02), only while it
//     already snows, only in winter, never in the desert;
//   - leaves / pollen: only the fields named in targets.ini [particles] (off until a probe finds them).
// The first failed read or write switches the whole module off ("particles: DISABLED: <why>"); nothing
// is restored because the game composes its weather anew every tick. Log lines start with "particles:".
#pragma once
#include <string>

namespace qs {

bool particlesStart(const std::wstring& scriptsDir, bool versionOk);
void particlesStop();

struct ParticlesInfo {
    bool started = false;
    bool flakesOn = false, leavesOn = false, pollenOn = false;  // module parts that may write
    float flakeK = 0, leaves = 0, pollen = 0;                   // current intensities 0..1
    float gameSnow = 0, outSnow = 0;                            // last snow read / written
    unsigned long long writes = 0;
};
bool particlesGetInfo(ParticlesInfo& out);  // false if never started

}  // namespace qs
