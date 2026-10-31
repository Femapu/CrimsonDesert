// QuattroStagioni - lightning in storms, by season (portable part, unit tested in tests/test_lightning.cpp).
//
// The game already has storms (HUD Storm bit 6) and the season's weather adds Storm spells. This
// module decides, at the start of every storm, whether it has lightning (ChancePerStorm of the phase)
// and then fires strikes as a Poisson process: StrikesPerHour of the phase (per GAME hour) times the
// storm's intensity, at least MinGapSec real seconds apart. Phase values blend in the transitions
// (phaseValue, same tail as the textures). Never in the desert, never with the feature off or the
// region off. The thunder stays the game's own: the plugin only drives a visual field.
//
//   LightningConfig    [Lightning] keys of QuattroStagioni.ini, every key optional
//   LightningTarget    [lightning] section of QuattroStagioni\targets.ini (WHERE to write, off by default)
//   LightningDirector  storms, strikes, level() 0..1 and the strike pulse; deterministic with seed()
// The Win32 side (src/win/apply/lightning_win.cpp) feeds it and writes after the weather compose.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "../ini.h"
#include "../season/season.h"
#include "layer_field.h"

namespace qs::apply {

struct LightningConfig {
    bool enabled = true;          // [Lightning] Enabled
    PhaseValues chancePerStorm;   // ChancePerStorm: share of storms with lightning, S U L A W (0..1)
    PhaseValues strikesPerHour;   // StrikesPerHour: strikes per game hour at full intensity, S U L A W
    float minGapSec = 3.0f;       // MinGapSec: real seconds between two strikes at least
    float pulseMs = 250.0f;       // PulseMs: length of a strike's flash (real milliseconds)
    float rampSec = 6.0f;         // RampSec: real seconds for level() to go 0 -> 1
    bool logStrikes = false;      // LogStrikes: one log line per strike

    LightningConfig();            // Stefano's rules (see lightningIniTemplate)
    // Missing keys keep the defaults; malformed values keep the default and add a warning line.
    void load(const Ini& ini, std::vector<std::string>& warnings);
};

std::string lightningIniTemplate();  // documented [Lightning] section with the defaults

enum class LightningMode : uint8_t { Level = 0, Pulse = 1 };
const char* toString(LightningMode m);  // "level", "pulse"

// [lightning] of targets.ini: Enabled=0 by default; Field=<LayerRef>; Mode=level|pulse; Value=<f32>.
// Nothing is restored on exit: the game composes the layers anew every tick.
struct LightningTarget {
    bool enabled = false;
    LayerRef field;               // valid=false: not found yet (the logic still runs and logs)
    LightningMode mode = LightningMode::Level;
    float value = 1.0f;
    bool usable() const { return enabled && field.valid; }
    // Malformed keys: warning, the target stays off (enabled=false) or the default is kept.
    void load(const Ini& ini, std::vector<std::string>& warnings);
};

struct LightningInput {
    bool storm = false;           // a storm now (season's Storm spell or the HUD Storm bit)
    float intensity = 1.0f;       // 0..1 (the spell's; 1 for the game's own storm)
    bool allowed = true;          // enabled, F5 on, region on, not in the desert (false: no strikes, level 0)
    season::Phase phase = season::Phase::Summer;
    float progress = 0;           // 0..1 in the phase
    float transitionFraction = 0.4f;
};

class LightningDirector {
public:
    static constexpr float kStormGraceSec = 20.0f;  // real seconds without a storm signal before it ends

    void configure(const LightningConfig& c) { cfg_ = c; }
    const LightningConfig& config() const { return cfg_; }
    void seed(uint32_t s) { rng_.seed(s); }

    // One step: real seconds (pulse, gaps, ramp) and game hours (strike rate; 0 while the game's clock
    // stands still or jumps). Returns the strikes fired in this step (0 or 1).
    int advance(double realSec, double gameHours, const LightningInput& in);

    float level() const { return level_; }               // 0..1, smooth
    bool strikeActive() const { return pulseLeft_ > 0; } // inside a strike's flash
    bool inStorm() const { return inStorm_; }
    bool stormHasLightning() const { return inStorm_ && hasLightning_; }
    uint64_t strikes() const { return strikes_; }
    uint64_t storms() const { return storms_; }
    double sinceLastStrikeSec() const { return sinceStrike_; }  // large before the first strike
    float rate() const { return rate_; }                 // strikes per game hour now (0 outside a lightning storm)
    float chance() const { return chance_; }             // ChancePerStorm of the current phase blend
    std::vector<std::string> takeLog();                  // storm begin / end lines (no prefix)

    // The phase blend of a value (exposed for the tests): phaseValue with the input's season.
    static float seasonal(const PhaseValues& p, const LightningInput& in);

private:
    float exposure();  // Exp(1) draw

    LightningConfig cfg_;
    Rng rng_;
    bool inStorm_ = false, hasLightning_ = false;
    double offFor_ = 0;           // real seconds without the storm signal
    double sinceStrike_ = 1e9;
    double need_ = 0;             // remaining exposure before the next strike
    float level_ = 0, pulseLeft_ = 0, rate_ = 0, chance_ = 0;
    uint64_t strikes_ = 0, storms_ = 0;
    std::vector<std::string> log_;
};

}  // namespace qs::apply
