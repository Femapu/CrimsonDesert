// QuattroStagioni - unit tests of the seasonal particles (src/core/apply/particles.h).
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include "../src/core/apply/particles.h"

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

static season::VisualStage stage(Phase from, Phase to, int step, int steps = 3) {
    season::VisualStage s;
    s.from = from;
    s.to = to;
    s.step = step;
    s.t = float(step) / float(steps + 1);
    return s;
}

int main() {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    ParticlesConfig c;  // defaults: boost 1.25, min 0.35, active above 0.05

    // ---- flakes: never snow from nothing
    CHECK(flakes(0.0f, 1.0f, 1.0f, 0.0f, c) == 0.0f);
    CHECK(flakes(0.04f, 1.0f, 1.0f, 0.0f, c) == 0.04f);   // below SnowMinActive
    CHECK(flakes(0.05f, 1.0f, 1.0f, 0.0f, c) == 0.05f);
    // boost while snowing in full winter
    CHECK(near(flakes(0.6f, 0.0f, 1.0f, 0.0f, c), 0.75f));
    CHECK(near(flakes(0.2f, 0.0f, 1.0f, 0.0f, c), 0.25f));   // no precipitation: no minimum
    CHECK(near(flakes(0.2f, 0.5f, 1.0f, 0.0f, c), 0.35f));   // precipitation: at least WinterFlakeMin
    CHECK(near(flakes(0.9f, 1.0f, 1.0f, 0.0f, c), 1.0f));    // capped at 1
    CHECK(flakes(1.0f, 1.0f, 1.0f, 0.0f, c) == 1.0f);
    CHECK(flakes(1.5f, 1.0f, 1.0f, 0.0f, c) == 1.5f);        // the game's own value above 1 stays
    // only in winter
    CHECK(flakes(0.6f, 1.0f, 0.0f, 0.0f, c) == 0.6f);
    CHECK(near(flakes(0.6f, 0.0f, 0.5f, 0.0f, c), 0.675f));  // half winter: half the boost
    // desert / unknown -> the game's value
    CHECK(flakes(0.6f, 1.0f, 1.0f, 1.0f, c) == 0.6f);
    CHECK(near(flakes(0.6f, 0.0f, 1.0f, 0.5f, c), 0.675f));
    CHECK(flakes(0.6f, 1.0f, 1.0f, nan, c) == 0.6f);
    // NaN snow left to the game, NaN inputs harmless
    CHECK(std::isnan(flakes(nan, 1.0f, 1.0f, 0.0f, c)));
    CHECK(flakes(0.6f, 1.0f, nan, 0.0f, c) == 0.6f);
    CHECK(near(flakes(0.6f, nan, 1.0f, 0.0f, c), 0.75f));
    // never lower than the game's snow, always 0..1
    for (float s = 0.0f; s <= 1.0f; s += 0.05f)
        for (float p = 0.0f; p <= 1.0f; p += 0.25f)
            for (float k = 0.0f; k <= 1.0f; k += 0.25f) {
                const float o = flakes(s, p, k, 0.0f, c);
                CHECK(o >= s && o <= 1.0f);
            }
    // switches
    ParticlesConfig off = c;
    off.winterFlakes = false;
    CHECK(flakes(0.6f, 1.0f, 1.0f, 0.0f, off) == 0.6f);
    off = c;
    off.enabled = false;
    CHECK(flakes(0.6f, 1.0f, 1.0f, 0.0f, off) == 0.6f);

    // ---- parsing
    {
        Ini ini;
        ini.parse("[Particles]\nEnabled=0\nWinterFlakes=0\nWinterFlakeBoost=1,5\nWinterFlakeMin=0.4\nSnowMinActive=0.1\n"
                  "RampPerSec=0.5\nPollenPerPhase=0.8, 0.1, 0, 0, 0\n");
        ParticlesConfig p;
        std::vector<std::string> w;
        p.load(ini, w);
        CHECK(w.empty());
        CHECK(!p.enabled && !p.winterFlakes);
        CHECK(near(p.winterFlakeBoost, 1.5f) && near(p.winterFlakeMin, 0.4f) && near(p.snowMinActive, 0.1f));
        CHECK(near(p.rampPerSec, 0.5f) && near(p.pollenPerPhase[Phase::Spring], 0.8f));
        CHECK(p.leavesFromSeason);  // no LeavesPerPhase
    }
    {
        Ini ini;
        ini.parse("[Particles]\nWinterFlakeBoost=9\nWinterFlakeMin=abc\nLeavesPerPhase=0, 0, 0.5, 2, 0\nPollenPerPhase=1, 2\n");
        ParticlesConfig p;
        std::vector<std::string> w;
        p.load(ini, w);
        CHECK(w.size() == 4);                       // clamp boost, bad min, clamp leaves, bad pollen
        CHECK(near(p.winterFlakeBoost, 3.0f));      // clamped
        CHECK(near(p.winterFlakeMin, 0.35f));       // default kept
        CHECK(!p.leavesFromSeason && near(p.leavesPerPhase[Phase::Autumn], 1.0f));
        CHECK(near(p.pollenPerPhase[Phase::Spring], 1.0f));  // default kept
    }
    {
        Ini ini;
        ini.parse("[Particles]\nLeavesPerPhase=x\n");
        ParticlesConfig p;
        std::vector<std::string> w;
        p.load(ini, w);
        CHECK(w.size() == 1 && p.leavesFromSeason);
    }
    {
        Ini ini;  // empty: defaults
        ParticlesConfig p;
        std::vector<std::string> w;
        p.load(ini, w);
        CHECK(w.empty() && p.enabled && p.winterFlakes && near(p.winterFlakeBoost, 1.25f));
        // the template parses to the defaults
        Ini t;
        t.parse(particlesIniTemplate());
        ParticlesConfig q;
        q.load(t, w);
        CHECK(w.empty() && q.enabled && near(q.winterFlakeBoost, 1.25f) && near(q.winterFlakeMin, 0.35f) && q.leavesFromSeason);
        CHECK(particlesIniTemplate().find("\xE2\x80\x94") == std::string::npos);  // no em dash
    }

    // ---- interpolation of leaves and pollen
    CHECK(near(leavesIntensity(c, stage(Phase::Autumn, Phase::Autumn, 0)), 1.0f));
    CHECK(near(leavesIntensity(c, stage(Phase::Spring, Phase::Spring, 0)), 0.0f));
    CHECK(near(leavesIntensity(c, stage(Phase::LateSummer, Phase::Autumn, 2)), 0.35f + (1.0f - 0.35f) * 0.5f));
    CHECK(near(leavesIntensity(c, stage(Phase::Autumn, Phase::Autumn, 0), 0.0f), 0.0f));  // evergreen region
    CHECK(near(leavesIntensity(c, stage(Phase::Autumn, Phase::Autumn, 0), 3.0f), 1.0f));  // limited to 1
    CHECK(near(pollenIntensity(c, stage(Phase::Spring, Phase::Spring, 0)), 1.0f));
    CHECK(near(pollenIntensity(c, stage(Phase::Autumn, Phase::Autumn, 0)), 0.0f));
    CHECK(near(pollenIntensity(c, stage(Phase::Winter, Phase::Winter, 0)), 0.0f));
    CHECK(near(pollenIntensity(c, stage(Phase::Spring, Phase::Summer, 1)), 1.0f + (0.25f - 1.0f) * 0.25f));

    // ---- [particles] of targets.ini
    {
        Ini ini;  // shipped state: off, no field
        ini.parse("[particles]\nEnabled=0\nLeaves=\nPollen=\nMode=scale\nValue=2\n");
        ParticleFields f;
        std::vector<std::string> w;
        f.load(ini, w);
        CHECK(w.empty() && !f.enabled && !f.leaves.valid && !f.pollen.valid);
    }
    {
        Ini ini;
        ini.parse("[particles]\nEnabled=1\nLeaves=atmosphere:0x1A0\nPollen=sky:0x10\nMode=Write\nValue=0.8\nMax=2\n");
        ParticleFields f;
        std::vector<std::string> w;
        f.load(ini, w);
        CHECK(w.size() == 1);  // bad Pollen
        CHECK(f.enabled && f.leaves.valid && f.leaves.offset == 0x1A0 && !f.pollen.valid);
        CHECK(f.mode == ParticleMode::Write && near(f.value, 0.8f) && near(f.maxValue, 2.0f));
        CHECK(near(particleField(0.3f, 1.0f, 0.0f, f), 0.8f));
        CHECK(near(particleField(0.3f, 0.5f, 0.0f, f), 0.4f));
        CHECK(near(particleField(0.3f, 1.0f, 1.0f, f), 0.3f));  // desert: the game's
    }
    {
        ParticleFields f;  // scale x2, Max 1
        CHECK(near(particleField(0.3f, 1.0f, 0.0f, f), 0.6f));
        CHECK(near(particleField(0.3f, 0.0f, 0.0f, f), 0.3f));  // intensity 0: the game's own
        CHECK(near(particleField(0.8f, 1.0f, 0.0f, f), 1.0f));  // limited to Max
        CHECK(near(particleField(1.5f, 0.0f, 0.0f, f), 1.5f));  // never cut what the game wrote
        CHECK(std::isnan(particleField(nan, 1.0f, 0.0f, f)));
        CHECK(near(particleField(0.3f, nan, 0.0f, f), 0.3f));
    }

    std::printf("test_particles: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
