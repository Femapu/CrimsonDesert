// QuattroStagioni - seasonal particles (see particles.h).
#include "particles.h"

#include <algorithm>
#include <cctype>
#include <cmath>

#include "targets.h"

namespace qs::apply {

namespace {
float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
float clamp01(float v) { return clampf(v, 0.0f, 1.0f); }
float fade(float bypass) { return std::isfinite(bypass) ? 1.0f - clamp01(bypass) : 0.0f; }

void loadNum(const Ini& ini, const char* S, const char* key, float& out, float lo, float hi,
             std::vector<std::string>& warnings) {
    if (!ini.has(S, key)) return;
    std::string t = ini.get(S, key);
    std::replace(t.begin(), t.end(), ',', '.');  // "0,4" accepted like in [Seasons]
    double d;
    if (!parseDouble(t, d) || !std::isfinite(d)) {
        warnings.push_back(std::string("[") + S + "] " + key + "=" + ini.get(S, key) + " not a number: default kept");
        return;
    }
    const float v = clampf(float(d), lo, hi);
    if (v != float(d)) warnings.push_back(std::string("[") + S + "] " + key + "=" + ini.get(S, key) + " clamped to the valid range");
    out = v;
}
}  // namespace

void ParticlesConfig::load(const Ini& ini, std::vector<std::string>& warnings) {
    const char* S = "Particles";
    if (ini.has(S, "Enabled")) enabled = ini.getInt(S, "Enabled", 1) != 0;
    if (ini.has(S, "WinterFlakes")) winterFlakes = ini.getInt(S, "WinterFlakes", 1) != 0;
    loadNum(ini, S, "WinterFlakeBoost", winterFlakeBoost, 1.0f, 3.0f, warnings);
    loadNum(ini, S, "WinterFlakeMin", winterFlakeMin, 0.0f, 1.0f, warnings);
    loadNum(ini, S, "SnowMinActive", snowMinActive, 0.001f, 1.0f, warnings);
    loadNum(ini, S, "RampPerSec", rampPerSec, 0.0f, 10.0f, warnings);
    if (ini.has(S, "LeavesPerPhase")) {  // a malformed value keeps the season engine's leaves
        PhaseValues ok;
        leavesFromSeason = !parsePhaseValues(ini.get(S, "LeavesPerPhase"), ok);
        loadPhaseValues(ini, S, "LeavesPerPhase", leavesPerPhase, 0.0f, 1.0f, warnings);
    }
    loadPhaseValues(ini, S, "PollenPerPhase", pollenPerPhase, 0.0f, 1.0f, warnings);
}

std::string particlesIniTemplate() {
    return "[Particles]\n"
           "; Particelle stagionali: fiocchi di neve più fitti d'inverno, foglie che cadono in autunno,\n"
           "; pollini e petali in primavera. Si spegne anche dal menu F5 (\"Particelle\").\n"
           "; Tutte le chiavi sono facoltative: se mancano valgono questi default.\n"
           "Enabled=1\n"
           "; 1 = d'inverno, quando nevica già, la neve cade più fitta (mai neve dal nulla, mai nel deserto)\n"
           "WinterFlakes=1\n"
           "; moltiplicatore della neve che sta già cadendo (1..3)\n"
           "WinterFlakeBoost=1.25\n"
           "; neve minima mentre nevica e c'è precipitazione (0..1)\n"
           "WinterFlakeMin=0.35\n"
           "; sotto questo valore di neve il gioco non sta nevicando: la neve resta com'è (0.001..1)\n"
           "SnowMinActive=0.05\n"
           "; intensità delle foglie per fase (Spring, Summer, LateSummer, Autumn, Winter; 0..1).\n"
           "; Se manca si usa il calendario delle stagioni (foglie della regione temperata).\n"
           "; LeavesPerPhase=0, 0.05, 0.35, 1, 0.1\n"
           "; intensità di pollini e petali per fase (0..1)\n"
           "PollenPerPhase=1, 0.25, 0, 0, 0\n"
           "; variazione massima delle intensità per secondo reale (0 = subito)\n"
           "RampPerSec=0.2\n"
           "; Foglie e pollini hanno bisogno di un campo del gioco non ancora trovato: si attivano in\n"
           "; QuattroStagioni\\targets.ini, sezione [particles] (vedi docs/dev/particelle.md).\n";
}

float flakes(float snow, float precip, float winterK, float bypass, const ParticlesConfig& cfg) {
    if (!std::isfinite(snow) || !cfg.enabled || !cfg.winterFlakes) return snow;
    if (snow <= cfg.snowMinActive || snow >= 1.0f) return snow;  // not snowing, or already full
    const float k = (std::isfinite(winterK) ? clamp01(winterK) : 0.0f) * fade(bypass);
    if (k <= 0.0f) return snow;
    float target = std::max(snow, std::min(1.0f, snow * cfg.winterFlakeBoost));
    if (std::isfinite(precip) && precip > 0.0f) target = std::max(target, clamp01(cfg.winterFlakeMin));
    const float out = snow + (target - snow) * k;
    return clampf(out, snow, 1.0f);
}

float leavesIntensity(const ParticlesConfig& cfg, const season::VisualStage& st, float regionMul) {
    if (!std::isfinite(regionMul)) regionMul = 1.0f;
    return clamp01(stageValue(cfg.leavesPerPhase, st) * std::max(0.0f, regionMul));
}

float pollenIntensity(const ParticlesConfig& cfg, const season::VisualStage& st) {
    return clamp01(stageValue(cfg.pollenPerPhase, st));
}

void ParticleFields::load(const Ini& ini, std::vector<std::string>& warnings) {
    const char* S = "particles";
    if (ini.has(S, "Enabled")) enabled = ini.getInt(S, "Enabled", 0) != 0;
    auto ref = [&](const char* key, LayerRef& out) {
        if (!ini.has(S, key)) return;
        std::string err;
        LayerRef r;
        if (parseLayerRef(ini.get(S, key), r, &err)) out = r;
        else warnings.push_back(std::string("[particles] ") + key + "=" + ini.get(S, key) + ": " + err + " (field off)");
    };
    ref("Leaves", leaves);
    ref("Pollen", pollen);
    if (ini.has(S, "Mode")) {
        std::string m = ini.get(S, "Mode");
        for (char& c : m) c = char(std::tolower((unsigned char)c));
        if (m == "scale") mode = ParticleMode::Scale;
        else if (m == "write") mode = ParticleMode::Write;
        else warnings.push_back("[particles] Mode=" + ini.get(S, "Mode") + " must be scale or write (scale kept)");
    }
    loadNum(ini, S, "Value", value, 0.0f, 1000.0f, warnings);
    loadNum(ini, S, "Max", maxValue, 0.01f, 1000.0f, warnings);
}

float particleField(float game, float intensity, float bypass, const ParticleFields& f) {
    if (!std::isfinite(game)) return game;
    const float i = std::isfinite(intensity) ? clamp01(intensity) : 0.0f;
    const float want = f.mode == ParticleMode::Scale ? game * (1.0f + (f.value - 1.0f) * i) : f.value * i;
    const float out = game + (want - game) * fade(bypass);
    return clampf(out, 0.0f, std::max(game, f.maxValue));
}

}  // namespace qs::apply
