// QuattroStagioni - shared helpers of the "after compose" features (see layer_field.h).
#include "layer_field.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace qs::apply {

const char* toString(Layer l) {
    switch (l) {
        case Layer::Weather: return "weather";
        case Layer::Atmosphere: return "atmosphere";
        case Layer::Manager: return "manager";
    }
    return "?";
}

namespace {
std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}
std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower((unsigned char)c));
    return s;
}
bool setErr(std::string* err, const std::string& m) {
    if (err) *err = m;
    return false;
}
}  // namespace

std::string LayerRef::text() const {
    if (!valid) return "";
    char b[64];
    if (count == 1) snprintf(b, sizeof b, "%s:0x%X", toString(layer), offset);
    else snprintf(b, sizeof b, "%s:0x%X:%d", toString(layer), offset, count);
    return b;
}

bool parseLayerRef(const std::string& s0, LayerRef& out, std::string* err) {
    std::string s = trim(s0);
    LayerRef r;
    if (s.empty()) {
        out = r;
        return true;
    }
    std::vector<std::string> parts;
    size_t st = 0;
    for (size_t i = 0; i <= s.size(); ++i)
        if (i == s.size() || s[i] == ':') {
            parts.push_back(trim(s.substr(st, i - st)));
            st = i + 1;
        }
    if (parts.size() < 2 || parts.size() > 3) return setErr(err, "expected <layer>:<offset>[:<count>]");
    std::string l = lower(parts[0]);
    if (l == "weather") r.layer = Layer::Weather;
    else if (l == "atmosphere" || l == "atm") r.layer = Layer::Atmosphere;
    else if (l == "manager" || l == "mgr") r.layer = Layer::Manager;
    else return setErr(err, "layer must be weather, atmosphere or manager");
    uint64_t off = 0;
    if (!parseU64(parts[1], off)) return setErr(err, "bad offset '" + parts[1] + "'");
    const uint64_t lim = r.layer == Layer::Manager ? 0x200000 : 0x10000;
    if (off >= lim || off % 4) return setErr(err, "offset must be 4-aligned and below the layer size");
    r.offset = uint32_t(off);
    if (parts.size() == 3) {
        uint64_t c = 0;
        if (!parseU64(parts[2], c) || c < 1 || c > 4) return setErr(err, "count must be 1..4");
        r.count = int(c);
    }
    r.valid = true;
    out = r;
    return true;
}

bool parsePhaseValues(const std::string& s, PhaseValues& out, std::string* err) {
    PhaseValues p;
    int n = 0;
    const char* c = s.c_str();
    while (*c) {
        while (*c && (std::isspace((unsigned char)*c) || *c == ',' || *c == ';')) ++c;
        if (!*c) break;
        char* end = nullptr;
        float v = std::strtof(c, &end);
        if (end == c) return setErr(err, "not a number near '" + std::string(c) + "'");
        if (!std::isfinite(v)) return setErr(err, "non-finite value");
        if (n >= season::kPhaseCount) return setErr(err, "more than 5 values (S U L A W)");
        p.v[n++] = v;
        c = end;
    }
    if (n != season::kPhaseCount) return setErr(err, "expected 5 values (S U L A W)");
    out = p;
    return true;
}

std::string formatPhaseValues(const PhaseValues& p) {
    std::string s;
    char b[32];
    for (int i = 0; i < season::kPhaseCount; ++i) {
        snprintf(b, sizeof b, "%s%g", i ? ", " : "", double(p.v[i]));
        s += b;
    }
    return s;
}

void loadPhaseValues(const Ini& ini, const std::string& section, const std::string& key, PhaseValues& out, float lo,
                     float hi, std::vector<std::string>& warnings) {
    if (!ini.has(section, key)) return;
    std::string err;
    PhaseValues p = out;
    if (!parsePhaseValues(ini.get(section, key), p, &err)) {
        warnings.push_back(section + " " + key + ": " + err + " (default kept)");
        return;
    }
    for (float& v : p.v)
        if (v < lo || v > hi) {
            warnings.push_back(section + " " + key + ": value clamped to the valid range");
            v = std::min(hi, std::max(lo, v));
        }
    out = p;
}

float stageValue(const PhaseValues& p, const season::VisualStage& st) {
    const float a = p[st.from], b = p[st.to];
    const float t = std::min(1.0f, std::max(0.0f, st.t));
    return st.step == 0 ? a : a + (b - a) * t;
}

float phaseValue(const PhaseValues& p, season::Phase phase, float progress, float tf) {
    const float a = p[phase], b = p[season::nextPhase(phase)];
    tf = std::min(1.0f, std::max(0.0f, tf));
    progress = std::min(1.0f, std::max(0.0f, progress));
    if (tf <= 0.0f || progress <= 1.0f - tf) return a;
    const float t = (progress - (1.0f - tf)) / tf;
    return a + (b - a) * t;
}

float approach(float cur, float target, float maxStep) {
    if (!std::isfinite(target)) return cur;
    if (!std::isfinite(cur)) return target;
    maxStep = std::fabs(maxStep);
    if (target > cur) return std::min(target, cur + maxStep);
    return std::max(target, cur - maxStep);
}

uint32_t Rng::next() {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}
float Rng::uniform() { return float(next() >> 8) * (1.0f / 16777216.0f); }

}  // namespace qs::apply
