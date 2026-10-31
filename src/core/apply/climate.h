// QuattroStagioni - seasonal temperatures: the season's offset written into the game's CPU climate maps.
//
// What the game does (Seasons of Pywel research notes and climate.cpp, ported with the author's
// permission): the game keeps two CPU climate maps (class pa::PAClimateTexture: the weather manager's
// and the climate manager's at +0xD8). Object: +0x18 texels (u32), +0x20 texel count (512*512),
// +0x30/+0x34 metres per texel (80), +0x38/+0x3C width and height (512). Texel u32 = A | R<<8 | G<<16 |
// B<<24: B = the day's top temperature, (2B/255-1)*50 C; G = day to night range, the night is
// (1-G/255)*18+7 C colder; R = wind. The game samples the maps bilinearly while it runs: the felt
// temperature, the temperature meter, snow and deep snow follow them at once (no reload).
//
// This file is the portable part (unit tested in tests/test_climate.cpp):
//   ClimateConfig     [Climate] keys of QuattroStagioni.ini, every key optional
//   climateOffsetC    the season's offset from our calendar (plateau value, smooth over the tail)
//   ClimatePlan       built once per original map: desert (kept as the game has it, with a fade band),
//                     the game's own cold places (3x3 rule) and the night floor per texel
//   applyClimate      original map + plan + offset -> the map the game gets (only the B byte changes)
// The Win32 side (src/win/apply/climate_win.cpp) finds the maps, copies the originals and writes.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "../ini.h"
#include "../season/season.h"

namespace qs::apply {

struct ClimateConfig {
    bool enabled = true;            // [Climate] Enabled
    // Offsets on the day's top temperature per phase (Spring, Summer, LateSummer, Autumn, Winter).
    float offsetC[season::kPhaseCount] = {-6.0f, 4.0f, 2.0f, -8.0f, -24.0f};
    float nightFloorC = -30.0f;     // NightFloorC: no season makes a night colder than this
    float desertTopC = 28.0f;       // DesertTopTempC: texels with a top at/above this are desert
    int desertRadius = 4;           // DesertRadius: texels around a desert texel kept as the game's
    int desertFade = 4;             // DesertFade: texels over which the offset fades in past the radius
    float maxRatePerSec = 0.5f;     // MaxRate: degrees per real second the offset in effect may move
    uint32_t startDelayMs = 25000;  // StartDelayMs
    float logIntervalSec = 60.0f;   // LogIntervalSec (0 = only changes)
    // Missing keys keep the defaults; malformed values keep the default and add a warning line.
    void load(const Ini& ini, std::vector<std::string>& warnings);
};
std::string climateIniTemplate();  // documented [Climate] section with the defaults

// Texel helpers (u32 = A | R<<8 | G<<16 | B<<24).
float climateRangeC(uint32_t texel);   // day to night: (1-G/255)*18+7
uint32_t climateTopByte(float c);      // nearest B for a top temperature (clamped 0..255)
uint32_t climateTopByteUp(float c);    // smallest B whose top is >= c

// The season's offset: the phase's value on its plateau, smoothstep to the next phase's value over
// the tail (transitionFraction of the phase, the same tail our 20 visual stages are cut from).
float climateOffsetC(const ClimateConfig& c, season::Phase phase, float progress, float transitionFraction);

// Moves `current` toward `target` by at most maxRate * dt (dt in real seconds).
float climateRamp(float current, float target, float maxRate, float dt);

enum class TexelKind : uint8_t { Normal = 0, Desert = 1, DesertFade = 2, GameCold = 3 };

struct ClimatePlan {
    int size = 0;
    std::vector<float> weight;     // share of the offset (0 desert, 0..1 fade band, 1 elsewhere)
    std::vector<uint8_t> cold;     // 1: the game's own night is below the floor (or a 3x3 neighbour)
    std::vector<uint8_t> least;    // smallest B a cooling may give (night >= floor)
    uint32_t capByte = 255;        // largest B a warming may give (stays below the desert line)
    size_t desert = 0, fade = 0, gameCold = 0;
    float hottestC = -100, coldestC = 100;
    TexelKind kind(size_t i) const;
};

// Checks the original map looks like the game's temperature map and builds the plan. False (with
// *why) when the data does not fit (wrong size, no desert, implausible temperatures).
bool buildClimatePlan(const std::vector<uint32_t>& orig, int size, const ClimateConfig& c, ClimatePlan& plan,
                      std::string* why);

struct ClimateApplyStats {
    size_t changed = 0, floored = 0, capped = 0;
};
// out = orig with the B byte moved by offsetC * weight (cooling: never below the night floor, never
// on the game's cold places; warming: never up to the desert line). offsetC 0 gives orig exactly.
void applyClimate(const std::vector<uint32_t>& orig, const ClimatePlan& plan, const ClimateConfig& c, float offsetC,
                  std::vector<uint32_t>& out, ClimateApplyStats* stats = nullptr);

}  // namespace qs::apply
