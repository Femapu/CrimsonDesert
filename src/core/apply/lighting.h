// QuattroStagioni - seasonal light and sky: multipliers over the game's own sun, ambient, fog and sky.
//
// Portable part (unit tested in tests/test_lighting.cpp). The Win32 side (src/win/apply/lighting_win.cpp)
// runs after the game composed its atmosphere every tick (weatherAddPostCompose) and writes
// game value * multiplier into the fields named in targets.ini [lighting] (offsets from the probes,
// docs/dev/luce.md). Summer = 1 everywhere: the game's own light.
//
//   LightingConfig   [Lighting] of QuattroStagioni.ini (every key optional), one PhaseValues per channel
//   LightingModel    multipliers of a visual stage (stageValue: the 15 transition stages blend like the
//                    texture packs), smoothed in time (RampPerSec)
//   applyScale       game * lerp(mult, 1, bypass): bypass 1 (desert, off) = the game's own value
//   LightingTargets  targets.ini [lighting]: which game fields get which channel
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "../ini.h"
#include "../season/season.h"
#include "layer_field.h"

namespace qs::apply {

enum class LightChannel : uint8_t {
    SunIntensity = 0, SunR, SunG, SunB,
    AmbientIntensity, AmbientR, AmbientG, AmbientB,
    FogDensity,
    SkyR, SkyG, SkyB,
};
constexpr int kLightChannels = 12;
const char* lightChannelKey(int ch);  // ini key: "SunIntensity", "SunColorR", ... "SkyB"

struct LightingConfig {
    bool enabled = true;          // [Lighting] Enabled
    float rampPerSec = 0.01f;     // RampPerSec: largest multiplier change per real second
    float logIntervalSec = 60.0f; // LogIntervalSec: multipliers in the log (0 = never)
    PhaseValues ch[kLightChannels];

    LightingConfig();             // defaults (see lightingIniTemplate)
    // Missing keys keep the defaults; malformed values keep the default and add a warning line;
    // out of range values are clamped (multipliers 0.5..1.5, fog 0.5..2).
    void load(const Ini& ini, std::vector<std::string>& warnings);
};

std::string lightingIniTemplate();  // documented [Lighting] section with the defaults (Italian)

struct LightingMultipliers {
    float v[kLightChannels] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
    float operator[](LightChannel c) const { return v[int(c)]; }
};

// game * lerp(mult, 1, bypass). Non-finite game value: returned unchanged (left to the game);
// non-finite multiplier or bypass: the game's value.
float applyScale(float gameValue, float mult, float bypass);

class LightingModel {
public:
    void configure(const LightingConfig& c) { cfg_ = c; }
    const LightingConfig& config() const { return cfg_; }
    // Target multipliers of a visual stage.
    LightingMultipliers target(const season::VisualStage& st) const;
    // Moves the current multipliers toward the stage's (on) or toward 1 (off) by RampPerSec * dtSec.
    // Starts from 1 (the game's own light), so the season's light fades in.
    void step(const season::VisualStage& st, bool on, float dtSec);
    const LightingMultipliers& current() const { return cur_; }

private:
    LightingConfig cfg_;
    LightingMultipliers cur_;
};

// ---- targets.ini [lighting] ---------------------------------------------------------------------
// Keys (LayerRef, see layer_field.h): SunColor=, AmbientColor=, SkyColor= (3 or 4 floats, a 4th is
// left alone), SunIntensity=, AmbientIntensity=, FogDensity= (1 float). Safety limits <Key>Min= and
// <Key>Max= (defaults 0 and 100000): a game value outside them turns the feature off.
enum class LightField : uint8_t { SunColor = 0, SunIntensity, AmbientColor, AmbientIntensity, FogDensity, SkyColor };
constexpr int kLightFields = 6;
const char* lightFieldKey(int f);  // "SunColor", ...
int lightFieldComponents(int f);   // 3 (colours) or 1
int lightFieldChannel(int f);      // first LightChannel of the field

struct LightingTarget {
    LayerRef ref;                  // valid=false: not set
    float minV = 0, maxV = 100000;
};
struct LightingTargets {
    bool enabled = false;          // [lighting] Enabled (default 0)
    LightingTarget field[kLightFields];
    int count() const;             // fields set
    // Reads [lighting]; bad keys are dropped with a warning line.
    void load(const Ini& ini, std::vector<std::string>& warnings);
};

}  // namespace qs::apply
