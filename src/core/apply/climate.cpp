// QuattroStagioni - seasonal temperatures (see climate.h).
//
// Ported and adapted from Seasons of Pywel (Khione95) climate.cpp with the author's permission: the
// map layout, the night formula, the night floor and the 3x3 rule for the game's own cold places are
// his findings. Differences: the offsets follow our calendar (smooth over the transition tail), the
// desert comes from the game's own map (no shipped texture), only the top byte changes.
#include "climate.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

#include "targets.h"

namespace qs::apply {
namespace {

const char* const kOffsetKey[season::kPhaseCount] = {"SpringOffsetC", "SummerOffsetC", "LateSummerOffsetC",
                                                     "AutumnOffsetC", "WinterOffsetC"};

float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
float smooth(float x) {
    x = clampf(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}
float topC(uint32_t t) { return (float(t >> 24) / 255.0f * 2.0f - 1.0f) * 50.0f; }
std::string fmt(float v) {
    char b[32];
    std::snprintf(b, sizeof b, "%g", double(v));
    return b;
}

}  // namespace

void ClimateConfig::load(const Ini& ini, std::vector<std::string>& warnings) {
    const char* S = "Climate";
    auto num = [&](const char* key, float& out, float lo, float hi) {
        if (!ini.has(S, key)) return;
        double d;
        if (parseDouble(ini.get(S, key), d) && std::isfinite(d)) out = clampf(float(d), lo, hi);
        else warnings.push_back(std::string("[Climate] ") + key + "=" + ini.get(S, key) + " not a number: default kept");
    };
    auto integer = [&](const char* key, int& out, int lo, int hi) {
        if (ini.has(S, key)) out = int(std::clamp<long long>(ini.getInt(S, key, out), lo, hi));
    };
    if (ini.has(S, "Enabled")) enabled = ini.getInt(S, "Enabled", 1) != 0;
    for (int p = 0; p < season::kPhaseCount; ++p) num(kOffsetKey[p], offsetC[p], -40.0f, 15.0f);
    num("NightFloorC", nightFloorC, -45.0f, 0.0f);
    num("DesertTopTempC", desertTopC, 0.0f, 60.0f);
    num("MaxRate", maxRatePerSec, 0.01f, 50.0f);
    num("LogIntervalSec", logIntervalSec, 0.0f, 3600.0f);
    integer("DesertRadius", desertRadius, 0, 12);
    integer("DesertFade", desertFade, 0, 16);
    if (ini.has(S, "StartDelayMs")) startDelayMs = uint32_t(std::clamp<long long>(ini.getInt(S, "StartDelayMs", 25000), 0, 600000));
}

std::string climateIniTemplate() {
    ClimateConfig c;
    std::ostringstream o;
    o << "[Climate]\n"
      << "; Temperature stagionali: la mod sposta la temperatura massima del giorno nella mappa del clima\n"
      << "; del gioco (in memoria, dal vivo; i file del gioco non cambiano). Ogni chiave è facoltativa.\n"
      << "Enabled=1\n"
      << "; Gradi rispetto al gioco per fase (estate del gioco = riferimento). Nelle code di transizione\n"
      << "; il valore sfuma verso la fase successiva.\n";
    for (int p = 0; p < season::kPhaseCount; ++p) o << kOffsetKey[p] << "=" << fmt(c.offsetC[p]) << "\n";
    o << "; Nessuna stagione rende una notte più fredda di così dove le notti del gioco sono più miti\n"
      << "; (il freddo sotto circa -40 ferisce). I luoghi freddi del gioco (montagne innevate) restano come sono.\n"
      << "NightFloorC=" << fmt(c.nightFloorC) << "\n"
      << "; Deserto: texel con massima >= DesertTopTempC; entro DesertRadius texel (80 m) restano come nel\n"
      << "; gioco in ogni stagione, poi la differenza stagionale entra in DesertFade texel.\n"
      << "DesertTopTempC=" << fmt(c.desertTopC) << "\n"
      << "DesertRadius=" << c.desertRadius << "\n"
      << "DesertFade=" << c.desertFade << "\n"
      << "; Gradi al secondo reale con cui la differenza in vigore insegue quella della stagione.\n"
      << "MaxRate=" << fmt(c.maxRatePerSec) << "\n"
      << "StartDelayMs=" << c.startDelayMs << "\n"
      << "LogIntervalSec=" << fmt(c.logIntervalSec) << "\n";
    return o.str();
}

float climateRangeC(uint32_t t) { return (1.0f - float((t >> 16) & 0xFF) / 255.0f) * 18.0f + 7.0f; }

uint32_t climateTopByte(float c) {
    if (!std::isfinite(c)) return 0;
    const float b = std::floor((c / 50.0f + 1.0f) / 2.0f * 255.0f + 0.5f);
    return uint32_t(clampf(b, 0.0f, 255.0f));
}

uint32_t climateTopByteUp(float c) {
    if (!std::isfinite(c)) return 255;
    const float b = std::ceil((c / 50.0f + 1.0f) / 2.0f * 255.0f - 0.001f);
    return uint32_t(clampf(b, 0.0f, 255.0f));
}

float climateOffsetC(const ClimateConfig& c, season::Phase phase, float progress, float tf) {
    const int p = std::clamp(int(phase), 0, season::kPhaseCount - 1);
    const float here = c.offsetC[p], next = c.offsetC[int(season::nextPhase(season::Phase(p)))];
    tf = clampf(std::isfinite(tf) ? tf : 0.4f, 0.0f, 1.0f);
    progress = clampf(std::isfinite(progress) ? progress : 0.0f, 0.0f, 1.0f);
    if (tf <= 0.0f) return here;
    const float k = smooth((progress - (1.0f - tf)) / tf);
    return here + k * (next - here);
}

float climateRamp(float current, float target, float maxRate, float dt) {
    if (!std::isfinite(target)) return current;
    if (!std::isfinite(current)) return target;
    const float step = std::max(0.0f, maxRate) * std::max(0.0f, std::isfinite(dt) ? dt : 0.0f);
    if (std::fabs(target - current) <= step) return target;
    return current + (target > current ? step : -step);
}

TexelKind ClimatePlan::kind(size_t i) const {
    if (i >= weight.size()) return TexelKind::Normal;
    if (cold[i]) return TexelKind::GameCold;
    if (weight[i] <= 0.0f) return TexelKind::Desert;
    if (weight[i] < 1.0f) return TexelKind::DesertFade;
    return TexelKind::Normal;
}

bool buildClimatePlan(const std::vector<uint32_t>& orig, int size, const ClimateConfig& c, ClimatePlan& plan,
                      std::string* why) {
    auto fail = [&](const std::string& w) {
        if (why) *why = w;
        return false;
    };
    if (size < 8 || orig.size() != size_t(size) * size_t(size)) return fail("map size does not match");
    const size_t n = orig.size();
    plan = ClimatePlan();
    plan.size = size;
    plan.weight.assign(n, 1.0f);
    plan.cold.assign(n, 0);
    plan.least.assign(n, 0);

    // Plausibility: the game's map has hot desert texels and mostly mild land.
    size_t hot = 0, mild = 0;
    for (uint32_t t : orig) {
        const float v = topC(t);
        plan.hottestC = std::max(plan.hottestC, v);
        plan.coldestC = std::min(plan.coldestC, v);
        hot += v >= c.desertTopC;
        mild += v > -10.0f && v < c.desertTopC;
    }
    if (hot == 0) return fail("no texel at or above the desert line: not the game's temperature map?");
    if (mild < n / 10) return fail("too few mild texels: not the game's temperature map?");

    // Chebyshev distance to the nearest desert texel (two passes, 8-connected, capped).
    const int cap = c.desertRadius + c.desertFade + 2;
    std::vector<int> d(n, cap);
    for (size_t i = 0; i < n; ++i)
        if (topC(orig[i]) >= c.desertTopC) d[i] = 0;
    auto at = [&](int r, int col) -> int& { return d[size_t(r) * size_t(size) + size_t(col)]; };
    for (int r = 0; r < size; ++r)
        for (int col = 0; col < size; ++col) {
            int& v = at(r, col);
            if (col > 0) v = std::min(v, at(r, col - 1) + 1);
            if (r > 0) {
                v = std::min(v, at(r - 1, col) + 1);
                if (col > 0) v = std::min(v, at(r - 1, col - 1) + 1);
                if (col + 1 < size) v = std::min(v, at(r - 1, col + 1) + 1);
            }
        }
    for (int r = size - 1; r >= 0; --r)
        for (int col = size - 1; col >= 0; --col) {
            int& v = at(r, col);
            if (col + 1 < size) v = std::min(v, at(r, col + 1) + 1);
            if (r + 1 < size) {
                v = std::min(v, at(r + 1, col) + 1);
                if (col + 1 < size) v = std::min(v, at(r + 1, col + 1) + 1);
                if (col > 0) v = std::min(v, at(r + 1, col - 1) + 1);
            }
        }
    for (size_t i = 0; i < n; ++i) {
        if (d[i] <= c.desertRadius) {
            plan.weight[i] = 0.0f;
            ++plan.desert;
        } else if (d[i] <= c.desertRadius + c.desertFade) {
            plan.weight[i] = float(d[i] - c.desertRadius) / float(c.desertFade + 1);
            ++plan.fade;
        }
    }

    // The game's own cold places: night below the floor -> that texel and its 3x3 neighbours keep
    // the game's values (the sampler blends 2x2 texels, so the blend there is the game's own).
    for (int r = 0; r < size; ++r)
        for (int col = 0; col < size; ++col) {
            const uint32_t t = orig[size_t(r) * size_t(size) + size_t(col)];
            if (topC(t) - climateRangeC(t) >= c.nightFloorC) continue;
            for (int dr = -1; dr <= 1; ++dr)
                for (int dc = -1; dc <= 1; ++dc) {
                    const int rr = r + dr, cc = col + dc;
                    if (rr >= 0 && cc >= 0 && rr < size && cc < size) plan.cold[size_t(rr) * size_t(size) + size_t(cc)] = 1;
                }
        }
    for (size_t i = 0; i < n; ++i) {
        plan.gameCold += plan.cold[i];
        plan.least[i] = uint8_t(climateTopByteUp(c.nightFloorC + climateRangeC(orig[i])));
    }
    // A warming never creates a new desert (keeps the weather module's desert check on the live map
    // equal to the game's own): stay below the desert line.
    const uint32_t line = climateTopByteUp(c.desertTopC);
    plan.capByte = line > 0 ? line - 1 : 0;
    return true;
}

void applyClimate(const std::vector<uint32_t>& orig, const ClimatePlan& plan, const ClimateConfig& c, float offsetC,
                  std::vector<uint32_t>& out, ClimateApplyStats* stats) {
    (void)c;
    out = orig;
    ClimateApplyStats st;
    if (!std::isfinite(offsetC) || offsetC == 0.0f || plan.weight.size() != orig.size()) {
        if (stats) *stats = st;
        return;
    }
    for (size_t i = 0; i < orig.size(); ++i) {
        float shift = offsetC * plan.weight[i];
        if (shift < 0.0f && plan.cold[i]) shift = 0.0f;
        if (shift == 0.0f) continue;
        const uint32_t t = orig[i], ob = t >> 24;
        uint32_t b = climateTopByte(topC(t) + shift);
        if (shift < 0.0f) {
            if (b < plan.least[i]) {
                b = plan.least[i];
                ++st.floored;
            }
            b = std::min(b, ob);  // a cooling never warms
        } else {
            if (b > plan.capByte) {
                b = plan.capByte;
                ++st.capped;
            }
            b = std::max(b, ob);  // a warming never cools
        }
        if (b != ob) {
            out[i] = (t & 0x00FFFFFFu) | (b << 24);
            ++st.changed;
        }
    }
    if (stats) *stats = st;
}

}  // namespace qs::apply
