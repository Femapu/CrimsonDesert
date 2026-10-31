// QuattroStagioni - seasonal particles: denser winter flakes, falling leaves in autumn, pollen in spring.
//
// Portable part (unit tested in tests/test_particles.cpp); the Win32 side is src/win/apply/particles_win.cpp.
//
// Winter flakes use only the snow field of the WeatherConstant checked on 2.04.02 (+0x16C, the same
// float the weather module writes). They never create snow: the snow already falling (above
// SnowMinActive) is raised to max(snow, min(1, snow * WinterFlakeBoost)) and, while there is
// precipitation, to at least WinterFlakeMin. All of it scales with the winter intensity (0 outside
// winter) and fades out in the desert (bypass 1 = the game's own value).
//
// Leaves and pollen: the game field that drives them is not known yet. The module is ready but off:
// section [particles] of QuattroStagioni\targets.ini names the field (Leaves= / Pollen= as
// "<layer>:<offset>", see layer_field.h) once a probe found it.
#pragma once
#include <string>
#include <vector>

#include "../ini.h"
#include "../season/season.h"
#include "layer_field.h"

namespace qs::apply {

struct ParticlesConfig {
    bool enabled = true;            // [Particles] Enabled
    bool winterFlakes = true;       // WinterFlakes
    float winterFlakeBoost = 1.25f; // WinterFlakeBoost: multiplier of the falling snow (1..3)
    float winterFlakeMin = 0.35f;   // WinterFlakeMin: minimum snow while it snows and there is precipitation (0..1)
    float snowMinActive = 0.05f;    // SnowMinActive: below this the game is not snowing: untouched (0.001..1)
    PhaseValues leavesPerPhase{{0.0f, 0.05f, 0.35f, 1.0f, 0.1f}};  // LeavesPerPhase (S U L A W, 0..1)
    PhaseValues pollenPerPhase{{1.0f, 0.25f, 0.0f, 0.0f, 0.0f}};   // PollenPerPhase (S U L A W, 0..1)
    bool leavesFromSeason = true;   // no LeavesPerPhase in the ini: SeasonState.leaves[Temperate]
    float rampPerSec = 0.2f;        // RampPerSec: max change of an intensity per real second (0 = at once)
    void load(const Ini& ini, std::vector<std::string>& warnings);
};

// Documented [Particles] section (Italian comments for the end user), default values.
std::string particlesIniTemplate();

// Snow value to write. snow/precip: the composed values just read; winterK: 0..1 winter intensity
// (already ramped); bypass: 0..1 desert fade (1 = game's own). Returns `snow` unchanged when it is not
// snowing, outside winter, in the desert, or when `snow` is not finite. Never lower than `snow`, never
// above 1 (unless the game itself wrote more).
float flakes(float snow, float precip, float winterK, float bypass, const ParticlesConfig& cfg);

// 0..1 intensities at a visual stage (same blend as the texture packs). regionMul: RegionConfig::leaves.
float leavesIntensity(const ParticlesConfig& cfg, const season::VisualStage& st, float regionMul = 1.0f);
float pollenIntensity(const ParticlesConfig& cfg, const season::VisualStage& st);

// ---- [particles] of targets.ini: the game fields of leaves and pollen (off until found) ---------
enum class ParticleMode : unsigned char { Scale = 0, Write = 1 };
struct ParticleFields {
    bool enabled = false;           // Enabled
    LayerRef leaves, pollen;        // Leaves= / Pollen= ("" = not found yet)
    ParticleMode mode = ParticleMode::Scale;  // Mode=scale|write
    float value = 2.0f;             // Value: scale = multiplier at full intensity; write = value at full intensity
    float maxValue = 1.0f;          // Max: upper limit of what is written (0.01..1000)
    // Malformed keys add a warning and leave that field off.
    void load(const Ini& ini, std::vector<std::string>& warnings);
};
// Value for one field: game value `game`, intensity 0..1, desert fade `bypass` (1 = game's own).
// Scale: game * (1 + (Value-1) * intensity); Write: Value * intensity. Non-finite game: unchanged.
// Result in [0, max(game, Max)].
float particleField(float game, float intensity, float bypass, const ParticleFields& f);

}  // namespace qs::apply
