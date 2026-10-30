// QuattroStagioni - winter snow: seasonal control of the game's SnowStart / SnowFull thresholds.
//
// What the game does (local probes on the 2.04.00 hotfix exe, read only): two global console
// variables of class pa::GlobalVariableFloat, SnowStart (vanilla -5) and SnowFull (vanilla -20),
// float value at +0x38 and default at +0x3C. The engine shows snow where the climate temperature is
// <= SnowStart and full coverage at <= SnowFull. With the vanilla values nothing in the world is cold
// enough; in winter the mod raises both thresholds so temperate and cold zones get snow. Desert
// climate temperatures are far above any allowed value (MaxSnowStart caps the user's settings), so
// the desert stays snow-free.
//
// This file is the portable part (unit tested in tests/test_snow.cpp against a fake memory):
//   SnowConfig        [Snow] keys of QuattroStagioni.ini, every key optional (defaults below)
//   winterIntensity   0..1 from the season engine: ramps in over the second half of the A>W
//                     transition, 1 in winter, ramps out over the first half of W>S, 0 otherwise
//   snowTargets       originals blended toward the configured winter thresholds by the intensity
//   SnowController    attach (validates the originals), rate limited writes with readback,
//                     "game changed our value" detection, restore of the originals
// The Win32 side (src/win/apply/snow_win.cpp) finds the two objects (signature + RTTI class) and
// runs the controller in its own thread.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "../ini.h"
#include "../season/season.h"
#include "applier.h"

namespace qs::apply {

constexpr float kSnowMinC = -60.0f, kSnowMaxC = 30.0f;  // plausible range of the two cvars
constexpr float kSnowStartVanilla = -5.0f, kSnowFullVanilla = -20.0f;

struct SnowConfig {
    bool enabled = true;           // [Snow] Enabled
    float winterStart = 2.0f;      // WinterSnowStart: SnowStart at full winter intensity (C)
    float winterFull = -6.0f;      // WinterSnowFull: SnowFull at full winter intensity (C)
    float maxStart = 4.0f;         // MaxSnowStart: safety cap (no snow in the desert), hard limit 10
    float maxRate = 0.5f;          // MaxRate: max change per real second (C), 0 = unlimited
    uint32_t startDelayMs = 20000; // StartDelayMs: wait before touching the game
    // Effective winter values after the caps: start <= maxStart, full <= start - 1, both in range.
    float effectiveStart() const;
    float effectiveFull() const;
    // Missing keys keep the defaults (an existing QuattroStagioni.ini needs no edit). Malformed
    // values keep the default and add a line to `warnings`.
    void load(const Ini& ini, std::vector<std::string>& warnings);
};

constexpr float kSnowHardMaxStart = 10.0f;  // MaxSnowStart itself can not go above this

// Documented [Snow] section (Italian comments for the end user), default values.
std::string snowIniTemplate();

// 0..1 winter intensity of one phase position. transitionFraction: SeasonConfig::transitionFraction.
float winterIntensity(season::Phase phase, float phaseProgress, float transitionFraction);
// Same for the region's calendar in a SeasonState (disabled region -> 0).
float winterIntensity(const season::SeasonState& s, const season::SeasonConfig& sc,
                      season::Region r = season::Region::Temperate);

struct SnowThresholds {
    float start = kSnowStartVanilla, full = kSnowFullVanilla;
};
// Values for `intensity` (0 = originals exactly, 1 = configured winter values).
SnowThresholds snowTargets(const SnowThresholds& original, const SnowConfig& cfg, float intensity);

enum class SnowState : uint8_t { Waiting, Idle, Active, Disabled };
const char* toString(SnowState s);

struct SnowStatus {
    SnowState state = SnowState::Waiting;
    std::string reason;            // why Disabled / Waiting
    bool attached = false;
    SnowThresholds original, current, target;
    float intensity = 0;
    uint32_t writes = 0, anomalies = 0;
};

class SnowController {
public:
    struct Field {
        uint64_t value = 0;        // address of the float value (+0x38)
        uint64_t def = 0;          // address of the float default (+0x3C), 0 = no default check
    };
    void configure(const SnowConfig& cfg) { cfg_ = cfg; }
    const SnowConfig& config() const { return cfg_; }

    // Reads both values (and defaults), checks they are finite, inside [kSnowMinC, kSnowMaxC] and
    // equal to their defaults, SnowFull < SnowStart. On success saves them as ORIGINALS.
    // On failure the controller is Disabled with the reason (nothing was written).
    bool attach(const Field& start, const Field& full, Memory& mem);
    // Disable without touching memory (e.g. signature or class check failed before attach).
    void disable(const std::string& why);

    // One step. on: [Snow] Enabled AND the F5 snow switch. intensity: winterIntensity().
    // realDt: real seconds since the previous tick. label: season text for the log ("Inverno, giorno 3").
    // Turning off restores the originals at once; otherwise values move at most MaxRate per second.
    void tick(bool on, float intensity, double realDt, Memory& mem, const std::string& label = "");
    // Writes the originals back when we changed them; true when memory holds the originals afterwards.
    bool restore(Memory& mem);

    const SnowStatus& status() const { return st_; }
    std::vector<std::string> takeLog();

private:
    bool readF(Memory& mem, uint64_t a, float& v) const;
    bool writeF(Memory& mem, uint64_t a, float v, std::string& why);
    void fail(Memory& mem, const std::string& why);
    void log(const std::string& s) { log_.push_back(s); }

    SnowConfig cfg_;
    SnowStatus st_;
    Field start_, full_;
    bool wrote_ = false;           // memory may differ from the originals
    double clock_ = 0, lastLog_ = -1e9;
    bool loggedReached_ = true;
    uint32_t streak_ = 0;
    std::vector<std::string> log_;
};

}  // namespace qs::apply
