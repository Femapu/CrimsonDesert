// QuattroStagioni - lightning in storms, by season (see lightning.h).
#include "lightning.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

#include "targets.h"

namespace qs::apply {

namespace {

const char* const kSec = "Lightning";

std::string lowerTrim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    s = s.substr(i);
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string num(float v) {
    char b[32];
    snprintf(b, sizeof b, "%g", double(v));
    return b;
}

}  // namespace

LightningConfig::LightningConfig() {
    // Stefano's rules: many in summer and autumn, hardly any in winter (snow storms).
    const float chance[season::kPhaseCount] = {0.6f, 0.9f, 0.85f, 0.85f, 0.03f};
    const float rate[season::kPhaseCount] = {8, 14, 12, 12, 1};
    for (int p = 0; p < season::kPhaseCount; ++p) {
        chancePerStorm.v[p] = chance[p];
        strikesPerHour.v[p] = rate[p];
    }
}

void LightningConfig::load(const Ini& ini, std::vector<std::string>& warnings) {
    auto number = [&](const char* key, float& out, float lo, float hi) {
        if (!ini.has(kSec, key)) return;
        double d;
        if (parseDouble(ini.get(kSec, key), d) && std::isfinite(d)) out = std::clamp(float(d), lo, hi);
        else warnings.push_back(std::string("[Lightning] ") + key + "=" + ini.get(kSec, key) + " not a number: default kept");
    };
    if (ini.has(kSec, "Enabled")) enabled = ini.getInt(kSec, "Enabled", 1) != 0;
    if (ini.has(kSec, "LogStrikes")) logStrikes = ini.getInt(kSec, "LogStrikes", 0) != 0;
    loadPhaseValues(ini, kSec, "ChancePerStorm", chancePerStorm, 0.0f, 1.0f, warnings);
    loadPhaseValues(ini, kSec, "StrikesPerHour", strikesPerHour, 0.0f, 600.0f, warnings);
    number("MinGapSec", minGapSec, 0.0f, 600.0f);
    number("PulseMs", pulseMs, 16.0f, 5000.0f);
    number("RampSec", rampSec, 0.0f, 600.0f);
}

std::string lightningIniTemplate() {
    LightningConfig c;
    std::string s =
        "[Lightning]\n"
        "; Fulmini nei temporali, secondo la stagione. A ogni nuovo temporale (della mod o del gioco) la mod\n"
        "; decide se avrà fulmini (ChancePerStorm); durante il temporale i lampi arrivano a caso (in media\n"
        "; StrikesPerHour per ora di gioco, per l'intensità del temporale). Mai nel deserto. Il tuono resta\n"
        "; quello del gioco. Si spegne anche dal menu F5 (\"Fulmini\", scheda Meteo e luce).\n"
        "; Il campo del gioco da scrivere si imposta in QuattroStagioni\\targets.ini, sezione [lightning]:\n"
        "; finché non è impostato la mod scrive solo le righe \"lightning: strike\" nel log.\n"
        "; Tutte le chiavi sono facoltative: se mancano valgono questi default.\n";
    s += "Enabled=" + std::string(c.enabled ? "1" : "0") + "\n";
    s += "; quota dei temporali con fulmini per fase: primavera, estate, fine estate, autunno, inverno (0..1)\n";
    s += "ChancePerStorm=" + formatPhaseValues(c.chancePerStorm) + "\n";
    s += "; lampi per ora di gioco a temporale pieno, per fase (stesso ordine)\n";
    s += "StrikesPerHour=" + formatPhaseValues(c.strikesPerHour) + "\n";
    s += "; secondi reali minimi tra due lampi\n";
    s += "MinGapSec=" + num(c.minGapSec) + "\n";
    s += "; durata del lampo in millisecondi reali\n";
    s += "PulseMs=" + num(c.pulseMs) + "\n";
    s += "; secondi reali per portare il livello dei fulmini da 0 a 1 (inizio e fine del temporale)\n";
    s += "RampSec=" + num(c.rampSec) + "\n";
    s += "; 1 = una riga nel log per ogni lampo (sempre attiva finché il campo non è impostato)\n";
    s += "LogStrikes=" + std::string(c.logStrikes ? "1" : "0") + "\n";
    return s;
}

const char* toString(LightningMode m) { return m == LightningMode::Pulse ? "pulse" : "level"; }

void LightningTarget::load(const Ini& ini, std::vector<std::string>& warnings) {
    const char* S = "lightning";
    if (ini.has(S, "Enabled")) enabled = ini.getInt(S, "Enabled", 0) != 0;
    if (ini.has(S, "Field")) {
        std::string err;
        LayerRef r;
        if (parseLayerRef(ini.get(S, "Field"), r, &err)) field = r;
        else {
            warnings.push_back("[lightning] Field=" + ini.get(S, "Field") + " malformed (" + err + "): target off");
            field = LayerRef();
            enabled = false;
        }
    }
    if (ini.has(S, "Mode")) {
        const std::string m = lowerTrim(ini.get(S, "Mode"));
        if (m == "level") mode = LightningMode::Level;
        else if (m == "pulse") mode = LightningMode::Pulse;
        else warnings.push_back("[lightning] Mode=" + ini.get(S, "Mode") + " is not level or pulse: level kept");
    }
    if (ini.has(S, "Value")) {
        double d;
        if (parseDouble(ini.get(S, "Value"), d) && std::isfinite(d) && std::fabs(d) < 1e9) value = float(d);
        else {
            warnings.push_back("[lightning] Value=" + ini.get(S, "Value") + " not a number: target off");
            enabled = false;
        }
    }
}

float LightningDirector::seasonal(const PhaseValues& p, const LightningInput& in) {
    return phaseValue(p, in.phase, in.progress, in.transitionFraction);
}

float LightningDirector::exposure() {
    const float u = rng_.uniform();          // [0, 1)
    return -std::log(1.0f - u);              // Exp(1)
}

int LightningDirector::advance(double realSec, double gameHours, const LightningInput& in) {
    if (!(realSec >= 0) || !std::isfinite(realSec)) realSec = 0;
    if (!(gameHours >= 0) || !std::isfinite(gameHours)) gameHours = 0;
    sinceStrike_ += realSec;
    pulseLeft_ = std::max(0.0f, pulseLeft_ - float(realSec));
    chance_ = std::clamp(seasonal(cfg_.chancePerStorm, in), 0.0f, 1.0f);

    // storm start / end (the signal may flicker: it ends after kStormGraceSec without it)
    if (in.storm) {
        offFor_ = 0;
        if (!inStorm_) {
            inStorm_ = true;
            ++storms_;
            hasLightning_ = rng_.uniform() < chance_;
            need_ = exposure();
            char b[160];
            snprintf(b, sizeof b, "storm begins (%s, chance %.2f): %s", season::toString(in.phase), double(chance_),
                     hasLightning_ ? "with lightning" : "without lightning");
            log_.push_back(b);
        }
    } else if (inStorm_) {
        offFor_ += realSec;
        if (offFor_ >= kStormGraceSec) {
            inStorm_ = false;
            log_.push_back("storm ends");
        }
    }

    const bool live = cfg_.enabled && in.allowed && in.storm && inStorm_ && hasLightning_;
    const float intensity = std::clamp(std::isfinite(in.intensity) ? in.intensity : 0.0f, 0.0f, 1.0f);
    const float phaseRate = std::max(0.0f, seasonal(cfg_.strikesPerHour, in));
    rate_ = live ? phaseRate * intensity : 0.0f;

    // level: intensity scaled by the season's share of the busiest phase
    float top = 0;
    for (float v : cfg_.strikesPerHour.v) top = std::max(top, v);
    const float target = live && top > 0 ? intensity * std::min(1.0f, phaseRate / top) : 0.0f;
    level_ = cfg_.rampSec > 0 ? approach(level_, target, float(realSec) / cfg_.rampSec) : target;
    if (!in.allowed || !cfg_.enabled) pulseLeft_ = 0;

    if (!live) return 0;
    need_ -= double(rate_) * gameHours;
    if (need_ > 0 || sinceStrike_ < cfg_.minGapSec) return 0;
    // strike (one per step; a postponed one fires as soon as the gap allows)
    ++strikes_;
    sinceStrike_ = 0;
    pulseLeft_ = cfg_.pulseMs / 1000.0f;
    need_ = exposure();
    return 1;
}

std::vector<std::string> LightningDirector::takeLog() {
    std::vector<std::string> out;
    out.swap(log_);
    return out;
}

}  // namespace qs::apply
