// QuattroStagioni - season engine: calendar, visual stages per region, weather, snow, light, day length.
//
// Contract (docs/dev/CONTRACTS.md, owner: season stream). Pure, portable, deterministic: no Win32,
// no game access. The plugin feeds it a day value and reads back a SeasonState; the F5 menu edits
// SeasonConfig. Design: docs/DESIGN.md sections 2-7. Notes for users of the API: docs/dev/season_engine.md.
//
// Calendar model. Day 0 is the first day of Spring; the year is the sum of the five phase lengths and
// wraps forever. Every region runs its own copy of the calendar: its phases are stretched by
// phaseScale and then renormalised so the regional year has the same length as the global one (the
// regions never drift apart), and the whole regional calendar is shifted by dayOffset. Each phase is a
// plateau (the pure look) followed by a tail (transitionFraction of the phase) cut into
// transitionSteps equal discrete stages that lead to the next phase.
//
// Determinism. Stage selection (stageFor / stageAt) uses only + - * / floor, so the native generator
// and the Windows plugin always agree on the stage of an asset. Temperature, weather and light use
// cos/acos and may differ in the last bit between C runtimes, which is harmless.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "../ini.h"

namespace qs::season {

enum class Phase : uint8_t { Spring = 0, Summer = 1, LateSummer = 2, Autumn = 3, Winter = 4 };
constexpr int kPhaseCount = 5;
const char* toString(Phase p);
char toChar(Phase p);              // 'S','U','L','A','W' (same letters as recipe::Look)
const char* phaseNameIt(Phase p);  // "Primavera", "Estate", "Fine estate", "Autunno", "Inverno"
inline Phase nextPhase(Phase p) { return Phase((int(p) + 1) % kPhaseCount); }

enum class Region : uint8_t { Temperate = 0, NorthCold, SwampJungle, Aquatic, DesertDry, Abyss, Count };
constexpr int kRegionCount = static_cast<int>(Region::Count);
const char* toString(Region r);
Region regionFromHint(const std::string& hint);  // "shared_temperate" -> Temperate, ...
bool regionFromName(const std::string& name, Region& out);  // "NorthCold" (case-insensitive) or a hint

// Floor of the temperature model. The game turns low temperatures into cold damage (an earlier
// seasons mod killed players and NPCs), so the engine never reports less than this.
constexpr float kMinTempC = -30.0f;

enum class DaySource : uint8_t { GameClock = 0, Playtime = 1, Manual = 2 };
const char* toString(DaySource s);  // "gameclock", "playtime", "manual" (ini values)

struct RegionConfig {
    bool enabled = true;
    float dayOffset = 0.0f;       // shifts the calendar for this region (negative = earlier)
    float phaseScale[kPhaseCount] = {1, 1, 1, 1, 1};  // relative phase length multipliers
    float meanTempC = 14.0f, ampTempC = 11.0f;          // annual temperature model
    float diurnalTempC = 4.0f;    // half of the day/night swing (warmest 15:00, coldest 03:00)
    float humidity = 0.5f;        // 0 dry .. 1 wet: more rain and clouds, fewer clear days
    float leaves = 1.0f;          // falling-leaves multiplier (0 = evergreen region)
};

struct SeasonConfig {
    float phaseDays[kPhaseCount] = {8, 8, 5, 8, 9};     // S U L A W
    float transitionFraction = 0.4f;                     // tail of each phase spent blending to the next
    int transitionSteps = 3;                             // discrete stages per transition (0 = hard switch)
    float assetJitterDays = 2.0f;                        // per-asset calendar jitter (+-)
    DaySource daySource = DaySource::Playtime;
    float realMinutesPerDay = 90.0f;                     // used when daySource == Playtime (game: ~60-120)
    float startDay = 0.0f;                               // day of year at first launch / manual value
    bool weatherControl = true, snowAccumulation = true, lightControl = true, dayLengthControl = true,
         particles = true;
    float snowAccumPerHour = 0.25f, snowMeltPerHour = 0.15f;  // in-game hours
    float virtualLatitudeDeg = 45.0f;
    RegionConfig regions[static_cast<int>(Region::Count)];
    // The game's own (summer) day window: its clock runs a DAY speed from 03:00 to 18:59 and a NIGHT
    // speed from 19:00 to 02:59 (docs/recon/recon_D_web.md).
    float vanillaSunriseHour = 3.0f, vanillaSunsetHour = 19.0f;
    float breathThresholdC = 5.0f;                       // visible breath below this temperature

    void setDefaults();                                  // fills regions with DESIGN.md defaults
    // Starts from setDefaults(), then reads [Seasons] and [Region.<name>]. Malformed values keep the
    // default, out of range values are clamped; every problem is appended to `errors` (one line each).
    // Returns true when there were no errors.
    bool load(const Ini& ini, std::vector<std::string>& errors);   // [Seasons], [Region.<name>] sections
    std::string save() const;                            // ini text, round-trips with load()
    float yearLength() const;

    // Clamps every field to its valid range (also used after menu edits). Reports changes if `errors`.
    void clamp(std::vector<std::string>* errors = nullptr);

    // ---- calendar helpers (menu, CLI). Days of year are in global calendar days, 0 .. yearLength.
    float phaseLength(Phase p, Region r = Region::Temperate) const;    // days, after scale/renormalise
    float phaseStartDay(Phase p, Region r = Region::Temperate) const;  // global day of year it starts
    double dayOfYear(double day) const;                                // wraps into [0, yearLength)
    long long yearIndex(double day) const;                              // 0 for the first year
    // "Autunno, giorno 3 di 8" for the regional calendar of `r`.
    std::string dayToLabel(double day, Region r = Region::Temperate) const;
    // Absolute day of the start of phase `p` (regional calendar of `r`) in the year of `currentDay`.
    // Used by the manual mode "jump to the start of <phase>".
    double jumpToPhase(double currentDay, Phase p, Region r = Region::Temperate) const;
    // Advances the day counter according to daySource: Playtime uses real seconds, GameClock uses
    // game hours (24 per day), Manual does not move.
    double advance(double day, double realSecondsElapsed, double gameHoursElapsed) const;
    // Absolute day of the next (unjittered) stage change for region `r` after `day`; +inf if never.
    double nextStageChangeDay(double day, Region r) const;
    // Number of distinct visual stages (including the vanilla Summer plateau): kPhaseCount *
    // (1 + transitionSteps), or kPhaseCount when transitions are hard switches. Defaults: 20.
    int stageCount() const;
};

// Documented ini template with default values (Italian comments for the end user). It is
// SeasonConfig::save() of a default config, ready to be appended to QuattroStagioni.ini.
std::string defaultIniText();

// One discrete visual stage, identical to recipe::Stage plus a stable id for packs/caches.
struct VisualStage {
    Phase from = Phase::Summer, to = Phase::Summer;
    int step = 0;          // 0 = plateau of `from`; 1..transitionSteps = intermediate stages
    float t = 0.0f;        // step / (transitionSteps + 1)
    std::string id() const;  // e.g. "U", "A>W.2"
    bool operator==(const VisualStage& o) const { return from == o.from && to == o.to && step == o.step; }
    bool operator!=(const VisualStage& o) const { return !(*this == o); }
    bool isPlateau() const { return step == 0; }
    bool isVanilla() const { return step == 0 && from == Phase::Summer; }  // no texture changes
};
// Parses an id produced by VisualStage::id(). `t` is computed with `transitionSteps`.
bool parseStageId(const std::string& id, int transitionSteps, VisualStage& out);

// Pure stage functions (the Engine methods forward to these with its own day).
// Per-asset jitter in days, in [-assetJitterDays, +assetJitterDays]; positive = this asset is ahead.
float assetJitter(const SeasonConfig& cfg, uint32_t assetSeed);
// Stage of region `r` at absolute `day` (no jitter). Disabled regions: Summer plateau.
VisualStage stageAt(const SeasonConfig& cfg, Region r, double day);
// Stage of one asset (region calendar + the asset's jitter).
VisualStage stageAt(const SeasonConfig& cfg, Region r, double day, uint32_t assetSeed);
std::vector<VisualStage> allStages(const SeasonConfig& cfg);  // calendar order, from the Spring plateau
VisualStage nextStage(const SeasonConfig& cfg, const VisualStage& s);

struct WeatherWeights {     // matches the game's WeatherState flags, normalised to sum 1
    float sunny = 0, cloudy = 0, rainy = 0, storm = 0, snowy = 0, windy = 0;
};

struct SeasonState {
    double day = 0;                  // continuous day counter since start
    float dayOfYear = 0;             // 0 .. yearLength
    Phase phase = Phase::Summer;
    float phaseProgress = 0;         // 0..1 inside the phase
    VisualStage stage[static_cast<int>(Region::Count)];  // per region (disabled regions -> Summer plateau)
    float temperatureC[static_cast<int>(Region::Count)] = {};  // never below kMinTempC
    WeatherWeights weather[static_cast<int>(Region::Count)];
    float snowCover[static_cast<int>(Region::Count)] = {};  // 0..1
    float sunElevationOffsetDeg = 0;  // vs. summer
    float lightTint[3] = {1, 1, 1};   // multiplicative colour, 1 = vanilla
    float sunriseHour = 3, sunsetHour = 19;
    float fallingLeaves = 0;          // 0..1 particle intensity
    float breathThresholdC = 5.0f;

    // Additions (season stream).
    float hourOfDay = 12;             // 0..24 used for the diurnal temperature
    float saturation = 1;             // light saturation multiplier, 1 = vanilla (winter < 1)
    float dayLengthHours = 16;        // sunsetHour - sunriseHour
    // Day length is applied through the game's two clock speeds: multiply the DAY speed by dayScale
    // and the NIGHT speed by nightScale. A full day/night cycle keeps its real duration, but the share
    // of real time spent in daylight becomes dayLengthHours / 24 (winter: dayScale > 1, nightScale < 1).
    float dayScale = 1, nightScale = 1;
    bool regionActive[kRegionCount] = {};          // enabled region (false -> leave the game alone)
    Phase regionPhase[kRegionCount] = {};          // regional calendar
    float regionPhaseProgress[kRegionCount] = {};
    float dailyMeanTempC[kRegionCount] = {};       // temperature without the diurnal swing
    float leaves[kRegionCount] = {};               // falling leaves per region (fallingLeaves = Temperate)
    bool breathVisible[kRegionCount] = {};         // temperatureC < breathThresholdC (and particles on)
};

class Engine {
public:
    void configure(const SeasonConfig& cfg);
    const SeasonConfig& config() const { return cfg_; }
    // Advances the internal clock. `inGameHours` is elapsed game time (for snow), `day` the absolute day.
    // The hour of day is taken from the fractional part of `day`. Pass inGameHours < 0 to derive it
    // from the day delta (24 hours per day). A backward step or a jump of more than 4 days (or 96
    // hours) re-simulates the snow of the preceding two weeks instead of integrating. Sleeping
    // (3..72 game hours in one step) is integrated in half-hour substeps, so it stays stable.
    void update(double day, float inGameHours);
    // Same, with the game's own hour of day (0..24) when it is known (GameClock hooked).
    void update(double day, float inGameHours, float hourOfDay);
    const SeasonState& state() const { return state_; }
    // Forgets the snow history: the next update re-simulates it from the calendar.
    void resetSnow() { hasPrev_ = false; }

    // Stage for one asset (applies per-asset jitter from its seed), used by the generator and GPU swap.
    VisualStage stageFor(Region r, uint32_t assetSeed) const;
    // All stages an asset can be in, in calendar order (for cache planning).
    std::vector<VisualStage> allStages() const;
    // The next stage after `s` in calendar order.
    VisualStage nextStage(const VisualStage& s) const;

private:
    SeasonConfig cfg_;
    SeasonState state_;
    double snow_[kRegionCount] = {};
    double prevDay_ = 0, prevHour_ = 0;
    bool hasPrev_ = false;
};

}  // namespace qs::season
