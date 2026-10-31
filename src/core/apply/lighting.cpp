// QuattroStagioni - seasonal light and sky (see lighting.h).
#include "lighting.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace qs::apply {

namespace {

const char* const kChannelKey[kLightChannels] = {
    "SunIntensity", "SunColorR", "SunColorG", "SunColorB",
    "AmbientIntensity", "AmbientR", "AmbientG", "AmbientB",
    "FogDensity",
    "SkyR", "SkyG", "SkyB",
};

// Defaults, S U L A W (Spring, Summer, LateSummer, Autumn, Winter). Summer = the game's own light.
// Small differences: winter weaker and colder, autumn warm and golden, spring slightly cool, late
// summer warm and a little hazy.
const float kDefault[kLightChannels][season::kPhaseCount] = {
    {0.97f, 1.0f, 1.02f, 0.95f, 0.88f},  // SunIntensity
    {0.98f, 1.0f, 1.03f, 1.05f, 0.94f},  // SunColorR
    {1.00f, 1.0f, 1.00f, 0.98f, 0.98f},  // SunColorG
    {1.02f, 1.0f, 0.95f, 0.88f, 1.06f},  // SunColorB
    {0.98f, 1.0f, 1.00f, 0.96f, 0.92f},  // AmbientIntensity
    {0.98f, 1.0f, 1.02f, 1.04f, 0.95f},  // AmbientR
    {1.00f, 1.0f, 1.00f, 0.99f, 0.98f},  // AmbientG
    {1.02f, 1.0f, 0.97f, 0.92f, 1.05f},  // AmbientB
    {1.05f, 1.0f, 1.08f, 1.10f, 1.10f},  // FogDensity
    {0.98f, 1.0f, 1.02f, 1.03f, 0.94f},  // SkyR
    {1.00f, 1.0f, 1.00f, 0.99f, 0.97f},  // SkyG
    {1.02f, 1.0f, 0.96f, 0.94f, 1.03f},  // SkyB
};

const char* const kFieldKey[kLightFields] = {"SunColor", "SunIntensity", "AmbientColor", "AmbientIntensity", "FogDensity", "SkyColor"};
const int kFieldComponents[kLightFields] = {3, 1, 3, 1, 1, 3};
const int kFieldChannel[kLightFields] = {int(LightChannel::SunR), int(LightChannel::SunIntensity), int(LightChannel::AmbientR),
                                         int(LightChannel::AmbientIntensity), int(LightChannel::FogDensity), int(LightChannel::SkyR)};

bool parseF(const std::string& s, float& out) {
    if (s.empty()) return false;
    std::string t = s;
    std::replace(t.begin(), t.end(), ',', '.');  // decimal comma accepted, like the rest of the ini
    char* end = nullptr;
    const double d = std::strtod(t.c_str(), &end);
    while (end && (*end == ' ' || *end == '\t')) ++end;
    if (!end || end == t.c_str() || *end || !std::isfinite(d)) return false;
    out = float(d);
    return true;
}

std::string fmt(const char* f, double v) {
    char b[64];
    snprintf(b, sizeof b, f, v);
    return b;
}

}  // namespace

const char* lightChannelKey(int ch) { return ch >= 0 && ch < kLightChannels ? kChannelKey[ch] : "?"; }
const char* lightFieldKey(int f) { return f >= 0 && f < kLightFields ? kFieldKey[f] : "?"; }
int lightFieldComponents(int f) { return f >= 0 && f < kLightFields ? kFieldComponents[f] : 0; }
int lightFieldChannel(int f) { return f >= 0 && f < kLightFields ? kFieldChannel[f] : 0; }

LightingConfig::LightingConfig() {
    for (int c = 0; c < kLightChannels; ++c)
        for (int p = 0; p < season::kPhaseCount; ++p) ch[c].v[p] = kDefault[c][p];
}

void LightingConfig::load(const Ini& ini, std::vector<std::string>& warnings) {
    const char* S = "Lighting";
    auto num = [&](const char* key, float& out, float lo, float hi) {
        if (!ini.has(S, key)) return;
        float v = 0;
        if (!parseF(ini.get(S, key), v)) {
            warnings.push_back(std::string("[Lighting] ") + key + "=" + ini.get(S, key) + " not a number: default kept");
            return;
        }
        if (v < lo || v > hi) warnings.push_back(std::string("[Lighting] ") + key + ": value clamped to the valid range");
        out = std::min(hi, std::max(lo, v));
    };
    if (ini.has(S, "Enabled")) enabled = ini.getInt(S, "Enabled", 1) != 0;
    num("RampPerSec", rampPerSec, 0.001f, 10.0f);
    num("LogIntervalSec", logIntervalSec, 0.0f, 3600.0f);
    for (int c = 0; c < kLightChannels; ++c) {
        const bool fog = c == int(LightChannel::FogDensity);
        loadPhaseValues(ini, S, kChannelKey[c], ch[c], 0.5f, fog ? 2.0f : 1.5f, warnings);
    }
}

std::string lightingIniTemplate() {
    LightingConfig c;
    std::string s =
        "[Lighting]\n"
        "; Luce e cielo stagionali: moltiplicano i valori del gioco (sole, luce ambiente, nebbia, colore del\n"
        "; cielo). 1 = valore del gioco; l'estate resta quella originale. I valori seguono lo stadio visivo\n"
        "; della regione temperata, compresi i passaggi graduali tra le stagioni (come le foglie). Nel\n"
        "; deserto la luce resta sempre quella del gioco. Si spegne anche dal menu F5 (\"Luce stagionale\").\n"
        "; Serve che i campi della luce siano indicati in QuattroStagioni\\targets.ini, sezione [lighting]\n"
        "; (trovati con le probe): finché non lo sono questa sezione non ha effetto.\n"
        "; Tutte le chiavi sono facoltative: se mancano valgono questi default.\n"
        "Enabled=1\n"
        "; variazione massima dei moltiplicatori per secondo reale (0.01 = un cambio di 0.05 in 5 secondi)\n"
        "RampPerSec=" + fmt("%g", c.rampPerSec) + "\n"
        "; ogni quanti secondi reali scrivere nel log i moltiplicatori (0 = mai)\n"
        "LogIntervalSec=" + fmt("%g", c.logIntervalSec) + "\n"
        "; Cinque valori per chiave, nell'ordine: primavera, estate, fine estate, autunno, inverno.\n"
        "; Colori e intensità 0.5 .. 1.5, nebbia 0.5 .. 2. Restare vicini a 1 (0.85 .. 1.1): il gioco\n"
        "; ha già il suo ciclo del giorno, qui si sposta solo un poco il tono della stagione.\n"
        "; inverno sole più debole e freddo, autunno caldo e dorato, primavera appena fresca,\n"
        "; fine estate calda e un po' velata\n";
    const char* const notes[kLightChannels] = {
        "intensità del sole", "colore del sole: rosso", "colore del sole: verde", "colore del sole: blu",
        "intensità della luce ambiente", "luce ambiente: rosso", "luce ambiente: verde", "luce ambiente: blu",
        "densità della nebbia (sopra 1 più velato)", "colore del cielo: rosso", "colore del cielo: verde",
        "colore del cielo: blu"};
    for (int i = 0; i < kLightChannels; ++i) {
        s += "; ";
        s += notes[i];
        s += "\n";
        s += kChannelKey[i];
        s += "=" + formatPhaseValues(c.ch[i]) + "\n";
    }
    return s;
}

float applyScale(float gameValue, float mult, float bypass) {
    if (!std::isfinite(gameValue)) return gameValue;
    if (!std::isfinite(mult) || !std::isfinite(bypass)) return gameValue;
    bypass = std::min(1.0f, std::max(0.0f, bypass));
    const float m = mult + (1.0f - mult) * bypass;
    const float out = gameValue * m;
    return std::isfinite(out) ? out : gameValue;
}

LightingMultipliers LightingModel::target(const season::VisualStage& st) const {
    LightingMultipliers m;
    for (int c = 0; c < kLightChannels; ++c) m.v[c] = stageValue(cfg_.ch[c], st);
    return m;
}

void LightingModel::step(const season::VisualStage& st, bool on, float dtSec) {
    LightingMultipliers t;
    if (on) t = target(st);
    if (!std::isfinite(dtSec) || dtSec < 0) dtSec = 0;
    const float maxStep = cfg_.rampPerSec * dtSec;
    for (int c = 0; c < kLightChannels; ++c) cur_.v[c] = approach(cur_.v[c], t.v[c], maxStep);
}

int LightingTargets::count() const {
    int n = 0;
    for (const LightingTarget& f : field) n += f.ref.valid ? 1 : 0;
    return n;
}

void LightingTargets::load(const Ini& ini, std::vector<std::string>& warnings) {
    const char* S = "lighting";
    if (ini.has(S, "Enabled")) enabled = ini.getInt(S, "Enabled", 0) != 0;
    for (int f = 0; f < kLightFields; ++f) {
        LightingTarget& t = field[f];
        const std::string key = kFieldKey[f];
        if (ini.has(S, key)) {
            std::string err;
            LayerRef r;
            const int n = kFieldComponents[f];
            if (!parseLayerRef(ini.get(S, key), r, &err)) {
                warnings.push_back("[lighting] " + key + ": " + err + " (field dropped)");
            } else if (r.valid && n == 3 && r.count != 3 && r.count != 4) {
                warnings.push_back("[lighting] " + key + ": a colour needs 3 (or 4) floats, e.g. atmosphere:0x1A0:3 (field dropped)");
            } else if (r.valid && n == 1 && r.count != 1) {
                warnings.push_back("[lighting] " + key + ": one float expected, e.g. atmosphere:0x1B0 (field dropped)");
            } else {
                t.ref = r;
            }
        }
        auto lim = [&](const std::string& k, float& out) {
            if (!ini.has(S, k)) return;
            float v = 0;
            if (parseF(ini.get(S, k), v)) out = v;
            else warnings.push_back("[lighting] " + k + "=" + ini.get(S, k) + " not a number: default kept");
        };
        lim(key + "Min", t.minV);
        lim(key + "Max", t.maxV);
        if (t.minV > t.maxV) {
            warnings.push_back("[lighting] " + key + "Min is above " + key + "Max: limits swapped");
            std::swap(t.minV, t.maxV);
        }
    }
}

}  // namespace qs::apply
