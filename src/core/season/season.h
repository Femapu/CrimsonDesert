// QuattroStagioni - season engine: calendar, visual stages per region, weather, snow, light, day length.
//
// Contract (docs/dev/CONTRACTS.md, owner: season stream). Pure, portable, deterministic: no Win32,
// no game access. The plugin feeds it a day value and reads back a SeasonState; the F5 menu edits
// SeasonConfig. Design: docs/DESIGN.md sections 2-7.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "../ini.h"

namespace qs::season {

enum class Phase : uint8_t { Spring = 0, Summer = 1, LateSummer = 2, Autumn = 3, Winter = 4 };
constexpr int kPhaseCount = 5;
const char* toString(Phase p);

enum class Region : uint8_t { Temperate = 0, NorthCold, SwampJungle, Aquatic, DesertDry, Abyss, Count };
const char* toString(Region r);
Region regionFromHint(const std::string& hint);  // "shared_temperate" -> Temperate, ...

enum class DaySource : uint8_t { GameClock = 0, Playtime = 1, Manual = 2 };

struct RegionConfig {
    bool enabled = true;
    float dayOffset = 0.0f;       // shifts the calendar for this region (negative = earlier)
    float phaseScale[kPhaseCount] = {1, 1, 1, 1, 1};  // relative phase length multipliers
    float meanTempC = 14.0f, ampTempC = 11.0f;          // annual temperature model
};

struct SeasonConfig {
    float phaseDays[kPhaseCount] = {8, 8, 5, 8, 9};     // S U L A W
    float transitionFraction = 0.4f;                     // tail of each phase spent blending to the next
    int transitionSteps = 3;                             // discrete stages per transition (0 = hard switch)
    float assetJitterDays = 2.0f;                        // per-asset calendar jitter (+-)
    DaySource daySource = DaySource::Playtime;
    float realMinutesPerDay = 48.0f;                     // used when daySource == Playtime
    float startDay = 0.0f;                               // day of year at first launch / manual value
    bool weatherControl = true, snowAccumulation = true, lightControl = true, dayLengthControl = true,
         particles = true;
    float snowAccumPerHour = 0.25f, snowMeltPerHour = 0.15f;  // in-game hours
    float virtualLatitudeDeg = 45.0f;
    RegionConfig regions[static_cast<int>(Region::Count)];

    void setDefaults();                                  // fills regions with DESIGN.md defaults
    bool load(const Ini& ini, std::vector<std::string>& errors);   // [Seasons], [Region.<name>] sections
    std::string save() const;                            // ini text, round-trips with load()
    float yearLength() const;
};

// One discrete visual stage, identical to recipe::Stage plus a stable id for packs/caches.
struct VisualStage {
    Phase from = Phase::Summer, to = Phase::Summer;
    int step = 0;          // 0 = plateau of `from`; 1..transitionSteps = intermediate stages
    float t = 0.0f;        // step / (transitionSteps + 1)
    std::string id() const;  // e.g. "U", "A>W.2"
    bool operator==(const VisualStage& o) const { return from == o.from && to == o.to && step == o.step; }
};

struct WeatherWeights {     // matches the game's WeatherState flags, normalised to sum 1
    float sunny = 0, cloudy = 0, rainy = 0, storm = 0, snowy = 0, windy = 0;
};

struct SeasonState {
    double day = 0;                  // continuous day counter since start
    float dayOfYear = 0;             // 0 .. yearLength
    Phase phase = Phase::Summer;
    float phaseProgress = 0;         // 0..1 inside the phase
    VisualStage stage[static_cast<int>(Region::Count)];  // per region (disabled regions -> Summer plateau)
    float temperatureC[static_cast<int>(Region::Count)] = {};
    WeatherWeights weather[static_cast<int>(Region::Count)];
    float snowCover[static_cast<int>(Region::Count)] = {};  // 0..1
    float sunElevationOffsetDeg = 0;  // vs. summer
    float lightTint[3] = {1, 1, 1};   // multiplicative colour, 1 = vanilla
    float sunriseHour = 6, sunsetHour = 20;
    float fallingLeaves = 0;          // 0..1 particle intensity
    float breathThresholdC = 5.0f;
};

class Engine {
public:
    void configure(const SeasonConfig& cfg);
    const SeasonConfig& config() const { return cfg_; }
    // Advances the internal clock. `inGameHours` is elapsed game time (for snow), `day` the absolute day.
    void update(double day, float inGameHours);
    const SeasonState& state() const { return state_; }

    // Stage for one asset (applies per-asset jitter from its seed), used by the generator and GPU swap.
    VisualStage stageFor(Region r, uint32_t assetSeed) const;
    // All stages an asset can be in, in calendar order (for cache planning).
    std::vector<VisualStage> allStages() const;
    // The next stage after `s` in calendar order.
    VisualStage nextStage(const VisualStage& s) const;

private:
    SeasonConfig cfg_;
    SeasonState state_;
};

}  // namespace qs::season
