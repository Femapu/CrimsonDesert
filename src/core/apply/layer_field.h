// QuattroStagioni - shared helpers of the "after compose" features (lightning, lighting, day length,
// particles). Portable, unit tested in tests/test_layer_field.cpp.
//
// The weather hook (src/win/apply/weather_win.cpp, signatures checked on 2.04.02) reaches two blocks
// the game rewrites every tick right before the hook returns:
//   weather     = [[climate mgr + 0x60] + 0x20]   WeatherConstant (precipitation +0x12C, snow +0x16C, rain +0x170)
//   atmosphere  = [[climate mgr + 0x60] + 0x28]   AtmosphereConstant (cloud cover +0xAC; sun, sky, fog: offsets TBD)
// A feature names a field of one of them in targets.ini as "<layer>:<offset>[:<count>]", e.g.
// "atmosphere:0x1A0:3" (three floats from +0x1A0). Offsets come from the probes (docs/dev/).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "../ini.h"
#include "../season/season.h"

namespace qs::apply {

enum class Layer : uint8_t { Weather = 0, Atmosphere = 1, Manager = 2 };
const char* toString(Layer l);  // "weather", "atmosphere", "manager"

struct LayerRef {
    bool valid = false;
    Layer layer = Layer::Atmosphere;
    uint32_t offset = 0;
    int count = 1;              // consecutive f32 (1..4)
    std::string text() const;   // "atmosphere:0x1A0:3" (round trip with parseLayerRef)
};
// "" -> valid=false, true (field not set). Malformed -> false with `err`.
// Offsets must be 4-aligned and below 0x10000 (manager: below 0x200000).
bool parseLayerRef(const std::string& s, LayerRef& out, std::string* err = nullptr);

// One value per phase, S U L A W (season::Phase order). "1, 1.1, 1, 0.9, 0.7".
struct PhaseValues {
    float v[season::kPhaseCount] = {1, 1, 1, 1, 1};
    float operator[](season::Phase p) const { return v[int(p)]; }
};
// Exactly 5 finite numbers, separated by ',' or spaces. false keeps `out`.
bool parsePhaseValues(const std::string& s, PhaseValues& out, std::string* err = nullptr);
std::string formatPhaseValues(const PhaseValues& p);
// Reads key `key` of `section`; missing keeps `out`, malformed keeps `out` and adds a warning line
// "<section> <key>: <why>". Values are clamped to [lo, hi].
void loadPhaseValues(const Ini& ini, const std::string& section, const std::string& key, PhaseValues& out, float lo,
                     float hi, std::vector<std::string>& warnings);

// The value at a visual stage: plateau = the phase's own, transition step k of n = lerp(from, to, t),
// t = step / (steps + 1) (VisualStage::t). Same blend as the texture packs, so light follows the leaves.
float stageValue(const PhaseValues& p, const season::VisualStage& st);
// Continuous variant: phase + progress in the phase, the tail (transitionFraction) blends to the next.
float phaseValue(const PhaseValues& p, season::Phase phase, float progress, float transitionFraction);

// Moves `cur` toward `target` by at most `maxStep` (any sign). Non-finite target: cur unchanged.
float approach(float cur, float target, float maxStep);

// Small deterministic RNG (xorshift32) for the features (tests reproduce sequences).
struct Rng {
    uint32_t s = 0x9E3779B9u;
    void seed(uint32_t v) { s = v ? v : 0x9E3779B9u; }
    uint32_t next();
    float uniform();  // [0, 1)
};

}  // namespace qs::apply
