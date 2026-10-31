// QuattroStagioni - unit tests of the seasonal temperatures (src/core/apply/climate.h).
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "../src/core/apply/climate.h"

using namespace qs;
using namespace qs::apply;
using season::Phase;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond)                                                                         \
    do {                                                                                    \
        if (cond) ++g_pass;                                                                 \
        else {                                                                              \
            ++g_fail;                                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                     \
        }                                                                                   \
    } while (0)

static bool near(double a, double b, double eps = 1e-3) { return std::fabs(a - b) <= eps; }
static float top(uint32_t t) { return (float(t >> 24) / 255.0f * 2.0f - 1.0f) * 50.0f; }
static uint32_t texel(float topC, uint8_t g, uint8_t r = 3, uint8_t a = 7) {
    return uint32_t(a) | (uint32_t(r) << 8) | (uint32_t(g) << 16) | (climateTopByte(topC) << 24);
}

constexpr int N = 64;
static size_t idx(int r, int c) { return size_t(r) * N + size_t(c); }

// 20 C land; desert 40 C in rows/cols 0..5; a cold mountain (-40 C) at rows/cols 40..43; a cold-night
// spot (top 0 C, range 25) at (30, 30); a warm 26 C texel at (20, 50).
static std::vector<uint32_t> synthMap() {
    std::vector<uint32_t> m(size_t(N) * N, texel(20.0f, 128));
    for (int r = 0; r <= 5; ++r)
        for (int c = 0; c <= 5; ++c) m[idx(r, c)] = texel(40.0f, 200);
    for (int r = 40; r <= 43; ++r)
        for (int c = 40; c <= 43; ++c) m[idx(r, c)] = texel(-40.0f, 128);
    m[idx(30, 30)] = texel(0.0f, 0);
    m[idx(20, 50)] = texel(26.0f, 128);
    return m;
}

static void testOffsets() {
    ClimateConfig c;
    CHECK(near(climateOffsetC(c, Phase::Summer, 0.2f, 0.4f), 4.0f));
    CHECK(near(climateOffsetC(c, Phase::Winter, 0.0f, 0.4f), -24.0f));
    CHECK(near(climateOffsetC(c, Phase::Autumn, 0.59f, 0.4f), -8.0f));      // tail starts at 0.6
    CHECK(near(climateOffsetC(c, Phase::Autumn, 0.8f, 0.4f), -16.0f));      // middle of A>W
    CHECK(near(climateOffsetC(c, Phase::Autumn, 1.0f, 0.4f), -24.0f));      // end = winter
    CHECK(near(climateOffsetC(c, Phase::Winter, 1.0f, 0.4f), -6.0f));       // W>S wraps to spring
    CHECK(near(climateOffsetC(c, Phase::LateSummer, 1.0f, 0.4f), -8.0f));
    CHECK(near(climateOffsetC(c, Phase::Spring, 0.5f, 0.0f), -6.0f));       // hard switch
    // monotone along a tail
    float prev = 10;
    bool mono = true;
    for (int i = 0; i <= 20; ++i) {
        float v = climateOffsetC(c, Phase::Autumn, 0.6f + 0.02f * float(i), 0.4f);
        mono &= v <= prev + 1e-6f;
        prev = v;
    }
    CHECK(mono);
    CHECK(near(climateRamp(0, -24, 0.5f, 1.0f), -0.5f));
    CHECK(near(climateRamp(-23.8f, -24, 0.5f, 1.0f), -24.0f));
    CHECK(near(climateRamp(-24, 4, 0.5f, 2.0f), -23.0f));
    CHECK(near(climateRamp(1, 1, 0.5f, 0.0f), 1.0f));
}

static void testBytes() {
    CHECK(near(top(climateTopByte(24.5f) << 24), 24.5f, 0.2));
    CHECK(climateTopByte(-100) == 0 && climateTopByte(100) == 255);
    for (float v = -45; v < 45; v += 0.37f) CHECK(top(climateTopByteUp(v) << 24) >= v - 1e-4f);
    CHECK(near(climateRangeC(texel(0, 255)), 7.0f) && near(climateRangeC(texel(0, 0)), 25.0f));
}

static void testPlanAndApply() {
    ClimateConfig c;
    std::vector<uint32_t> m = synthMap();
    ClimatePlan p;
    std::string why;
    CHECK(buildClimatePlan(m, N, c, p, &why));
    CHECK(p.kind(idx(0, 0)) == TexelKind::Desert);
    CHECK(p.kind(idx(9, 9)) == TexelKind::Desert);          // 5 + radius 4
    CHECK(p.kind(idx(10, 2)) == TexelKind::DesertFade);
    CHECK(p.weight[idx(10, 2)] > 0 && p.weight[idx(10, 2)] < p.weight[idx(12, 2)]);
    CHECK(p.kind(idx(14, 2)) == TexelKind::Normal);         // past radius + fade
    CHECK(p.kind(idx(39, 39)) == TexelKind::GameCold);      // 3x3 neighbour of the mountain
    CHECK(p.kind(idx(38, 38)) == TexelKind::Normal);
    CHECK(p.kind(idx(30, 30)) == TexelKind::Normal);        // night -25 >= -30: not the game's cold

    std::vector<uint32_t> out;
    applyClimate(m, p, c, 0.0f, out);
    CHECK(out == m);

    ClimateApplyStats st;
    applyClimate(m, p, c, -24.0f, out, &st);
    CHECK(near(top(out[idx(30, 2)]), top(m[idx(30, 2)]) - 24.0f, 0.25));         // ordinary land
    CHECK(out[idx(2, 2)] == m[idx(2, 2)]);                  // desert
    CHECK(out[idx(9, 9)] == m[idx(9, 9)]);
    CHECK(top(out[idx(11, 2)]) > -4.0f && top(out[idx(11, 2)]) < 20.0f);  // fade band
    CHECK(out[idx(41, 41)] == m[idx(41, 41)] && out[idx(39, 44)] == m[idx(39, 44)]);  // game's cold
    // night floor: top 0, range 25 -> -24 would give a -49 night; held at >= -30
    const uint32_t f = out[idx(30, 30)];
    CHECK(top(f) - climateRangeC(f) >= c.nightFloorC - 1e-3f);
    CHECK(top(f) < 0.0f);
    CHECK(st.floored >= 1 && st.changed > 1000);
    bool lowBytesKept = true, nightsOk = true;
    for (size_t i = 0; i < m.size(); ++i) {
        lowBytesKept &= (out[i] & 0x00FFFFFFu) == (m[i] & 0x00FFFFFFu);
        if (out[i] != m[i]) nightsOk &= top(out[i]) - climateRangeC(out[i]) >= c.nightFloorC - 1e-3f;
    }
    CHECK(lowBytesKept && nightsOk);

    // summer warming stays below the desert line (the weather module's live desert check)
    applyClimate(m, p, c, 4.0f, out, &st);
    CHECK(near(top(out[idx(30, 2)]), top(m[idx(30, 2)]) + 4.0f, 0.25));
    CHECK(top(out[idx(20, 50)]) < c.desertTopC && top(out[idx(20, 50)]) >= 26.0f - 0.2f);
    CHECK(st.capped >= 1);
    CHECK(top(out[idx(41, 41)]) > -40.0f + 3.0f);           // warming also on the cold mountain
}

static void testPlanRejects() {
    ClimateConfig c;
    ClimatePlan p;
    std::string why;
    std::vector<uint32_t> m(size_t(N) * N, texel(20.0f, 128));
    CHECK(!buildClimatePlan(m, N, c, p, &why) && !why.empty());   // no desert
    CHECK(!buildClimatePlan(synthMap(), N + 1, c, p, &why));      // wrong size
    std::vector<uint32_t> z(size_t(N) * N, 0xFFFFFFFFu);
    CHECK(!buildClimatePlan(z, N, c, p, &why));                   // all hot: implausible
}

static void testIni() {
    ClimateConfig d, c;
    Ini ini;
    ini.parse(climateIniTemplate());
    std::vector<std::string> warn;
    c.load(ini, warn);
    CHECK(warn.empty());
    bool same = c.enabled == d.enabled && c.nightFloorC == d.nightFloorC && c.desertRadius == d.desertRadius &&
                c.desertFade == d.desertFade && c.startDelayMs == d.startDelayMs;
    for (int i = 0; i < season::kPhaseCount; ++i) same &= c.offsetC[i] == d.offsetC[i];
    CHECK(same);
    Ini bad;
    bad.parse("[Climate]\nWinterOffsetC=abc\nSummerOffsetC=99\nEnabled=0\n");
    ClimateConfig b;
    warn.clear();
    b.load(bad, warn);
    CHECK(warn.size() == 1 && b.offsetC[4] == -24.0f && b.offsetC[1] == 15.0f && !b.enabled);
    Ini empty;
    ClimateConfig e;
    e.load(empty, warn);
    CHECK(e.enabled && e.offsetC[4] == -24.0f);
}

int main() {
    testOffsets();
    testBytes();
    testPlanAndApply();
    testPlanRejects();
    testIni();
    std::printf("test_climate: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
