// QuattroStagioni - seasonal sunrise and sunset without touching the speed of the game's clock.
//
// Idea: the game draws its sun from its hour of day h. We keep the clock as it is and remap only the
// "sun hour": h -> h', piecewise linear and continuous, the seasonal day [sunrise, sunset] onto the
// game's own visible day [GameSunrise, GameSunset] and the night on the rest (across midnight). In
// summer (no shift) h' = h. Right after the game composed its atmosphere we write on the sun field the
// value the game ITSELF has at hour h': since nobody knows how the game computes the sun direction,
// the module LEARNS a table (96 bins of 15 minutes, up to 4 floats each) from the values the game
// composes at each hour, and writes the interpolated value of h' only when the neighbouring bins are
// known. A field that holds an hour (Kind=hour) is simply written with h'.
//
// Portable part, unit tested in tests/test_daylength.cpp:
//   DayLengthConfig   [DayLength] keys of QuattroStagioni.ini (template dayLengthIniTemplate)
//   DayLengthTarget   [daylength] section of QuattroStagioni\targets.ini (Enabled, Field, Kind)
//   seasonalSunTimes  sunrise and sunset of a phase (stageValue / phaseValue blend)
//   remapHour         h -> h'
//   SunTable          learned values per bin; text form saved in QuattroStagioni\sun_table.ini
// Win32 side: src/win/apply/daylength_win.cpp.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "../ini.h"
#include "../season/season.h"
#include "layer_field.h"

namespace qs::apply {

struct DayLengthConfig {
    bool enabled = true;            // [DayLength] Enabled
    float gameSunrise = 5.5f;       // GameSunrise: visible sunrise of the game (game hour)
    float gameSunset = 19.5f;       // GameSunset: visible sunset of the game
    PhaseValues sunriseShift;       // SunriseShift: hours added to GameSunrise, per phase S U L A W
    PhaseValues sunsetShift;        // SunsetShift: hours added to GameSunset
    float minDaylightHours = 8.0f;  // MinDaylightHours: the seasonal day is never shorter
    float rampPerHour = 0.5f;       // RampPerHour: change of sunrise/sunset (hours) per game hour
    int maxGapBins = 2;             // MaxGapBins: missing bins bridged by the interpolation
    int minSamples = 4;             // MinSamples: readings before a bin counts as known
    uint32_t startDelayMs = 25000;  // StartDelayMs
    float saveIntervalSec = 180;    // SaveIntervalSec: table saved this often (and at exit)
    float logIntervalSec = 120;     // LogIntervalSec: status line in the log (0 = never)

    DayLengthConfig();
    void load(const Ini& ini, std::vector<std::string>& warnings);
};
std::string dayLengthIniTemplate();  // documented [DayLength] section with the defaults

enum class SunKind : uint8_t { SunDir = 0, Hour = 1 };
const char* toString(SunKind k);  // "sundir", "hour"

// [daylength] of targets.ini. Off by default; Field empty = not found yet.
struct DayLengthTarget {
    bool enabled = false;
    LayerRef field;
    SunKind kind = SunKind::SunDir;
    void load(const Ini& ini, std::vector<std::string>& warnings);
};

struct SunTimes {
    float sunrise = 5.5f, sunset = 19.5f;
    float daylight() const { return sunset - sunrise; }
};
// The game's own times (no shift), clamped to a valid day.
SunTimes gameSunTimes(const DayLengthConfig& c);
// Seasonal times: game + shift of the phase (continuous blend at the end of the phase), with at
// least MinDaylightHours of day (the cut is shared by sunrise and sunset), inside [0.5, 23.5].
SunTimes seasonalSunTimes(const DayLengthConfig& c, season::Phase phase, float progress, float transitionFraction);
SunTimes seasonalSunTimes(const DayLengthConfig& c, const season::VisualStage& st);

// Game hour h (any value, wrapped to [0, 24)) -> sun hour h' in [0, 24). The seasonal day
// [s.sunrise, s.sunset] maps linearly onto [g.sunrise, g.sunset], the night [s.sunset, s.sunrise + 24]
// onto [g.sunset, g.sunrise + 24]. Continuous and increasing (modulo 24); identity when s == g.
// Invalid input (not finite, sunset <= sunrise): returns h wrapped.
float remapHour(float h, const SunTimes& s, const SunTimes& g);
// Shortest signed distance a - b on the 24 h circle (-12..12).
float hourDelta(float a, float b);
// "6:15"
std::string formatHour(float h);

constexpr int kSunBins = 96;  // 15 minutes each
constexpr int kSunMaxFloats = 4;

class SunTable {
public:
    explicit SunTable(int count = 3) { reset(count); }
    void reset(int count);     // forgets everything; count clamped to 1..4
    int count() const { return count_; }
    static int binOf(float hour);  // 0..95 (hour wrapped)
    // Adds a reading of the game's value at `hour` (running mean, then an exponential mean once the
    // bin has `capSamples`). Non-finite values are ignored (false).
    bool record(float hour, const float* v, int capSamples = 32);
    // Value at `hour`: linear interpolation between the centres of the nearest known bins around it
    // (wrapping at midnight), at most maxGap missing bins in between. false: not known.
    bool lookup(float hour, float* out, int minSamples = 4, int maxGap = 2) const;
    int known(int minSamples = 4) const;  // bins with at least minSamples readings
    uint32_t samples(int bin) const { return bin >= 0 && bin < kSunBins ? n_[bin] : 0; }
    // Text form (INI): header with the exe TimeDateStamp and the field, one line per known bin.
    std::string serialize(uint32_t exeStamp, const std::string& field) const;
    // Accepts only a table of the same exe and field (else false with `err`, the table is untouched).
    bool parse(const std::string& text, uint32_t exeStamp, const std::string& field, std::string* err = nullptr);

private:
    int count_ = 3;
    float v_[kSunBins][kSunMaxFloats] = {};
    uint32_t n_[kSunBins] = {};
};

}  // namespace qs::apply
