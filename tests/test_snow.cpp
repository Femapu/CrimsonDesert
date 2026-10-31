// QuattroStagioni - native unit tests for the winter snow thresholds (src/core/apply/snow.*).
// Usage: test_snow [data_dir]   (data_dir unused: everything is synthetic)
// Covers: [Snow] parsing with missing keys and caps, the winter intensity ramp, the target blend,
// attach validation (range, default, order), rate limited ramp in and out, immediate restore when
// switched off, restore on stop, foreign changes (game reset vs. another writer), refused writes.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../src/core/apply/snow.h"

using namespace qs::apply;
namespace season = qs::season;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond)                                                                         \
    do {                                                                                    \
        if (cond) ++g_pass;                                                                 \
        else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); }       \
    } while (0)

static bool near(double a, double b, double eps = 1e-3) { return std::fabs(a - b) <= eps; }

// Fake game: one data page with the two cvars (value +0x38, default +0x3C).
struct FakeMem : Memory {
    std::vector<uint8_t> page = std::vector<uint8_t>(0x200, 0);
    uint64_t base = 0x140000000ull;
    bool writable = true;
    int writes = 0;
    bool read(uint64_t a, void* out, size_t n) override {
        if (a < base || a + n > base + page.size()) return false;
        std::memcpy(out, page.data() + (a - base), n);
        return true;
    }
    bool write(uint64_t a, const void* in, size_t n, std::string* why) override {
        if (!writable || a < base || a + n > base + page.size()) {
            if (why) *why = "not a read-write data page";
            return false;
        }
        std::memcpy(page.data() + (a - base), in, n);
        ++writes;
        return true;
    }
    void setF(uint64_t a, float v) { std::memcpy(page.data() + (a - base), &v, 4); }
    float getF(uint64_t a) {
        float v;
        std::memcpy(&v, page.data() + (a - base), 4);
        return v;
    }
};

struct Rig {
    FakeMem mem;
    SnowController::Field s, f;
    Rig(float sv = -5, float fv = -20) {
        s = {mem.base + 0x38, mem.base + 0x3C};
        f = {mem.base + 0x100 + 0x38, mem.base + 0x100 + 0x3C};
        mem.setF(s.value, sv);
        mem.setF(s.def, sv);
        mem.setF(f.value, fv);
        mem.setF(f.def, fv);
    }
};

static void testConfig() {
    qs::Ini ini;
    std::vector<std::string> w;
    SnowConfig c;
    ini.parse("[General]\nEnabled=1\n");  // an old QuattroStagioni.ini without [Snow]
    c.load(ini, w);
    CHECK(w.empty());
    CHECK(c.enabled);
    CHECK(near(c.effectiveStart(), 10.0));
    CHECK(near(c.effectiveFull(), 0.0));

    SnowConfig d;
    w.clear();
    ini.parse("[Snow]\nEnabled=0\nWinterSnowStart=12\nWinterSnowFull=15\nMaxSnowStart=50\nMaxRate=abc\n");
    d.load(ini, w);
    CHECK(!d.enabled);
    CHECK(near(d.effectiveStart(), kSnowHardMaxStart));  // MaxSnowStart capped at the hard limit
    CHECK(near(d.effectiveFull(), kSnowHardMaxStart - 1));
    CHECK(near(d.maxRate, 0.5));  // malformed: default kept
    CHECK(w.size() >= 3);

    SnowConfig e;
    w.clear();
    ini.parse("[Snow]\nWinterSnowStart=8\nMaxSnowStart=4\n");
    e.load(ini, w);
    CHECK(near(e.effectiveStart(), 4.0));  // the safety cap wins over the user's value
    CHECK(w.size() == 1);

    // the template parses back to the defaults
    SnowConfig t;
    w.clear();
    ini.parse(snowIniTemplate());
    t.load(ini, w);
    CHECK(w.empty() && t.enabled && near(t.winterStart, 10) && near(t.winterFull, 0) && near(t.maxStart, 10));
}

static void testShippedIni() {
    FILE* f = std::fopen("dist/bin64/scripts/QuattroStagioni.ini", "rb");
    if (!f) {
        std::printf("  (skipping shipped QuattroStagioni.ini check: not found from cwd)\n");
        return;
    }
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    std::fclose(f);
    qs::Ini ini;
    ini.parse(text);
    SnowConfig c, d;
    std::vector<std::string> w;
    c.load(ini, w);
    CHECK(w.empty());
    CHECK(ini.has("Snow", "WinterSnowStart"));
    CHECK(c.enabled == d.enabled && c.winterStart == d.winterStart && c.winterFull == d.winterFull &&
          c.maxStart == d.maxStart && c.maxRate == d.maxRate && c.startDelayMs == d.startDelayMs);
}

static void testIntensity() {
    using season::Phase;
    const float tf = 0.4f;
    CHECK(winterIntensity(Phase::Spring, 0.5f, tf) == 0.0f);
    CHECK(winterIntensity(Phase::Summer, 1.0f, tf) == 0.0f);
    CHECK(winterIntensity(Phase::LateSummer, 0.9f, tf) == 0.0f);
    CHECK(winterIntensity(Phase::Autumn, 0.0f, tf) == 0.0f);
    CHECK(winterIntensity(Phase::Autumn, 0.79f, tf) == 0.0f);  // early A>W: no snow yet
    CHECK(near(winterIntensity(Phase::Autumn, 0.9f, tf), 0.5));
    CHECK(near(winterIntensity(Phase::Autumn, 1.0f, tf), 1.0));
    CHECK(near(winterIntensity(Phase::Winter, 0.0f, tf), 1.0));  // continuous across the phase change
    CHECK(near(winterIntensity(Phase::Winter, 0.6f, tf), 1.0));
    CHECK(near(winterIntensity(Phase::Winter, 0.7f, tf), 0.5));
    CHECK(winterIntensity(Phase::Winter, 0.8f, tf) == 0.0f);   // late W>S: no snow
    CHECK(winterIntensity(Phase::Winter, 1.0f, tf) == 0.0f);
    // monotonic ramp in
    float prev = 0;
    bool mono = true;
    for (int i = 0; i <= 100; ++i) {
        float k = winterIntensity(Phase::Autumn, i / 100.0f, tf);
        mono &= k >= prev - 1e-6f;
        prev = k;
    }
    CHECK(mono);

    season::SeasonState s;
    season::SeasonConfig sc;
    const int r = int(season::Region::Temperate);
    s.regionActive[r] = true;
    s.regionPhase[r] = Phase::Winter;
    s.regionPhaseProgress[r] = 0.3f;
    CHECK(near(winterIntensity(s, sc), 1.0));
    s.regionActive[r] = false;
    CHECK(winterIntensity(s, sc) == 0.0f);

    SnowConfig c;
    SnowThresholds o{-5, -20};
    SnowThresholds t0 = snowTargets(o, c, 0.0f), t1 = snowTargets(o, c, 1.0f), th = snowTargets(o, c, 0.5f);
    CHECK(t0.start == -5.0f && t0.full == -20.0f);  // exactly the originals outside winter
    CHECK(near(t1.start, 10) && near(t1.full, 0));
    CHECK(near(th.start, 2.5) && near(th.full, -10));
}

static void testAttach() {
    {
        Rig g;
        SnowController c;
        CHECK(c.attach(g.s, g.f, g.mem));
        CHECK(c.status().state == SnowState::Idle);
        CHECK(near(c.status().original.start, -5) && near(c.status().original.full, -20));
    }
    {
        Rig g(-5, -20);
        g.mem.setF(g.s.value, -4);  // value differs from its default
        SnowController c;
        CHECK(!c.attach(g.s, g.f, g.mem));
        CHECK(c.status().state == SnowState::Disabled);
        CHECK(g.mem.writes == 0);
    }
    {
        Rig g(100, -20);  // implausible
        SnowController c;
        CHECK(!c.attach(g.s, g.f, g.mem));
    }
    {
        Rig g(-20, -5);  // full above start
        SnowController c;
        CHECK(!c.attach(g.s, g.f, g.mem));
    }
    {
        Rig g;
        g.mem.setF(g.s.value, NAN);
        SnowController c;
        CHECK(!c.attach(g.s, g.f, g.mem));
    }
    {
        Rig g;
        SnowController c;
        c.disable("class check failed");
        CHECK(!c.attach(g.s, g.f, g.mem));  // stays disabled
        c.tick(true, 1.0f, 1.0, g.mem);
        CHECK(g.mem.writes == 0);
    }
}

static void testRamp() {
    Rig g;
    SnowController c;
    SnowConfig cfg;  // MaxRate 0.5 C/s
    c.configure(cfg);
    CHECK(c.attach(g.s, g.f, g.mem));
    // outside winter: never written
    for (int i = 0; i < 20; ++i) c.tick(true, 0.0f, 0.25, g.mem, "Estate");
    CHECK(g.mem.writes == 0);
    CHECK(c.status().state == SnowState::Idle);
    // winter: ramps at most 0.5 C/s, never a bigger jump
    float prevS = g.mem.getF(g.s.value), prevF = g.mem.getF(g.f.value);
    bool smoothOk = true, orderOk = true;
    for (int i = 0; i < 200; ++i) {
        c.tick(true, 1.0f, 0.25, g.mem, "Inverno");
        float s = g.mem.getF(g.s.value), f = g.mem.getF(g.f.value);
        smoothOk &= std::fabs(s - prevS) <= 0.125f + 1e-4f && std::fabs(f - prevF) <= 0.125f + 1e-4f;
        orderOk &= f < s;
        prevS = s;
        prevF = f;
    }
    CHECK(smoothOk);
    CHECK(orderOk);
    CHECK(near(g.mem.getF(g.s.value), 10.0));
    CHECK(near(g.mem.getF(g.f.value), 0.0));
    CHECK(c.status().state == SnowState::Active);
    // the log is throttled: far fewer lines than ticks
    std::vector<std::string> lines = c.takeLog();
    CHECK(lines.size() >= 2 && lines.size() <= 6);
    // ramp out with the season (intensity back to 0): back to the exact originals
    for (int i = 0; i < 200; ++i) c.tick(true, 0.0f, 0.25, g.mem, "Primavera");
    CHECK(g.mem.getF(g.s.value) == -5.0f);
    CHECK(g.mem.getF(g.f.value) == -20.0f);
    CHECK(c.status().state == SnowState::Idle);
    int w = g.mem.writes;
    c.tick(true, 0.0f, 0.25, g.mem);
    CHECK(g.mem.writes == w);  // nothing written once back at the originals
}

static void testOffAndRestore() {
    Rig g;
    SnowController c;
    c.configure(SnowConfig());
    CHECK(c.attach(g.s, g.f, g.mem));
    for (int i = 0; i < 100; ++i) c.tick(true, 1.0f, 0.25, g.mem);
    CHECK(g.mem.getF(g.s.value) > -5.0f);
    // menu switch off: originals at once
    c.tick(false, 1.0f, 0.25, g.mem);
    CHECK(g.mem.getF(g.s.value) == -5.0f && g.mem.getF(g.f.value) == -20.0f);
    CHECK(c.status().state == SnowState::Idle);
    // on again, then plugin stop
    for (int i = 0; i < 10; ++i) c.tick(true, 1.0f, 0.25, g.mem);
    CHECK(g.mem.getF(g.s.value) > -5.0f);
    CHECK(c.restore(g.mem));
    CHECK(g.mem.getF(g.s.value) == -5.0f && g.mem.getF(g.f.value) == -20.0f);
    int w = g.mem.writes;
    CHECK(c.restore(g.mem));  // nothing to restore twice
    CHECK(g.mem.writes == w);
}

static void testForeign() {
    {
        // the game resets the field to vanilla (e.g. a load): we keep going
        Rig g;
        SnowController c;
        c.configure(SnowConfig());
        CHECK(c.attach(g.s, g.f, g.mem));
        for (int i = 0; i < 10; ++i) c.tick(true, 1.0f, 0.25, g.mem);
        g.mem.setF(g.s.value, -5.0f);
        g.mem.setF(g.f.value, -20.0f);
        for (int i = 0; i < 5; ++i) c.tick(true, 1.0f, 0.25, g.mem);
        CHECK(c.status().state == SnowState::Active);
        CHECK(g.mem.getF(g.s.value) > -5.0f);
    }
    {
        // someone else writes another value: after 3 ticks the applier gives up and restores
        Rig g;
        SnowController c;
        c.configure(SnowConfig());
        CHECK(c.attach(g.s, g.f, g.mem));
        for (int i = 0; i < 10; ++i) c.tick(true, 1.0f, 0.25, g.mem);
        g.mem.setF(g.s.value, 7.0f);
        c.tick(true, 1.0f, 0.25, g.mem);
        c.tick(true, 1.0f, 0.25, g.mem);
        CHECK(c.status().state == SnowState::Active);
        c.tick(true, 1.0f, 0.25, g.mem);
        CHECK(c.status().state == SnowState::Disabled);
        CHECK(g.mem.getF(g.s.value) == -5.0f && g.mem.getF(g.f.value) == -20.0f);
        int w = g.mem.writes;
        c.tick(true, 1.0f, 0.25, g.mem);
        CHECK(g.mem.writes == w);
    }
    {
        // write refused (page not writable): disabled, nothing half written stays
        Rig g;
        SnowController c;
        c.configure(SnowConfig());
        CHECK(c.attach(g.s, g.f, g.mem));
        g.mem.writable = false;
        c.tick(true, 1.0f, 0.25, g.mem);
        CHECK(c.status().state == SnowState::Disabled);
        CHECK(g.mem.getF(g.s.value) == -5.0f);
    }
}

int main(int, char**) {
    testConfig();
    testShippedIni();
    testIntensity();
    testAttach();
    testRamp();
    testOffAndRestore();
    testForeign();
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
