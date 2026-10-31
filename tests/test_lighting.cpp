// QuattroStagioni - unit tests of the seasonal light and sky (src/core/apply/lighting.h).
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include "../src/core/apply/lighting.h"

using namespace qs;
using namespace qs::apply;
using season::Phase;
using season::VisualStage;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (cond) ++g_pass;                                             \
        else {                                                          \
            ++g_fail;                                                   \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                               \
    } while (0)

static bool near(double a, double b, double eps = 1e-4) { return std::fabs(a - b) <= eps; }

static VisualStage plateau(Phase p) {
    VisualStage s;
    s.from = s.to = p;
    return s;
}
static VisualStage trans(Phase from, int step, int steps) {
    VisualStage s;
    s.from = from;
    s.to = season::nextPhase(from);
    s.step = step;
    s.t = float(step) / float(steps + 1);
    return s;
}

int main() {
    const float kNan = std::numeric_limits<float>::quiet_NaN();
    const float kInf = std::numeric_limits<float>::infinity();

    // ---- defaults: summer = the game, winter weaker and colder
    LightingConfig c;
    LightingModel m;
    m.configure(c);
    const LightingMultipliers su = m.target(plateau(Phase::Summer));
    for (int i = 0; i < kLightChannels; ++i) CHECK(su.v[i] == 1.0f);
    const LightingMultipliers wi = m.target(plateau(Phase::Winter));
    CHECK(wi[LightChannel::SunIntensity] < 1.0f);
    CHECK(wi[LightChannel::AmbientIntensity] < 1.0f);
    CHECK(wi[LightChannel::SunB] > wi[LightChannel::SunR]);  // cold
    const LightingMultipliers au = m.target(plateau(Phase::Autumn));
    CHECK(au[LightChannel::SunR] > au[LightChannel::SunB]);  // warm
    for (int p = 0; p < season::kPhaseCount; ++p)
        for (int i = 0; i < kLightChannels; ++i) CHECK(c.ch[i].v[p] >= 0.85f && c.ch[i].v[p] <= 1.1f);

    // ---- the 15 transition stages (5 transitions x 3 steps) are monotone between the plateaus
    const int steps = 3;
    int seen = 0;
    for (int p = 0; p < season::kPhaseCount; ++p) {
        const Phase from = Phase(p);
        for (int i = 0; i < kLightChannels; ++i) {
            float prev = m.target(plateau(from)).v[i];
            const float end = m.target(plateau(season::nextPhase(from))).v[i];
            const bool up = end >= prev;
            for (int k = 1; k <= steps; ++k) {
                const float v = m.target(trans(from, k, steps)).v[i];
                CHECK(up ? (v >= prev && v <= end) : (v <= prev && v >= end));
                prev = v;
                if (i == 0) ++seen;
            }
        }
    }
    CHECK(seen == 15);
    // halfway step of Autumn > Winter
    CHECK(near(m.target(trans(Phase::Autumn, 2, steps))[LightChannel::SunIntensity], (0.95f + 0.88f) / 2));

    // ---- applyScale: bypass 1 (desert) = the game's value, 0 = full multiplier
    CHECK(near(applyScale(2.0f, 0.9f, 0.0f), 1.8f));
    CHECK(applyScale(2.0f, 0.9f, 1.0f) == 2.0f);
    CHECK(near(applyScale(2.0f, 0.8f, 0.5f), 1.8f));
    CHECK(applyScale(2.0f, 0.9f, 7.0f) == 2.0f);  // clamped bypass
    CHECK(std::isnan(applyScale(kNan, 0.9f, 0.0f)));  // left to the game
    CHECK(applyScale(kInf, 0.9f, 0.0f) == kInf);
    CHECK(applyScale(3.0f, kNan, 0.0f) == 3.0f);
    CHECK(applyScale(3.0f, 0.9f, kNan) == 3.0f);

    // ---- smoothing: from 1, RampPerSec per second, toward the stage (on) or back to 1 (off)
    LightingModel s;
    LightingConfig c2;
    c2.rampPerSec = 0.01f;
    s.configure(c2);
    CHECK(s.current()[LightChannel::SunIntensity] == 1.0f);
    s.step(plateau(Phase::Winter), true, 1.0f);
    CHECK(near(s.current()[LightChannel::SunIntensity], 0.99f));
    for (int i = 0; i < 100; ++i) s.step(plateau(Phase::Winter), true, 0.25f);
    CHECK(near(s.current()[LightChannel::SunIntensity], 0.88f));
    s.step(plateau(Phase::Winter), false, 2.0f);
    CHECK(near(s.current()[LightChannel::SunIntensity], 0.90f));
    s.step(plateau(Phase::Winter), true, kNan);  // bad dt: no move
    CHECK(near(s.current()[LightChannel::SunIntensity], 0.90f));

    // ---- [Lighting] parsing
    {
        Ini ini;
        ini.parse("[Lighting]\nEnabled=0\nRampPerSec=0,05\nSunIntensity=1, 1, 1, 0.9, 0.8\nFogDensity=1 1 1 1 5\n"
                  "SkyB=1, 2\nLogIntervalSec=abc\n");
        LightingConfig l;
        std::vector<std::string> w;
        l.load(ini, w);
        CHECK(!l.enabled);
        CHECK(near(l.rampPerSec, 0.05f));
        CHECK(near(l.ch[int(LightChannel::SunIntensity)].v[4], 0.8f));
        CHECK(near(l.ch[int(LightChannel::FogDensity)].v[4], 2.0f));    // clamped
        CHECK(near(l.ch[int(LightChannel::SkyB)].v[4], 1.03f));         // malformed: default kept
        CHECK(near(l.logIntervalSec, 60.0f));
        CHECK(w.size() == 3);
    }
    {
        // the documented template is the defaults, without warnings
        Ini ini;
        ini.parse(lightingIniTemplate());
        LightingConfig l;
        l.ch[0].v[0] = 0.5f;
        std::vector<std::string> w;
        l.load(ini, w);
        CHECK(w.empty());
        CHECK(l.enabled);
        LightingConfig d;
        for (int i = 0; i < kLightChannels; ++i)
            for (int p = 0; p < season::kPhaseCount; ++p) CHECK(near(l.ch[i].v[p], d.ch[i].v[p]));
    }

    // ---- targets.ini [lighting]
    {
        Ini ini;
        ini.parse("[lighting]\nEnabled=1\nSunColor=atmosphere:0x1A0:3\nSunIntensity=atmosphere:0x1AC\n"
                  "SkyColor=atmosphere:0x200\nFogDensity=weather:0x10:2\nAmbientColor=sky:0x10:3\n"
                  "SunIntensityMin=0\nSunIntensityMax=50\nSunColorMin=5\nSunColorMax=1\n");
        LightingTargets t;
        std::vector<std::string> w;
        t.load(ini, w);
        CHECK(t.enabled);
        CHECK(t.count() == 2);
        CHECK(t.field[int(LightField::SunColor)].ref.offset == 0x1A0);
        CHECK(t.field[int(LightField::SunIntensity)].maxV == 50.0f);
        CHECK(t.field[int(LightField::SunColor)].minV == 1.0f && t.field[int(LightField::SunColor)].maxV == 5.0f);
        CHECK(!t.field[int(LightField::SkyColor)].ref.valid);    // colour with one float
        CHECK(!t.field[int(LightField::FogDensity)].ref.valid);  // scalar with two
        CHECK(w.size() == 4);
        LightingTargets e;
        Ini empty;
        e.load(empty, w);
        CHECK(!e.enabled && e.count() == 0);
    }
    CHECK(lightFieldComponents(int(LightField::SkyColor)) == 3);
    CHECK(lightFieldChannel(int(LightField::SkyColor)) == int(LightChannel::SkyR));
    CHECK(std::string(lightChannelKey(int(LightChannel::FogDensity))) == "FogDensity");

    std::printf("test_lighting: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
