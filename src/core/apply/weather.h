// QuattroStagioni - seasonal weather: spells rolled on the game's clock, composed over the game's weather.
//
// What the game does (Seasons of Pywel research notes, ported with the author's permission, checked on
// the 2.04.02 exe 0x6AE46DD0): every tick the climate manager composes its weather layers into a
// WeatherConstant at [[mgr+0x60]+0x20] (+0x12C precipitation, +0x130 cloudiness, +0x134 humidity,
// +0x16C snow, +0x170 rain) and an atmosphere at [[mgr+0x60]+0x28] (+0xAC cloud cover the sky shows,
// about 0.45 by default, 1 = overcast). Rain/snow effects and the HUD read those values; writing them
// right AFTER the compose changes the visible weather. Storms: the HUD has a Storm flag (bit 6) but no
// known field drives lightning yet, so a Storm spell is the heaviest rain tier (rain, overcast, humid);
// lightning is a separate module (lightning.h) that writes a field found with the probes.
//
// This file is the portable part (unit tested in tests/test_weather.cpp):
//   WeatherConfig    [Weather] keys of QuattroStagioni.ini, every key optional (defaults per phase)
//   WeatherDirector  spells (kind, intensity, game hours) rolled from the phase rules on the game's
//                    clock, smooth levels (RampPerHour), compose() over the game's own values
//   GameClock        game hours elapsed from successive readings of the game's hour (midnight, jumps)
//   desert helpers   the game's CPU climate map (512x512, 80 m texels, top byte = day's top temp)
// The Win32 side (src/win/apply/weather_win.cpp) hooks the game's weather update and HUD query.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "../ini.h"
#include "../season/season.h"

namespace qs::apply {

// Game = the game's own weather (only the phase's native rain scale applies).
enum class WeatherKind : uint8_t { Game = 0, Clear, Cloudy, Overcast, Rain, Storm };
constexpr int kWeatherKinds = 6;
const char* toString(WeatherKind k);                    // "game", "clear", ... (log, ini order)
const char* weatherKindIt(WeatherKind k, bool snowing); // "pioggia" / "neve", "temporale" / "bufera" ...

struct PhaseWeather {
    // Relative weights of the next spell, in WeatherKind order (Game, Clear, Cloudy, Overcast, Rain, Storm).
    float weight[kWeatherKinds] = {0, 1, 0, 0, 0, 0};
    float minHours = 3, maxHours = 8;            // spell length (game hours), Storm excluded
    float stormMinHours = 1, stormMaxHours = 3;  // Storm length
    float rainMin = 0.3f, rainMax = 0.7f;        // Rain intensity (rain or snow amount 0..1)
    float stormMin = 0.85f;                      // Storm intensity stormMin..1
    float nativeRain = 1.0f;                     // share of the game's own rain kept (winter: as snow)
};

struct WeatherConfig {
    bool enabled = true;              // [Weather] Enabled
    float rampPerHour = 1.5f;         // RampPerHour: level change per game hour (1.5 = 40 game minutes)
    float clearCover = 0.25f;         // ClearCover: highest cloud cover in a Clear spell
    float desertTopC = 28.0f;         // DesertTopTempC: climate day top at or above this = desert
    int desertRadius = 4;             // DesertRadius: texels (80 m) around the camera checked
    float desertFadeSec = 3.0f;       // DesertFadeSec: real seconds to hand the sky back to the game
    bool nativeSnowOutsideWinter = true;  // KeepGameSnow: the game's own snow outside winter (mountains)
    uint32_t startDelayMs = 20000;    // StartDelayMs
    float logIntervalSec = 60.0f;     // LogIntervalSec: composed values in the log (0 = never)
    PhaseWeather phase[season::kPhaseCount];

    WeatherConfig();                  // Stefano's rules (see weatherIniTemplate)
    // Missing keys keep the defaults; malformed values keep the default and add a warning line.
    void load(const Ini& ini, std::vector<std::string>& warnings);
};

std::string weatherIniTemplate();     // documented [Weather] section with the defaults

// The composed values the game reads (hasCover false: the atmosphere layer was not readable).
struct WeatherLayer {
    float precipitation = 0, cloudiness = 0, humidity = 0, snow = 0, rain = 0, cover = 0;
    bool hasCover = false;
};

struct WeatherSpell {
    WeatherKind kind = WeatherKind::Game;
    season::Phase phase = season::Phase::Summer;   // whose rules rolled it
    float intensity = 0;
    float hours = 0;
};

// Smoothed state the compose applies (moves toward the spell's target at RampPerHour).
struct WeatherLevels {
    float precip = 0;       // our rain / snow amount
    float coverFloor = 0, coverCap = 1;
    float cloudFloor = 0, cloudCap = 1;
    float humid = 0;
    float suppress = 0;     // 1: the game's own precipitation removed (Clear)
    float nativeRain = 1;   // the phase's share of the game's own rain
};

// Season inputs of one step. winterK: apply::winterIntensity (0..1, same ramp as the snow cvars).
struct WeatherSeason {
    season::Phase phase = season::Phase::Summer;
    float progress = 0;           // 0..1 in the phase
    float transitionFraction = 0.4f;
    float winterK = 0;
};

class WeatherDirector {
public:
    void configure(const WeatherConfig& c) { cfg_ = c; }
    const WeatherConfig& config() const { return cfg_; }
    void seed(uint32_t s) { rng_ = s ? s : 0x9E3779B9u; }

    // Game hours passed (any amount: done in 0.25 h steps, at most 48 h). Rolls spells, moves levels.
    void advance(double gameHours, const WeatherSeason& s);
    // Forces the next roll now (F5 / tests).
    void reroll() { until_ = clock_; }

    // The game's composed values -> ours. bypass 0..1: share handed back to the game (desert, off).
    // Non-finite input: returned unchanged.
    WeatherLayer compose(const WeatherLayer& game, float winterK, float bypass) const;

    const WeatherSpell& spell() const { return spell_; }
    bool hasSpell() const { return rolled_; }
    float hoursLeft() const { return until_ > clock_ ? float(until_ - clock_) : 0.0f; }
    const WeatherLevels& levels() const { return lv_; }
    std::vector<std::string> takeLog();

    // Target levels of a spell (exposed for the tests).
    static WeatherLevels target(const WeatherConfig& c, const WeatherSpell& s);

private:
    float uniform();  // 0..1
    void roll(const WeatherSeason& s);

    WeatherConfig cfg_;
    WeatherSpell spell_;
    WeatherLevels lv_;
    bool rolled_ = false;
    double clock_ = 0, until_ = 0;
    uint32_t rng_ = 0x9E3779B9u;
    std::vector<std::string> log_;
};

// Snow share of precipitation for a winter intensity (0 below 0.3, 1 above 0.7).
float snowShare(float winterK);

// Game hours from the game's hour of day, read repeatedly (0..24). A step forward of at most
// maxRunHours counts as time running; a bigger one (sleep, wait, loading) as a jump, capped at
// maxJumpHours. Backward steps past midnight wrap. Invalid readings are ignored.
class GameClock {
public:
    struct Step {
        double hours = 0;
        bool jump = false;
    };
    Step feed(float hour, double maxRunHours = 0.5, double maxJumpHours = 24.0);
    bool started() const { return last_ >= 0; }
    float hour() const { return last_; }

private:
    float last_ = -1;
};

// Climate map texel (u32 = A | R<<8 | G<<16 | B<<24): the day's top temperature in C (B: -50..+50).
float climateTopC(uint32_t texel);
constexpr int kClimateMapSize = 512;
constexpr float kClimateTexelM = 80.0f;
// Row/column of a world position (x, z in metres) in the climate map; false outside.
bool climateCell(float x, float z, int& row, int& col, int size = kClimateMapSize);
// Desert: some texel within `radius` of (row, col) has a top >= thresholdC. `rows` holds the
// (2r+1) x (2r+1) block around the cell, row major, already read from the game (out of map = 0).
bool desertBlock(const std::vector<uint32_t>& block, float thresholdC, float* hottest = nullptr);

}  // namespace qs::apply

namespace qs::apply {

// The HUD's weather flags (bit 0 rainy, 1 snowy, 2 windy, 3 fog, 4 cloudy, 5 sunny, 6 storm; the HUD
// shows the lowest bit set). Returns the index 0..6, or -1 when none is set.
int hudWeatherIndex(uint32_t flags);
const char* hudWeatherName(int index);  // "rainy" ... "storm", "unknown"
constexpr uint32_t kHudStormBit = 1u << 6;

}  // namespace qs::apply
