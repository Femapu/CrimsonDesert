// QuattroStagioni - unit tests of the shared "after compose" helpers (src/core/apply/layer_field.h).
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "../src/core/apply/layer_field.h"

using namespace qs;
using namespace qs::apply;
using season::Phase;

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

int main() {
    // ---- LayerRef
    LayerRef r;
    CHECK(parseLayerRef("", r) && !r.valid);
    CHECK(parseLayerRef("atmosphere:0x1A0:3", r) && r.valid && r.layer == Layer::Atmosphere && r.offset == 0x1A0 && r.count == 3);
    CHECK(r.text() == "atmosphere:0x1A0:3");
    LayerRef r2;
    CHECK(parseLayerRef(r.text(), r2) && r2.offset == r.offset && r2.count == 3);
    CHECK(parseLayerRef(" Weather : 0x16C ", r) && r.layer == Layer::Weather && r.offset == 0x16C && r.count == 1);
    CHECK(r.text() == "weather:0x16C");
    CHECK(parseLayerRef("manager:0x100200", r) && r.layer == Layer::Manager);
    std::string err;
    CHECK(!parseLayerRef("sky:0x10", r, &err) && !err.empty());
    CHECK(!parseLayerRef("weather:0x16D", r));       // not aligned
    CHECK(!parseLayerRef("weather:0x10000", r));     // too far
    CHECK(!parseLayerRef("weather:zz", r));
    CHECK(!parseLayerRef("weather:0x10:5", r));      // count
    CHECK(!parseLayerRef("weather:0x10:0", r));
    CHECK(!parseLayerRef("weather", r));

    // ---- PhaseValues
    PhaseValues p;
    CHECK(parsePhaseValues("1, 1.1, 1, 0.9, 0.7", p) && near(p.v[1], 1.1) && near(p.v[4], 0.7));
    CHECK(parsePhaseValues("0 1 2 3 4", p) && near(p[Phase::Autumn], 3));
    PhaseValues keep = p;
    CHECK(!parsePhaseValues("1,2,3,4", p, &err) && near(p.v[3], keep.v[3]));
    CHECK(!parsePhaseValues("1,2,3,4,5,6", p));
    CHECK(!parsePhaseValues("1,2,x,4,5", p));
    CHECK(!parsePhaseValues("1,2,nan,4,5", p));
    PhaseValues q;
    CHECK(parsePhaseValues(formatPhaseValues(p), q) && near(q.v[2], p.v[2]));

    Ini ini;
    ini.parse("[Lighting]\nSun=1,2,3,4,5\nBad=1,2\nBig=0,0,9,0,0\n");
    std::vector<std::string> warn;
    PhaseValues a, b, c, d;
    loadPhaseValues(ini, "Lighting", "Sun", a, 0, 10, warn);
    CHECK(near(a.v[4], 5) && warn.empty());
    loadPhaseValues(ini, "Lighting", "Bad", b, 0, 10, warn);
    CHECK(near(b.v[0], 1) && warn.size() == 1);
    loadPhaseValues(ini, "Lighting", "Big", c, 0, 2, warn);
    CHECK(near(c.v[2], 2) && warn.size() == 2);
    loadPhaseValues(ini, "Lighting", "Missing", d, 0, 2, warn);
    CHECK(near(d.v[0], 1) && warn.size() == 2);

    // ---- stageValue: plateau and the 15 transitions (5 phases x 3 steps)
    PhaseValues lin;
    parsePhaseValues("0, 10, 20, 30, 40", lin);
    season::VisualStage st;
    st.from = Phase::Autumn;
    st.to = Phase::Autumn;
    CHECK(near(stageValue(lin, st), 30));
    int transitions = 0;
    for (int ph = 0; ph < season::kPhaseCount; ++ph)
        for (int k = 1; k <= 3; ++k) {
            season::VisualStage s;
            s.from = Phase(ph);
            s.to = season::nextPhase(Phase(ph));
            s.step = k;
            s.t = float(k) / 4.0f;
            float v = stageValue(lin, s);
            float exp = lin.v[ph] + (lin.v[(ph + 1) % 5] - lin.v[ph]) * s.t;
            CHECK(near(v, exp));
            ++transitions;
        }
    CHECK(transitions == 15);
    // winter -> spring wraps (40 -> 0)
    season::VisualStage ws;
    ws.from = Phase::Winter;
    ws.to = Phase::Spring;
    ws.step = 2;
    ws.t = 0.5f;
    CHECK(near(stageValue(lin, ws), 20));

    // ---- phaseValue: plateau then tail
    CHECK(near(phaseValue(lin, Phase::Summer, 0.3f, 0.4f), 10));
    CHECK(near(phaseValue(lin, Phase::Summer, 0.6f, 0.4f), 10));
    CHECK(near(phaseValue(lin, Phase::Summer, 0.8f, 0.4f), 15));
    CHECK(near(phaseValue(lin, Phase::Summer, 1.0f, 0.4f), 20));
    CHECK(near(phaseValue(lin, Phase::Winter, 1.0f, 0.4f), 0));
    CHECK(near(phaseValue(lin, Phase::Summer, 0.99f, 0.0f), 10));

    // ---- approach
    CHECK(near(approach(0, 1, 0.25f), 0.25));
    CHECK(near(approach(1, 0, 0.25f), 0.75));
    CHECK(near(approach(0.9f, 1, 0.25f), 1));
    CHECK(near(approach(0.5f, NAN, 0.25f), 0.5));
    CHECK(near(approach(NAN, 0.3f, 0.25f), 0.3));

    // ---- Rng: deterministic, in range
    Rng g1, g2;
    g1.seed(42);
    g2.seed(42);
    bool same = true, inRange = true;
    for (int i = 0; i < 1000; ++i) {
        float u = g1.uniform();
        same &= u == g2.uniform();
        inRange &= u >= 0 && u < 1;
    }
    CHECK(same && inRange);

    std::printf("test_layer_field: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
