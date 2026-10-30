// QuattroStagioni - targets.ini schema: WHERE the season engine writes into the game (portable).
//
// Why a file and not code: the addresses and field offsets of the game's weather, clock, light and
// particle systems are found at runtime by the probes (docs/recon/probe_plan.md) on Stefano's PC.
// Once they are known, the local session only fills bin64\scripts\QuattroStagioni\targets.ini and
// relaunches the game: no recompiling. Documented template:
// dist/bin64/scripts/QuattroStagioni/targets.ini; guide (Italian): docs/dev/appliers.md.
//
// Two kinds of sections:
//   [object.<name>]  how to find one game object (RTTI class + instance discovery, reusing the
//                    probe framework: heap scan by vtable, global pointer chain from a signature or
//                    RVA, or capture of `this` through a vtable slot / guarded MinHook)
//   [target.<name>]  one DATA field of an object: offset, type, semantic (what season value goes
//                    there), mode (write / read / scale / offset), limits, rate limit, enabled flag
// Everything is disabled by default ([General] Enabled=0 and every target Enabled=0).
#pragma once
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "../ini.h"
#include "../probe/pattern.h"

namespace qs::apply {

// Feature groups. The first five are the SeasonConfig master switches of the F5 menu
// (weatherControl, snowAccumulation, lightControl, dayLengthControl, particles); Clock is the
// read-only game clock used as the season day source.
enum class Feature : uint8_t { Weather = 0, Snow, Light, DayLength, Particles, Clock, Count };
constexpr int kFeatureCount = static_cast<int>(Feature::Count);
const char* toString(Feature f);  // "weather", "snow", "light", "daylength", "particles", "clock"
bool featureFromString(const std::string& s, Feature& out);

// The six WeatherState flags of the game (recon C), same order as season::WeatherWeights.
enum class Weather : uint8_t { Sunny = 0, Cloudy, Rainy, Storm, Snowy, Windy, Count };
constexpr int kWeatherCount = static_cast<int>(Weather::Count);
const char* toString(Weather w);  // "sunny" ...
bool weatherFromString(const std::string& s, Weather& out);

enum class Semantic : uint8_t {
    // clock (read only)
    Hour,                 // hour of day 0..24 (float hours, or integer hours with a Minute target)
    Minute,               // 0..59, added to Hour when both are found
    DayCounter,           // game day counter
    // day length
    TimeScaleDay,         // clock speed during the day window: x dayScale
    TimeScaleNight,       // clock speed during the night window: x nightScale
    TimeScaleCurrent,     // single clock speed field: dayScale or nightScale by hour of day
    // weather
    WeatherManualFlag,    // 1 while the mod drives the weather (written only once another weather target is active)
    WeatherStateFlags,    // WeatherState bit mask of the scheduled weather ([Weather] Flag<state>)
    WeatherIndex,         // index of the scheduled weather ([Weather] Index<state>)
    WeatherWeight,        // seasonal probability weight of one state (the game's own chooser picks)
    WeatherMix,           // 0..1 blend of one state in the deterministic schedule
    Precipitation,        // 0..1 rain + storm + snow of the schedule
    RainAmount,           // 0..1 rain + storm
    SnowRate,             // 0..1 falling snow of the schedule
    TemperatureOffset,    // C, regional temperature minus the region's summer maximum (<= 0)
    Temperature,          // C, regional temperature (never below season::kMinTempC)
    // snow
    SnowAmount,           // 0..1 accumulated snow cover (season engine snowCover)
    // light
    SunElevationOffset,   // degrees vs. summer (negative in winter)
    LightTintR, LightTintG, LightTintB,  // multiplicative tint, 1 = vanilla (default mode: scale)
    Saturation,           // multiplicative saturation, 1 = vanilla
    // particles
    LeavesParticles,      // 0..1 falling leaves intensity of the region
    BreathVisible,        // 1 when breath is visible (temperature below the threshold)
    Count
};
const char* toString(Semantic s);   // ini spelling, e.g. "time_scale_day"
// Parses "weather_weight_rainy" etc. (sets *weather for WeatherWeight / WeatherMix).
bool semanticFromString(const std::string& s, Semantic& out, Weather* weather);
Feature featureOf(Semantic s);
bool isReadOnlySemantic(Semantic s);  // clock semantics

enum class FieldKind : uint8_t { F32, F64, I32, U32, U8, Flags32 };
const char* toString(FieldKind k);
bool fieldKindFromString(const std::string& s, FieldKind& out);
size_t fieldBytes(FieldKind k);

enum class Mode : uint8_t { Write, Read, Scale, Offset };
const char* toString(Mode m);
bool modeFromString(const std::string& s, Mode& out);

enum class InstanceKind : uint8_t { Scan, Global, Capture };
const char* toString(InstanceKind k);
enum class HookKind : uint8_t { Vtable, MinHook };
enum class MultiplePolicy : uint8_t { Refuse, First, All };

// Plausibility check on a candidate object: the field must read inside [min, max].
struct ObjectCheck {
    uint32_t offset = 0;
    FieldKind kind = FieldKind::F32;
    double minV = 0, maxV = 0;
};

struct ObjectSpec {
    std::string name;                    // section name without "object."
    std::string className;               // optional for Global (verification), required for Scan/Capture-vtable
    InstanceKind instance = InstanceKind::Scan;
    // Global: Signature (+SigOffset, Resolve) or Rva (honoured only with [General] ExpectTimestamp),
    // then Chain (p = *(p + off) for each element). An empty chain means the resolved address is the
    // object itself (a static struct in the exe's data section).
    std::string signatureText;
    probe::Pattern signature;
    bool sigInData = false;
    int64_t sigOffset = 0;
    probe::RipSpec resolve;
    uint64_t rva = 0;
    std::vector<int64_t> chain;
    // Capture: Class + Slot with Hook=vtable, or Hook=minhook on Class+Slot / Signature / Rva.
    HookKind hook = HookKind::Vtable;
    int slot = -1;
    bool captureRdx = false;
    bool keepHook = false;               // keep the capture hook after the object is found
    // Scan / Capture: what to do when several candidates pass the checks.
    MultiplePolicy multiple = MultiplePolicy::Refuse;
    uint32_t maxInstances = 4;           // with Multiple=all
    uint32_t scanMaxMB = 4096;
    std::vector<ObjectCheck> checks;     // Check=0x10:f32:0:24, 0x14:u32:0:100000
    bool checkVtable = true;             // drop the object when its vtable pointer changes
};

struct TargetSpec {
    std::string name;                    // section name without "target."
    bool enabled = false;
    std::string object;                  // [object.<name>] it lives in
    int objectIndex = -1;                // resolved by parseTargets
    Semantic semantic = Semantic::Hour;
    Weather weather = Weather::Sunny;    // for WeatherWeight / WeatherMix
    Feature feature = Feature::Clock;    // featureOf(semantic) unless Feature= overrides it
    uint32_t offset = 0;
    FieldKind kind = FieldKind::F32;
    int bit = -1;                        // u8/u32/flags32: single boolean bit
    uint32_t mask = 0xFFFFFFFFu;         // flags32: bits owned by the mod (others are preserved)
    Mode mode = Mode::Write;
    // Value conversion. Write: game = v*Scale + Bias. Offset: game = original + v*Scale + Bias.
    // Scale: game = original * (1 + (v - 1) * Scale) (Scale = strength, 1 = full). Read: v = (game - Bias)/Scale.
    double scale = 1.0, bias = 0.0;
    double minV = NAN, maxV = NAN;       // clamp of the final game value (read: plausibility range)
    double maxRate = 0.0;                // max change of the game value per real second (0 = unlimited)
    double tolerance = -1.0;             // readback/stick tolerance (<0: automatic)
    bool restore = true;                 // restore the original value on stop / feature off
};

struct TargetsConfig {
    bool enabled = false;
    uint32_t startDelayMs = 30000;
    uint32_t intervalMs = 100;           // apply tick (10 Hz)
    uint32_t resolveRetryMs = 5000;      // how often missing objects are searched again
    uint32_t anomalyLimit = 5;           // consecutive anomalies before a target is disabled
    bool allowHooks = false;             // master switch for Instance=capture
    uint32_t expectTimestamp = 0;        // exe TimeDateStamp that Rva= values were written for
    int logLevel = 1;                    // 2 = log every write
    int region = 0;                      // season::Region whose values are applied (Temperate)
    bool features[kFeatureCount] = {true, true, true, true, true, true};  // [Features] ini switches

    // [Weather] deterministic schedule
    double holdHours = 6.0;              // game hours of one weather slot (like StageInfo_Weather _weatherIngTime)
    double blendHours = 0.75;            // start blend from the previous weather (_weatherStartBlendTime)
    double persistence = 0.35;           // probability of keeping the previous weather when it is still possible
    uint32_t seed = 0x5EA50;
    uint32_t flag[kWeatherCount] = {0x20, 0x10, 0x01, 0x08, 0x02, 0x04};  // WeatherState bits (recon B/C, to confirm)
    int index[kWeatherCount] = {1, 0, 3, 2, 4, 5};  // enum string order in the exe: Cloudy Sunny Storm Rainy Snowy Windy

    std::vector<ObjectSpec> objects;
    std::vector<TargetSpec> targets;
};

// Parses an Ini. Invalid objects/targets are dropped with one line each in `errors`; the rest is
// used. Returns false only when [General] is missing.
bool parseTargets(const Ini& ini, TargetsConfig& out, std::vector<std::string>& errors);
bool parseTargetsText(const std::string& text, TargetsConfig& out, std::vector<std::string>& errors);

// Helpers exposed for tests.
bool parseCheckList(const std::string& s, std::vector<ObjectCheck>& out, std::string* err);
bool parseDouble(const std::string& s, double& out);

}  // namespace qs::apply
