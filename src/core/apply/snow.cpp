// QuattroStagioni - winter snow thresholds (see snow.h).
#include "snow.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "targets.h"

namespace qs::apply {

namespace {

float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
float smooth(float x) {
    x = clampf(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}
std::string fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));
std::string fmt(const char* f, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, f);
    vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}
constexpr float kEps = 1e-4f;

}  // namespace

float SnowConfig::effectiveStart() const {
    float cap = clampf(std::isfinite(maxStart) ? maxStart : 4.0f, kSnowMinC + 1.0f, kSnowHardMaxStart);
    float s = std::isfinite(winterStart) ? winterStart : 2.0f;
    return clampf(s, kSnowMinC + 1.0f, cap);
}

float SnowConfig::effectiveFull() const {
    float f = std::isfinite(winterFull) ? winterFull : -6.0f;
    return clampf(f, kSnowMinC, effectiveStart() - 1.0f);
}

void SnowConfig::load(const Ini& ini, std::vector<std::string>& warnings) {
    auto num = [&](const char* key, float& out) {
        if (!ini.has("Snow", key)) return;
        double d;
        if (parseDouble(ini.get("Snow", key), d) && std::isfinite(d)) out = float(d);
        else warnings.push_back(std::string("[Snow] ") + key + "=" + ini.get("Snow", key) + " not a number: default kept");
    };
    if (ini.has("Snow", "Enabled")) enabled = ini.getInt("Snow", "Enabled", 1) != 0;
    num("WinterSnowStart", winterStart);
    num("WinterSnowFull", winterFull);
    num("MaxSnowStart", maxStart);
    num("MaxRate", maxRate);
    if (maxRate < 0) maxRate = 0;
    if (ini.has("Snow", "StartDelayMs")) startDelayMs = uint32_t(std::clamp<long long>(ini.getInt("Snow", "StartDelayMs", 20000), 0, 600000));
    if (maxStart > kSnowHardMaxStart)
        warnings.push_back(fmt("[Snow] MaxSnowStart=%.1f above the hard limit: %.1f used", maxStart, kSnowHardMaxStart));
    if (winterStart > effectiveStart() + kEps)
        warnings.push_back(fmt("[Snow] WinterSnowStart=%.1f capped to %.1f (MaxSnowStart)", winterStart, effectiveStart()));
    if (winterFull > effectiveFull() + kEps)
        warnings.push_back(fmt("[Snow] WinterSnowFull=%.1f lowered to %.1f (must stay below SnowStart)", winterFull, effectiveFull()));
}

std::string snowIniTemplate() {
    return "[Snow]\n"
           "; Neve d'inverno: soglie di temperatura del gioco (SnowStart, SnowFull).\n"
           "; Il gioco mostra neve dove la temperatura del clima è <= SnowStart, copertura piena a <= SnowFull\n"
           "; (vanilla -5 e -20: in pratica mai). In inverno la mod alza le soglie gradualmente, fuori\n"
           "; dall'inverno tornano esattamente ai valori originali. Il deserto resta senza neve perché la sua\n"
           "; temperatura è molto più alta di qualunque soglia ammessa (MaxSnowStart).\n"
           "; Si spegne anche dal menu F5 (Meteo e luce, \"Neve che si accumula\").\n"
           "Enabled=1\n"
           "WinterSnowStart=2.0\n"
           "WinterSnowFull=-6.0\n"
           "; limite di sicurezza per WinterSnowStart (massimo assoluto 10)\n"
           "MaxSnowStart=4.0\n"
           "; variazione massima in gradi al secondo reale\n"
           "MaxRate=0.5\n"
           "StartDelayMs=20000\n";
}

float winterIntensity(season::Phase phase, float p, float tf) {
    tf = clampf(std::isfinite(tf) ? tf : 0.4f, 0.05f, 1.0f);
    p = clampf(std::isfinite(p) ? p : 0.0f, 0.0f, 1.0f);
    const float half = tf * 0.5f;
    if (phase == season::Phase::Autumn) return smooth((p - (1.0f - half)) / half);  // late A>W
    if (phase == season::Phase::Winter) return 1.0f - smooth((p - (1.0f - tf)) / half);  // early W>S
    return 0.0f;
}

float winterIntensity(const season::SeasonState& s, const season::SeasonConfig& sc, season::Region r) {
    const int i = int(r);
    if (i < 0 || i >= season::kRegionCount || !s.regionActive[i]) return 0.0f;
    return winterIntensity(s.regionPhase[i], s.regionPhaseProgress[i], sc.transitionFraction);
}

SnowThresholds snowTargets(const SnowThresholds& o, const SnowConfig& cfg, float k) {
    k = clampf(std::isfinite(k) ? k : 0.0f, 0.0f, 1.0f);
    if (k <= 0.0f) return o;
    SnowThresholds t;
    t.start = clampf(o.start + k * (cfg.effectiveStart() - o.start), kSnowMinC, kSnowMaxC);
    t.full = clampf(o.full + k * (cfg.effectiveFull() - o.full), kSnowMinC, kSnowMaxC);
    return t;
}

const char* toString(SnowState s) {
    switch (s) {
        case SnowState::Waiting: return "waiting";
        case SnowState::Idle: return "idle";
        case SnowState::Active: return "active";
        case SnowState::Disabled: return "disabled";
    }
    return "?";
}

bool SnowController::readF(Memory& mem, uint64_t a, float& v) const {
    return mem.read(a, &v, sizeof v) && std::isfinite(v);
}

bool SnowController::writeF(Memory& mem, uint64_t a, float v, std::string& why) {
    if (!mem.write(a, &v, sizeof v, &why)) return false;
    float back;
    if (!readF(mem, a, back) || std::memcmp(&back, &v, sizeof v) != 0) {
        why = "readback mismatch";
        return false;
    }
    ++st_.writes;
    return true;
}

void SnowController::disable(const std::string& why) {
    st_.state = SnowState::Disabled;
    st_.reason = why;
    log("snow: DISABLED: " + why);
}

void SnowController::fail(Memory& mem, const std::string& why) {
    restore(mem);
    st_.attached = false;
    disable(why);
}

bool SnowController::attach(const Field& s, const Field& f, Memory& mem) {
    if (st_.state == SnowState::Disabled) return false;
    float v[2], d[2];
    const Field* fl[2] = {&s, &f};
    const char* nm[2] = {"SnowStart", "SnowFull"};
    for (int i = 0; i < 2; ++i) {
        if (!fl[i]->value || !readF(mem, fl[i]->value, v[i])) {
            disable(fmt("%s: value not readable", nm[i]));
            return false;
        }
        if (v[i] < kSnowMinC || v[i] > kSnowMaxC) {
            disable(fmt("%s: implausible value %.2f (expected %.0f..%.0f)", nm[i], v[i], kSnowMinC, kSnowMaxC));
            return false;
        }
        if (fl[i]->def) {
            if (!readF(mem, fl[i]->def, d[i])) {
                disable(fmt("%s: default not readable", nm[i]));
                return false;
            }
            if (std::fabs(d[i] - v[i]) > kEps) {
                disable(fmt("%s: value %.2f differs from the default %.2f (changed by someone else?)", nm[i], v[i], d[i]));
                return false;
            }
        }
    }
    if (!(v[1] < v[0])) {
        disable(fmt("SnowFull %.2f not below SnowStart %.2f", v[1], v[0]));
        return false;
    }
    start_ = s;
    full_ = f;
    st_.original = {v[0], v[1]};
    st_.current = st_.target = st_.original;
    st_.attached = true;
    st_.state = SnowState::Idle;
    st_.reason.clear();
    wrote_ = false;
    streak_ = 0;
    log(fmt("snow: attached, originals SnowStart %.2f SnowFull %.2f; winter values %.2f / %.2f (MaxSnowStart %.1f)",
            v[0], v[1], cfg_.effectiveStart(), cfg_.effectiveFull(), cfg_.maxStart));
    if (std::fabs(v[0] - kSnowStartVanilla) > kEps || std::fabs(v[1] - kSnowFullVanilla) > kEps)
        log("snow: note: originals differ from the known vanilla values -5 / -20 (game update?)");
    return true;
}

bool SnowController::restore(Memory& mem) {
    if (!st_.attached) return true;
    if (!wrote_) return true;
    std::string why;
    bool ok = writeF(mem, start_.value, st_.original.start, why);
    ok = writeF(mem, full_.value, st_.original.full, why) && ok;
    if (ok) {
        wrote_ = false;
        st_.current = st_.original;
        log(fmt("snow: originals restored (SnowStart %.2f, SnowFull %.2f)", st_.original.start, st_.original.full));
    } else {
        log("snow: restore FAILED: " + why);
    }
    return ok;
}

void SnowController::tick(bool on, float intensity, double dt, Memory& mem, const std::string& label) {
    if (!std::isfinite(dt) || dt < 0) dt = 0;
    clock_ += dt;
    st_.intensity = on ? clampf(std::isfinite(intensity) ? intensity : 0.0f, 0.0f, 1.0f) : 0.0f;
    if (!st_.attached || st_.state == SnowState::Disabled) return;

    // 1. Is memory still what we left there? (a save load, a console command, or a wrong field)
    float cur[2];
    if (!readF(mem, start_.value, cur[0]) || !readF(mem, full_.value, cur[1])) {
        fail(mem, "values not readable any more");
        return;
    }
    const float mine[2] = {st_.current.start, st_.current.full};
    const float orig[2] = {st_.original.start, st_.original.full};
    bool foreign = false;
    for (int i = 0; i < 2; ++i)
        if (std::fabs(cur[i] - mine[i]) > kEps) {
            if (std::fabs(cur[i] - orig[i]) <= kEps) continue;  // the game reset it to vanilla: we just write again
            foreign = true;
        }
    if (foreign) {
        ++st_.anomalies;
        if (++streak_ >= 3) {
            fail(mem, fmt("values changed by the game or another mod (SnowStart %.2f, SnowFull %.2f)", cur[0], cur[1]));
            return;
        }
        return;  // wait: write nothing this tick
    }
    streak_ = 0;
    st_.current = {cur[0], cur[1]};

    // 2. Off: originals at once.
    if (!on) {
        st_.target = st_.original;
        if (wrote_ || std::fabs(cur[0] - orig[0]) > kEps || std::fabs(cur[1] - orig[1]) > kEps) {
            wrote_ = true;  // make restore() write even after a game reset of one field
            if (!restore(mem)) fail(mem, "restore failed");
        }
        st_.state = SnowState::Idle;
        loggedReached_ = true;
        return;
    }

    // 3. Move toward the seasonal target, rate limited.
    st_.target = snowTargets(st_.original, cfg_, st_.intensity);
    const float tgt[2] = {st_.target.start, st_.target.full};
    float next[2];
    const float step = cfg_.maxRate > 0 ? float(cfg_.maxRate * dt) : 1e9f;
    for (int i = 0; i < 2; ++i) {
        float d = tgt[i] - cur[i];
        next[i] = std::fabs(d) <= step ? tgt[i] : cur[i] + (d > 0 ? step : -step);
    }
    // keep SnowFull below SnowStart while both move
    if (next[1] > next[0] - 0.5f) next[1] = std::min(next[1], next[0] - 0.5f);
    const bool changed = std::fabs(next[0] - cur[0]) > kEps || std::fabs(next[1] - cur[1]) > kEps;
    if (changed) {
        std::string why;
        wrote_ = true;
        if (!writeF(mem, start_.value, next[0], why) || !writeF(mem, full_.value, next[1], why)) {
            fail(mem, "write refused: " + why);
            return;
        }
        st_.current = {next[0], next[1]};
    }
    const bool atOriginal = std::fabs(st_.current.start - orig[0]) <= kEps && std::fabs(st_.current.full - orig[1]) <= kEps;
    if (atOriginal) wrote_ = false;
    st_.state = atOriginal ? SnowState::Idle : SnowState::Active;

    // 4. Throttled log: at most every 60 s while moving, and once when the target is reached.
    const bool reached = std::fabs(st_.current.start - tgt[0]) <= kEps && std::fabs(st_.current.full - tgt[1]) <= kEps;
    if (changed && (clock_ - lastLog_ >= 60.0 || (reached && !loggedReached_))) {
        log(fmt("snow: SnowStart %.2f -> %.2f, SnowFull %.2f -> %.2f (target %.2f / %.2f, original %.2f / %.2f), %s, winter intensity %.2f",
                cur[0], st_.current.start, cur[1], st_.current.full, tgt[0], tgt[1], orig[0], orig[1],
                label.empty() ? "?" : label.c_str(), st_.intensity));
        lastLog_ = clock_;
        loggedReached_ = reached;
    } else if (changed && !reached) {
        loggedReached_ = false;
    }
}

std::vector<std::string> SnowController::takeLog() {
    std::vector<std::string> v;
    v.swap(log_);
    return v;
}

}  // namespace qs::apply
