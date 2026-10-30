// QuattroStagioni - applies the season engine to the game's DATA fields (portable controller).
//
// Why portable: every decision (which value goes where, rate limiting, "value does not stick",
// restore-on-stop bookkeeping, the deterministic weather schedule, the game clock reader) is plain
// logic, unit tested natively in tests/test_apply.cpp against a fake memory. The Win32 side
// (src/win/apply/) only finds the objects (probe helpers) and provides a guarded Memory: reads via
// VirtualQuery + ReadProcessMemory, writes ONLY to committed PAGE_READWRITE data pages, verified by
// readback. It never patches code bytes and never touches the protector section.
//
// Life of a target (one [target.*] section, per bound instance of its object):
//   feature off / region disabled  -> original restored, target idle ("off")
//   object not found               -> "waiting" (the Win32 resolver keeps searching)
//   first tick with the object     -> the current game value is saved as the ORIGINAL
//   every tick                     -> desired value from SeasonState, clamped to Min/Max, moved
//                                     toward it at most MaxRate per second, written and verified
//   game rewrote our value         -> scale/offset targets re-base on the new original; after
//                                     AnomalyLimit consecutive ticks the target is disabled
//                                     ("value not sticking") and its original restored
//   object vtable changed          -> object lost: nothing is written to it any more, rebinding
//                                     reads a fresh original
//   stop                           -> every saved original is written back (bits outside the
//                                     mod's Bit/Mask are preserved)
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "../season/season.h"
#include "targets.h"

namespace qs::apply {

// Game memory as seen by the controller. Addresses are absolute.
class Memory {
public:
    virtual ~Memory() = default;
    virtual bool read(uint64_t addr, void* out, size_t n) = 0;
    // Must refuse anything but committed read-write data and verify the store by reading it back.
    virtual bool write(uint64_t addr, const void* in, size_t n, std::string* why) = 0;
};

// ------------------------------------------------------------------------------- weather schedule
// Deterministic weather sequence from the seasonal weights, like the game's StageInfo_Weather
// (_weatherIngTime, _weatherStartBlendTime): game time is cut into slots of HoldHours; each slot
// keeps the previous weather with probability Persistence (when still possible) or draws a new one
// from the weights with a hash of (seed, slot), so the same calendar always gives the same weather.
// The first BlendHours of a slot blend from the previous weather.
struct WeatherMix {
    Weather current = Weather::Sunny, previous = Weather::Sunny;
    float blend = 1.0f;                  // 0 = previous, 1 = current
    float mix[kWeatherCount] = {1, 0, 0, 0, 0, 0};
    long long slot = 0;
    double slotStartHours = 0;
    float precipitation() const { return mix[int(Weather::Rainy)] + mix[int(Weather::Storm)] + mix[int(Weather::Snowy)]; }
    float rain() const { return mix[int(Weather::Rainy)] + mix[int(Weather::Storm)]; }
};

class WeatherScheduler {
public:
    void configure(double holdHours, double blendHours, double persistence, uint32_t seed);
    // hours: absolute game hours (season day * 24). Weights need not be normalised.
    const WeatherMix& update(double hours, const season::WeatherWeights& w);
    const WeatherMix& mix() const { return mix_; }
    void reset() { have_ = false; }
    // Exposed for tests: the weather drawn for `slot` given the previous one (or none if prev < 0).
    Weather choose(long long slot, const season::WeatherWeights& w, int prev) const;

private:
    double hold_ = 6, blend_ = 0.75, persistence_ = 0.35;
    uint32_t seed_ = 0;
    bool have_ = false;
    WeatherMix mix_;
};

// ------------------------------------------------------------------------------- game clock
// Turns periodic samples of the game's hour (and day counter, when found) into elapsed game hours
// for the season tick. Midnight wraps are detected when only the hour is known (samples at 10 Hz);
// a backward jump (save loaded, clock reset) or a jump of more than 10 days counts as 0 hours.
class ClockTracker {
public:
    void sample(bool haveDay, double dayCounter, bool haveHour, double hourOfDay);
    bool known() const { return known_; }
    double hourOfDay() const { return hour_; }
    double day() const { return haveDay_ ? day_ + hour_ / 24.0 : -1.0; }
    // day: game day counter + hour/24 (-1 without a day counter); hours: game hours elapsed since
    // the previous take (0 on the first call). False while the clock is unknown.
    bool take(double& day, double& hoursElapsed);
    void reset();

private:
    bool known_ = false, haveDay_ = false, havePrev_ = false;
    double hour_ = 0, day_ = 0, prevTotal_ = 0, prevHour_ = 0, pending_ = 0;
};

// ------------------------------------------------------------------------------- value mapping
// Season value of a target in "season units" (before Scale/Bias), false when there is none.
// hourOfDay: the game's hour when the clock is known, else the engine's.
bool seasonValue(const TargetSpec& t, const TargetsConfig& cfg, const season::SeasonState& s, const season::SeasonConfig& sc,
                 const WeatherMix& wm, double hourOfDay, double& out);
// Master switch of a feature: [Features] ini key AND the SeasonConfig flag of the F5 menu, and for
// region dependent features (weather, snow, particles) the applied region must be enabled.
bool featureOn(Feature f, const TargetsConfig& cfg, const season::SeasonState& s, const season::SeasonConfig& sc);

// Field encoding helpers (little endian, as in memory). Integers are rounded and saturated.
void encodeField(FieldKind k, double v, uint8_t out[8]);
double decodeField(FieldKind k, const uint8_t* in);

// ------------------------------------------------------------------------------- status
enum class TargetState : uint8_t { Disabled, Off, Waiting, Active, Error };
const char* toString(TargetState s);
enum class FeatureState : uint8_t { Inactive, NotFound, Active, Error };
const char* toString(FeatureState s);

struct TargetStatus {
    std::string name;
    Semantic semantic = Semantic::Hour;
    Weather weather = Weather::Sunny;
    Feature feature = Feature::Clock;
    Mode mode = Mode::Write;
    TargetState state = TargetState::Disabled;
    uint32_t instances = 0;
    uint64_t address = 0;                // field address of the first instance
    bool haveOriginal = false;
    double original = 0, desired = 0, current = 0;  // game units
    uint32_t anomalies = 0;              // total since start
    std::string error;
};

struct FeatureStatus {
    FeatureState state = FeatureState::Inactive;
    int enabledTargets = 0, activeTargets = 0, errorTargets = 0;
};

// Snapshot for the F5 menu (copied under the Win32 module's lock).
struct ApplyStatus {
    bool running = false;                // apply thread alive
    std::string phase;                   // "waiting start delay", "running", "stopped", "disabled"
    FeatureStatus features[kFeatureCount];
    std::vector<TargetStatus> targets;
    WeatherMix weather;
    bool weatherScheduled = false;
    bool clockKnown = false;
    double gameHour = 0, gameDay = -1;
    uint64_t ticks = 0, writes = 0;
};

// ------------------------------------------------------------------------------- controller
class Applier {
public:
    void configure(const TargetsConfig& cfg);
    const TargetsConfig& config() const { return cfg_; }

    // Object binding from the resolver. expectVtable 0 = no vtable guard (plain struct).
    void bindObject(int obj, const std::vector<uint64_t>& addrs, uint64_t expectVtable);
    void unbindObject(int obj);          // object gone: forget originals, write nothing
    bool objectBound(int obj) const;
    std::vector<uint64_t> objectInstances(int obj) const;
    // An enabled target whose feature is on (as of the last tick) needs this object.
    bool objectNeeded(int obj) const;

    // One apply step. realDt: real seconds since the previous tick (rate limits). Returns the
    // objects lost in this tick (already unbound).
    std::vector<int> tick(const season::SeasonState& s, const season::SeasonConfig& sc, double realDt, Memory& mem);
    // Writes every saved original back (objects still valid); returns the number restored.
    int restoreAll(Memory& mem);

    void status(ApplyStatus& out) const;  // fills features, targets, weather, clock, counters
    ClockTracker& clock() { return clock_; }
    const WeatherMix& weather() const { return sched_.mix(); }
    std::vector<std::string> takeLog();  // messages for QuattroStagioni.log since the last call

private:
    struct FieldRt {
        bool attached = false;
        double original = 0;
        uint32_t originalBits = 0;
        bool wrote = false;
        double lastWritten = 0;          // decoded value as stored
        uint32_t lastWrittenBits = 0;
        double lastValue = 0;            // rate limiter position (game units)
        uint32_t stickStreak = 0;
    };
    struct TargetRt {
        TargetState state = TargetState::Disabled;
        std::vector<FieldRt> inst;
        uint32_t failStreak = 0, readStreak = 0, anomalies = 0;
        double desired = 0, current = 0;
        std::string error;
    };
    struct ObjectRt {
        std::vector<uint64_t> addrs;
        uint64_t vtableX = 0;            // expected vtable ^ key (never stored in clear: heap scans)
        bool bound = false;
        bool needed = false;
    };

    void log(const std::string& s);
    void detach(size_t ti, Memory* mem, bool restore);
    void fail(size_t ti, Memory& mem, const std::string& why);
    bool restoreField(const TargetSpec& t, uint64_t addr, const FieldRt& f, Memory& mem);
    void applyTarget(size_t ti, const season::SeasonState& s, const season::SeasonConfig& sc, double dt, Memory& mem);
    bool weatherDriverActive(size_t except) const;

    TargetsConfig cfg_;
    std::vector<TargetRt> trt_;
    std::vector<ObjectRt> ort_;
    WeatherScheduler sched_;
    bool schedUsed_ = false;
    ClockTracker clock_;
    double clockHour_ = 0, clockMinute_ = 0, clockDay_ = 0;
    bool haveHour_ = false, haveMinute_ = false, haveDay_ = false;
    uint64_t ticks_ = 0, writes_ = 0;
    std::vector<std::string> log_;
};

}  // namespace qs::apply
