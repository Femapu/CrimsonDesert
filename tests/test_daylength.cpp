// QuattroStagioni - unit tests of the seasonal day length (src/core/apply/daylength.h).
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "../src/core/apply/daylength.h"

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

// Continuity and monotony (modulo 24) of remapHour over a whole day.
static bool smoothDay(const SunTimes& s, const SunTimes& g, int* wraps) {
    const int N = 24 * 240;
    float prev = remapHour(0.0f, s, g);
    *wraps = 0;
    for (int i = 1; i <= N; ++i) {
        const float h = 24.0f * float(i) / float(N);
        const float v = remapHour(h, s, g);
        float d = v - prev;
        if (d < -12.0f) {
            d += 24.0f;
            ++*wraps;
        }
        if (d < -1e-4f || d > 0.05f) return false;  // increasing, no jump (step 0.1 h max slope < 0.5)
        prev = v;
    }
    return true;
}

int main() {
    DayLengthConfig c;
    const SunTimes g = gameSunTimes(c);
    CHECK(near(g.sunrise, 5.5) && near(g.sunset, 19.5));

    // ---- summer: identity
    const SunTimes su = seasonalSunTimes(c, Phase::Summer, 0.3f, 0.4f);
    CHECK(near(su.sunrise, 5.5) && near(su.sunset, 19.5));
    for (float h = 0; h < 24.0f; h += 0.37f) CHECK(near(remapHour(h, su, g), h, 1e-4));

    // ---- winter: shorter day, later sunrise, earlier sunset
    const SunTimes wi = seasonalSunTimes(c, Phase::Winter, 0.2f, 0.4f);
    CHECK(near(wi.sunrise, 7.25) && near(wi.sunset, 17.75));
    CHECK(wi.daylight() < su.daylight());
    CHECK(near(remapHour(wi.sunrise, wi, g), g.sunrise));  // seasonal sunrise shows the game's sunrise
    CHECK(near(remapHour(wi.sunset, wi, g), g.sunset));
    CHECK(near(remapHour(12.5f, wi, g), 12.5f));           // noon stays noon (symmetric shift)
    CHECK(remapHour(7.0f, wi, g) < g.sunrise);              // 7:00 in winter: still before the game's dawn
    // the sun is up (game day) only between the seasonal sunrise and sunset
    for (float h = 0; h < 24.0f; h += 0.25f) {
        const float v = remapHour(h, wi, g);
        const bool up = v > g.sunrise && v < g.sunset;
        const bool want = h > wi.sunrise && h < wi.sunset;
        CHECK(up == want);
    }

    // ---- continuity and monotony on 24 h, one wrap at midnight
    int wraps = 0;
    CHECK(smoothDay(wi, g, &wraps) && wraps == 1);
    CHECK(smoothDay(su, g, &wraps) && wraps == 1);
    SunTimes odd{3.0f, 22.0f};
    CHECK(smoothDay(odd, g, &wraps) && wraps == 1);

    // ---- midnight wrap
    CHECK(near(remapHour(24.0f, wi, g), remapHour(0.0f, wi, g)));
    CHECK(near(remapHour(-1.0f, wi, g), remapHour(23.0f, wi, g)));
    CHECK(near(remapHour(0.5f, wi, g), 0.5f));  // symmetric shift: the middle of the night stays (0:30)
    CHECK(remapHour(23.99f, wi, g) < 1.0f);     // just before midnight the sun hour is already past it
    CHECK(near(hourDelta(23.5f, 0.5f), -1.0f) && near(hourDelta(0.5f, 23.5f), 1.0f));
    CHECK(formatHour(7.25f) == "7:15" && formatHour(24.0f) == "0:00");

    // ---- MinDaylightHours and blends
    DayLengthConfig c2 = c;
    c2.sunriseShift.v[int(Phase::Winter)] = 5;
    c2.sunsetShift.v[int(Phase::Winter)] = -5;
    const SunTimes m = seasonalSunTimes(c2, Phase::Winter, 0.0f, 0.4f);
    CHECK(near(m.daylight(), 8.0) && near((m.sunrise + m.sunset) * 0.5, 12.5));
    const SunTimes au = seasonalSunTimes(c, Phase::Autumn, 0.0f, 0.4f);
    const SunTimes au9 = seasonalSunTimes(c, Phase::Autumn, 0.9f, 0.4f);  // blending toward winter
    CHECK(near(au.sunrise, 6.5) && au9.sunrise > au.sunrise && au9.sunrise < wi.sunrise);
    season::VisualStage st;
    st.from = Phase::Autumn;
    st.to = Phase::Winter;
    st.step = 1;
    st.t = 0.5f;
    CHECK(near(seasonalSunTimes(c, st).sunrise, 6.5 + 0.375));
    // invalid input -> h unchanged
    SunTimes bad{10.0f, 9.0f};
    CHECK(near(remapHour(8.0f, bad, g), 8.0f));

    // ---- SunTable
    SunTable t(3);
    float out[4] = {};
    CHECK(!t.lookup(12.0f, out));
    CHECK(SunTable::binOf(0.0f) == 0 && SunTable::binOf(23.99f) == 95 && SunTable::binOf(24.0f) == 0 && SunTable::binOf(12.1f) == 48);
    auto fill = [&](int bin, float a) {
        float v[3] = {a, -a, 0.5f};
        for (int k = 0; k < 4; ++k) t.record((bin + 0.5f) * 0.25f, v);
    };
    fill(48, 1.0f);
    fill(49, 2.0f);
    CHECK(t.known() == 2 && t.samples(48) == 4);
    CHECK(t.lookup(12.125f, out) && near(out[0], 1.0) && near(out[1], -1.0) && near(out[2], 0.5));
    CHECK(t.lookup(12.25f, out) && near(out[0], 1.5));  // halfway between the two centres
    CHECK(!t.lookup(12.5f, out));                         // bin 50 unknown, nothing after it
    fill(52, 5.0f);                                       // gap of 2 bins (50, 51)
    CHECK(t.lookup(12.875f, out) && near(out[0], 4.0));   // centre of bin 51: 2/3 of the way from bin 49 to bin 52
    CHECK(!t.lookup(12.875f, out, 4, 1));                 // gap too big for maxGap 1
    // not enough samples
    float v1[3] = {9, 9, 9};
    t.record(3.1f, v1);
    CHECK(t.samples(12) == 1 && !t.lookup(3.125f, out) && t.lookup(3.125f, out, 1, 2) == false);
    // midnight wrap
    fill(95, 10.0f);
    fill(0, 20.0f);
    CHECK(t.lookup(0.0f, out) && near(out[0], 15.0));
    CHECK(t.lookup(23.9375f, out) && near(out[0], 12.5));
    // non finite readings ignored
    float nanv[3] = {NAN, 0, 0};
    CHECK(!t.record(5.0f, nanv) && t.samples(20) == 0);
    // running mean
    SunTable t1(1);
    float a = 1, b = 3;
    t1.record(6.0f, &a);
    t1.record(6.0f, &b);
    CHECK(t1.lookup(6.125f, out, 1, 0) == false);  // one bin alone: no neighbour to interpolate with
    t1.record(6.3f, &a);
    CHECK(t1.lookup(6.125f, out, 1, 0) && near(out[0], 2.0));

    // round trip
    const std::string txt = t.serialize(0x6AE46DD0u, "atmosphere:0x1A0:3 sundir");
    CHECK(txt.find("Exe=0x6AE46DD0") != std::string::npos);
    SunTable r(1);
    std::string err;
    CHECK(r.parse(txt, 0x6AE46DD0u, "atmosphere:0x1A0:3 sundir", &err));
    CHECK(r.count() == 3 && r.known() == t.known() && r.samples(12) == 1);
    CHECK(r.lookup(12.875f, out) && near(out[0], 4.0) && near(out[1], -4.0));
    CHECK(!r.parse(txt, 0x12345678u, "atmosphere:0x1A0:3 sundir", &err) && !err.empty());
    CHECK(!r.parse(txt, 0x6AE46DD0u, "atmosphere:0x1B0:3 sundir", &err));
    CHECK(!r.parse("[SunTable]\nVersion=1\nExe=0x6AE46DD0\nField=f\nCount=2\n[Bins]\nB01=4 | 1\n", 0x6AE46DD0u, "f", &err));
    CHECK(r.known() == t.known());  // failed parses keep the table
    CHECK(!r.parse("", 0x6AE46DD0u, "f"));

    // ---- [DayLength] ini
    Ini ini;
    ini.parse("[DayLength]\nEnabled=0\nGameSunrise=6\nGameSunset=20\nSunriseShift=1,0,0,1,2\nSunsetShift=-1 0 0 -1 x\n"
              "MinDaylightHours=pippo\nMaxGapBins=3\n");
    DayLengthConfig lc;
    std::vector<std::string> warn;
    lc.load(ini, warn);
    CHECK(!lc.enabled && near(lc.gameSunrise, 6) && near(lc.gameSunset, 20));
    CHECK(near(lc.sunriseShift[Phase::Winter], 2) && near(lc.sunsetShift[Phase::Winter], -1.75));  // malformed: default kept
    CHECK(near(lc.minDaylightHours, 8) && lc.maxGapBins == 3);
    CHECK(warn.size() == 2);
    // the template parses back to the defaults, without warnings
    Ini tini;
    tini.parse(dayLengthIniTemplate());
    DayLengthConfig tc;
    tc.enabled = false;
    warn.clear();
    tc.load(tini, warn);
    CHECK(warn.empty() && tc.enabled && near(tc.sunriseShift[Phase::Winter], 1.75) && near(tc.gameSunset, 19.5));
    CHECK(dayLengthIniTemplate().find("\xE2\x80\x94") == std::string::npos);  // no em dash

    // ---- [daylength] of targets.ini
    DayLengthTarget dt;
    warn.clear();
    Ini ti;
    ti.parse("[daylength]\nEnabled=1\nField=atmosphere:0x1A0:3\nKind=sundir\n");
    dt.load(ti, warn);
    CHECK(dt.enabled && dt.field.valid && dt.field.offset == 0x1A0 && dt.field.count == 3 && dt.kind == SunKind::SunDir && warn.empty());
    DayLengthTarget dh;
    Ini th;
    th.parse("[DayLength]\nEnabled=1\nField=manager:0x200:2\nKind=Hour\n");
    dh.load(th, warn);
    CHECK(dh.kind == SunKind::Hour && dh.field.count == 1 && warn.size() == 1);
    DayLengthTarget dn;
    Ini tn;
    tn.parse("[daylength]\nEnabled=1\nField=sky:0x10\nKind=moon\n");
    warn.clear();
    dn.load(tn, warn);
    CHECK(!dn.enabled && !dn.field.valid && warn.size() == 2);
    DayLengthTarget de;
    Ini te;
    te.parse("[general]\nEnabled=1\n");
    de.load(te, warn);
    CHECK(!de.enabled && !de.field.valid);

    std::printf("test_daylength: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
