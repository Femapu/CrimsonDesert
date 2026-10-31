// QuattroStagioni - unit tests of the seasonal lightning (src/core/apply/lightning.h).
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "../src/core/apply/lightning.h"

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

// Simulates `storms` storms of `stormHours` game hours (1 game hour = 150 real s, 0.25 s steps),
// with clear weather between them. Returns strikes per storm hour.
static double simulate(Phase phase, int storms, double stormHours, bool allowed = true, float minGap = 0,
                       float intensity = 1.0f, uint32_t seed = 7) {
    LightningConfig c;
    c.minGapSec = minGap;
    LightningDirector d;
    d.configure(c);
    d.seed(seed);
    LightningInput in;
    in.phase = phase;
    in.progress = 0.3f;  // plateau: the phase's own values
    in.allowed = allowed;
    in.intensity = intensity;
    const double stepSec = 0.25, hoursPerSec = 1.0 / 150.0;
    const long stormSteps = long(stormHours / hoursPerSec / stepSec);
    const long gapSteps = long((LightningDirector::kStormGraceSec + 5) / stepSec);
    uint64_t strikes = 0;
    for (int s = 0; s < storms; ++s) {
        in.storm = true;
        for (long i = 0; i < stormSteps; ++i) strikes += d.advance(stepSec, stepSec * hoursPerSec, in);
        in.storm = false;
        for (long i = 0; i < gapSteps; ++i) strikes += d.advance(stepSec, stepSec * hoursPerSec, in);
    }
    CHECK(d.storms() == uint64_t(storms));
    return double(strikes) / (storms * stormHours);
}

int main() {
    // ---- defaults and the ini template round trip
    {
        LightningConfig def;
        CHECK(def.enabled && near(def.chancePerStorm[Phase::Summer], 0.9) && near(def.strikesPerHour[Phase::Winter], 1));
        Ini ini;
        ini.parse(lightningIniTemplate());
        LightningConfig c;
        c.minGapSec = 99;
        std::vector<std::string> w;
        c.load(ini, w);
        CHECK(w.empty());
        CHECK(near(c.minGapSec, def.minGapSec) && near(c.pulseMs, def.pulseMs) && near(c.rampSec, def.rampSec));
        for (int p = 0; p < season::kPhaseCount; ++p)
            CHECK(near(c.chancePerStorm.v[p], def.chancePerStorm.v[p]) && near(c.strikesPerHour.v[p], def.strikesPerHour.v[p]));
        CHECK(lightningIniTemplate().find("\xE2\x80\x94") == std::string::npos);  // no em dash
    }
    // ---- tolerant parsing
    {
        Ini ini;
        ini.parse("[Lightning]\nEnabled=0\nChancePerStorm=1, 1, 1, 1, 2\nStrikesPerHour=1,2,3\nMinGapSec=abc\nPulseMs=1\nLogStrikes=1\n");
        LightningConfig c;
        std::vector<std::string> w;
        c.load(ini, w);
        CHECK(!c.enabled && c.logStrikes);
        CHECK(near(c.chancePerStorm[Phase::Winter], 1));          // clamped to 1
        CHECK(near(c.strikesPerHour[Phase::Summer], 14));         // malformed: default kept
        CHECK(near(c.minGapSec, 3));                              // not a number: default kept
        CHECK(near(c.pulseMs, 16));                               // clamped
        CHECK(w.size() == 3);
    }
    // ---- targets.ini [lightning]
    {
        LightningTarget t;
        CHECK(!t.enabled && !t.field.valid && !t.usable());
        Ini ini;
        ini.parse("[lightning]\nEnabled=1\nField=atmosphere:0x1A0\nMode=Pulse\nValue=5.5\n");
        std::vector<std::string> w;
        t.load(ini, w);
        CHECK(w.empty() && t.usable() && t.mode == LightningMode::Pulse && near(t.value, 5.5) && t.field.offset == 0x1A0);
        Ini bad;
        bad.parse("[lightning]\nEnabled=1\nField=sky:0x10\nMode=blink\n");
        LightningTarget b;
        w.clear();
        b.load(bad, w);
        CHECK(!b.usable() && w.size() == 2 && b.mode == LightningMode::Level);
        Ini empty;
        empty.parse("[lightning]\nEnabled=1\nField=\n");
        LightningTarget e;
        w.clear();
        e.load(empty, w);
        CHECK(w.empty() && e.enabled && !e.usable());
    }
    // ---- interpolation in the transitions (tail 0.4: progress 0.9 = 75% toward the next phase)
    {
        LightningConfig c;
        LightningInput in;
        in.phase = Phase::Spring;
        in.progress = 0.9f;
        in.transitionFraction = 0.4f;
        CHECK(near(LightningDirector::seasonal(c.strikesPerHour, in), 8 + 6 * 0.75));
        in.progress = 0.3f;
        CHECK(near(LightningDirector::seasonal(c.strikesPerHour, in), 8));
        in.phase = Phase::Autumn;
        in.progress = 1.0f;
        CHECK(near(LightningDirector::seasonal(c.chancePerStorm, in), 0.03, 1e-3));
        LightningDirector d;
        d.configure(c);
        in.storm = true;
        in.progress = 0.8f;  // autumn to winter, half way: rate 6.5
        d.advance(0.25, 0, in);
        CHECK(near(d.chance(), (0.85 + 0.03) / 2, 1e-3));
        if (d.stormHasLightning()) CHECK(near(d.rate(), 6.5, 1e-3));
    }
    // ---- mean frequencies per season (many simulated storm hours)
    {
        const double spring = simulate(Phase::Spring, 400, 2);
        const double summer = simulate(Phase::Summer, 400, 2);
        const double late = simulate(Phase::LateSummer, 400, 2);
        const double autumn = simulate(Phase::Autumn, 400, 2);
        const double winter = simulate(Phase::Winter, 400, 2);
        std::printf("strikes per storm hour: spring %.2f summer %.2f late %.2f autumn %.2f winter %.3f\n", spring, summer, late,
                    autumn, winter);
        CHECK(std::fabs(summer - 0.9 * 14) < 0.12 * 12.6);
        CHECK(std::fabs(spring - 0.6 * 8) < 0.15 * 4.8);
        CHECK(std::fabs(autumn - 0.85 * 12) < 0.12 * 10.2);
        CHECK(std::fabs(late - 0.85 * 12) < 0.12 * 10.2);
        CHECK(winter < 0.2);
        CHECK(summer > spring && autumn > spring && winter < spring / 10);
        // desert / off: never
        CHECK(simulate(Phase::Summer, 50, 2, false) == 0.0);
        // half intensity: about half the strikes
        const double half = simulate(Phase::Summer, 400, 2, true, 0, 0.5f);
        CHECK(std::fabs(half - summer / 2) < 0.15 * summer / 2);
        // the gap: at 3 s minimum, never closer
        LightningConfig c;
        c.minGapSec = 3;
        c.strikesPerHour.v[1] = 600;
        c.chancePerStorm.v[1] = 1;
        LightningDirector d;
        d.configure(c);
        d.seed(3);
        LightningInput in;
        in.storm = true;
        double since = 1e9, minSeen = 1e9;
        for (int i = 0; i < 20000; ++i) {
            since += 0.25;
            if (d.advance(0.25, 0.25 / 150.0, in)) {
                minSeen = std::min(minSeen, since);
                since = 0;
            }
        }
        CHECK(minSeen >= 3.0 && d.strikes() > 100);
    }
    // ---- determinism, pulse, level, no strikes while the clock stands still
    {
        CHECK(simulate(Phase::Summer, 30, 1, true, 3, 1, 11) == simulate(Phase::Summer, 30, 1, true, 3, 1, 11));
        LightningConfig c;
        c.chancePerStorm.v[1] = 1;
        c.strikesPerHour.v[1] = 600;
        LightningDirector d;
        d.configure(c);
        LightningInput in;
        in.storm = true;
        for (int i = 0; i < 400; ++i) d.advance(0.25, 0, in);
        CHECK(d.strikes() == 0 && d.stormHasLightning());
        CHECK(near(d.level(), 1, 1e-3));  // summer is the busiest phase: level = intensity
        bool pulsed = false, pulseEnded = false;
        for (int i = 0; i < 4000 && !pulsed; ++i) pulsed = d.advance(0.1, 0.1 / 150.0, in) == 1 && d.strikeActive();
        CHECK(pulsed);
        d.advance(0.3, 0, in);
        pulseEnded = !d.strikeActive();
        CHECK(pulseEnded);
        in.allowed = false;  // entering the desert: level falls, no pulse
        for (int i = 0; i < 100; ++i) CHECK(d.advance(0.25, 0.25 / 150.0, in) == 0);
        CHECK(d.level() < 1e-6 && !d.strikeActive());
        in.allowed = true;
        in.storm = false;
        for (int i = 0; i < 100; ++i) d.advance(0.25, 0, in);
        CHECK(!d.inStorm());
        std::vector<std::string> log = d.takeLog();
        CHECK(log.size() == 2 && log[0].find("storm begins") == 0 && log[1] == "storm ends");
        CHECK(d.takeLog().empty());
    }

    std::printf("test_lightning: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
