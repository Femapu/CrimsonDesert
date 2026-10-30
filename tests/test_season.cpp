// QuattroStagioni - unit tests for the season engine (calendar, stages, weather, snow, light, ini).
// Usage: test_season [data_dir]   (data_dir unused: the engine needs no game files)
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "../src/core/ini.h"
#include "../src/core/season/season.h"

using namespace qs::season;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond)                                                                         \
    do {                                                                                    \
        if (cond) ++g_pass;                                                                 \
        else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); }       \
    } while (0)
#define NEAR(a, b, eps) CHECK(std::fabs(double(a) - double(b)) <= (eps))

static SeasonConfig defaults() {
    SeasonConfig c;
    c.setDefaults();
    return c;
}

static int stageIndex(const std::vector<VisualStage>& all, const VisualStage& s) {
    for (size_t i = 0; i < all.size(); ++i)
        if (all[i] == s) return int(i);
    return -1;
}

static bool hasError(const std::vector<std::string>& errs, const std::string& needle) {
    for (const auto& e : errs)
        if (e.find(needle) != std::string::npos) return true;
    return false;
}

// ---------------------------------------------------------------------------------------------
static void testNames() {
    CHECK(std::string(toString(Phase::LateSummer)) == "LateSummer");
    CHECK(toChar(Phase::Autumn) == 'A');
    CHECK(std::string(phaseNameIt(Phase::Winter)) == "Inverno");
    CHECK(regionFromHint("shared_temperate") == Region::Temperate);
    CHECK(regionFromHint("north_cold") == Region::NorthCold);
    CHECK(regionFromHint("swamp_jungle") == Region::SwampJungle);
    CHECK(regionFromHint("aquatic") == Region::Aquatic);
    CHECK(regionFromHint("desert_dry") == Region::DesertDry);
    CHECK(regionFromHint("abyss") == Region::Abyss);
    CHECK(regionFromHint("something_else") == Region::Temperate);
    Region r;
    CHECK(regionFromName("NORTHCOLD", r) && r == Region::NorthCold);
    CHECK(!regionFromName("Mars", r));
}

// ---------------------------------------------------------------------------------------------
static void testCalendar() {
    SeasonConfig c = defaults();
    NEAR(c.yearLength(), 38, 1e-6);
    Engine e;
    e.configure(c);
    // Boundaries: day 8.0 starts Summer, a hair before is still Spring.
    e.update(7.999, 0);
    CHECK(e.state().phase == Phase::Spring);
    e.update(8.0, 0);
    CHECK(e.state().phase == Phase::Summer);
    NEAR(e.state().phaseProgress, 0, 1e-6);
    e.update(12.0, 0);
    NEAR(e.state().phaseProgress, 0.5, 1e-6);
    e.update(29.0, 0);
    CHECK(e.state().phase == Phase::Winter);
    e.update(37.99, 0);
    CHECK(e.state().phase == Phase::Winter);
    // Wrap-around: year 2 and negative days.
    e.update(38.0, 0);
    CHECK(e.state().phase == Phase::Spring);
    NEAR(e.state().dayOfYear, 0, 1e-6);
    e.update(38.0 * 5 + 22.0, 0);
    CHECK(e.state().phase == Phase::Autumn);
    NEAR(e.state().dayOfYear, 22, 1e-4);
    e.update(-1.0, 0);
    CHECK(e.state().phase == Phase::Winter);
    NEAR(e.state().dayOfYear, 37, 1e-6);
    CHECK(c.yearIndex(-1.0) == -1);
    CHECK(c.yearIndex(76.5) == 2);
    NEAR(c.dayOfYear(76.5), 0.5, 1e-9);

    // Phase starts and lengths.
    NEAR(c.phaseStartDay(Phase::Spring), 0, 1e-6);
    NEAR(c.phaseStartDay(Phase::Autumn), 21, 1e-6);
    NEAR(c.phaseStartDay(Phase::Winter), 29, 1e-6);
    NEAR(c.phaseLength(Phase::LateSummer), 5, 1e-6);

    // Labels and the manual jump.
    CHECK(c.dayToLabel(23.5) == "Autunno, giorno 3 di 8");
    CHECK(c.dayToLabel(0.0) == "Primavera, giorno 1 di 8");
    CHECK(c.dayToLabel(37.9) == "Inverno, giorno 9 di 9");
    CHECK(c.dayToLabel(38.0 + 8.2) == "Estate, giorno 1 di 8");
    NEAR(c.jumpToPhase(38.0 * 3 + 5.0, Phase::Autumn), 38.0 * 3 + 21.0, 1e-6);
    NEAR(c.jumpToPhase(2.0, Phase::Winter), 29.0, 1e-6);

    // Day sources.
    SeasonConfig p = c;
    p.daySource = DaySource::Playtime;
    p.realMinutesPerDay = 48;
    NEAR(p.advance(1.0, 48 * 60, 999), 2.0, 1e-9);
    p.daySource = DaySource::GameClock;
    NEAR(p.advance(1.0, 999, 12), 1.5, 1e-9);
    p.daySource = DaySource::Manual;
    NEAR(p.advance(1.0, 999, 999), 1.0, 1e-9);
}

// ---------------------------------------------------------------------------------------------
static void testStages() {
    SeasonConfig c = defaults();
    std::vector<VisualStage> all = allStages(c);
    CHECK(int(all.size()) == 20);
    CHECK(c.stageCount() == 20);
    std::set<std::string> ids;
    for (const auto& s : all) ids.insert(s.id());
    CHECK(ids.size() == all.size());
    CHECK(all[0].id() == "S");
    CHECK(all[1].id() == "S>U.1");
    CHECK(all[4].id() == "U");
    CHECK(all[4].isVanilla());
    CHECK(all.back().id() == "W>S.3");
    NEAR(all[1].t, 0.25, 1e-6);
    NEAR(all[3].t, 0.75, 1e-6);
    int vanilla = 0;
    for (const auto& s : all) vanilla += s.isVanilla();
    CHECK(vanilla == 1);  // 19 non-vanilla sets to generate with defaults

    // nextStage walks allStages in order and wraps.
    for (size_t i = 0; i < all.size(); ++i) CHECK(nextStage(c, all[i]) == all[(i + 1) % all.size()]);

    // Ids parse back.
    for (const auto& s : all) {
        VisualStage p;
        CHECK(parseStageId(s.id(), c.transitionSteps, p) && p == s);
        NEAR(p.t, s.t, 1e-6);
    }
    VisualStage bad;
    CHECK(!parseStageId("A>S.1", 3, bad));  // not adjacent
    CHECK(!parseStageId("A>W.4", 3, bad));  // beyond the steps
    CHECK(!parseStageId("X", 3, bad));
    CHECK(!parseStageId("", 3, bad));

    // Stage sequence over a year: monotonic in calendar order (one step at a time), every stage seen.
    for (int r = 0; r < kRegionCount; ++r) {
        if (!c.regions[r].enabled) continue;
        std::set<int> seen;
        int prev = stageIndex(all, stageAt(c, Region(r), 0.0));
        CHECK(prev >= 0);
        seen.insert(prev);
        bool ok = true;
        for (double d = 0.01; d < 38.0 * 2; d += 0.01) {
            int cur = stageIndex(all, stageAt(c, Region(r), d));
            if (cur < 0) ok = false;
            else if (cur != prev) {
                if (cur != (prev + 1) % int(all.size())) ok = false;
                prev = cur;
            }
            seen.insert(cur);
        }
        CHECK(ok);
        CHECK(seen.size() == all.size());
    }

    // Concrete positions in the Temperate calendar (plateau 60%, then 3 stages over the tail).
    CHECK(stageAt(c, Region::Temperate, 21.0).id() == "A");
    CHECK(stageAt(c, Region::Temperate, 21.0 + 4.7).id() == "A");
    CHECK(stageAt(c, Region::Temperate, 21.0 + 4.9).id() == "A>W.1");
    CHECK(stageAt(c, Region::Temperate, 21.0 + 6.0).id() == "A>W.2");
    CHECK(stageAt(c, Region::Temperate, 21.0 + 7.9).id() == "A>W.3");
    CHECK(stageAt(c, Region::Temperate, 29.0).id() == "W");
    CHECK(stageAt(c, Region::Temperate, 10.0).isVanilla());

    // Disabled / excluded regions are always the Summer plateau.
    for (double d = 0; d < 38; d += 0.5) {
        CHECK(stageAt(c, Region::DesertDry, d).isVanilla());
        CHECK(stageAt(c, Region::Abyss, d, 12345u).isVanilla());
    }

    // nextStageChangeDay agrees with stageAt.
    double d0 = 22.0;
    double ch = c.nextStageChangeDay(d0, Region::Temperate);
    NEAR(ch, 21.0 + 8.0 * 0.6, 1e-6);
    CHECK(stageAt(c, Region::Temperate, ch - 1e-6) != stageAt(c, Region::Temperate, ch + 1e-6));
    CHECK(std::isinf(c.nextStageChangeDay(d0, Region::DesertDry)));

    // Hard switches.
    SeasonConfig h = c;
    h.transitionSteps = 0;
    CHECK(h.stageCount() == 5);
    CHECK(allStages(h).size() == 5);
    CHECK(stageAt(h, Region::Temperate, 28.9).id() == "A");
    CHECK(nextStage(h, allStages(h)[3]).id() == "W");
    SeasonConfig z = c;
    z.transitionFraction = 0;
    CHECK(z.stageCount() == 5);
    CHECK(stageAt(z, Region::Temperate, 28.9).id() == "A");
}

// ---------------------------------------------------------------------------------------------
static void testJitter() {
    SeasonConfig c = defaults();
    float lo = 1e9f, hi = -1e9f;
    double sum = 0;
    const int N = 20000;
    for (uint32_t s = 0; s < uint32_t(N); ++s) {
        float j = assetJitter(c, s);
        CHECK(j >= -c.assetJitterDays && j <= c.assetJitterDays);
        CHECK(j == assetJitter(c, s));  // deterministic
        lo = std::min(lo, j), hi = std::max(hi, j);
        sum += j;
    }
    CHECK(lo < -1.9f && hi > 1.9f);           // uses the whole range
    NEAR(sum / N, 0.0, 0.05);                 // centred
    CHECK(assetJitter(c, 0xC5EDEu) != assetJitter(c, 0xC5EDFu));
    // Known value: guards against accidental changes of the hash (would reshuffle every forest).
    NEAR(assetJitter(c, 1), 1.811051, 1e-5);

    // A jittered asset is the unjittered stage of a shifted day; max lag/lead = jitter days.
    std::vector<VisualStage> all = allStages(c);
    int early = 0, late = 0;
    for (uint32_t s = 0; s < 2000; ++s) {
        float j = assetJitter(c, s);
        VisualStage a = stageAt(c, Region::Temperate, 26.0, s);
        CHECK(a == stageAt(c, Region::Temperate, 26.0 + j));
        int ia = stageIndex(all, a), ib = stageIndex(all, stageAt(c, Region::Temperate, 26.0));
        early += ia > ib;
        late += ia < ib;
    }
    CHECK(early > 100 && late > 100);  // the forest does not turn all at once
    SeasonConfig nj = c;
    nj.assetJitterDays = 0;
    for (uint32_t s = 0; s < 100; ++s) CHECK(stageAt(nj, Region::Temperate, 25.0, s) == stageAt(nj, Region::Temperate, 25.0));

    // Engine::stageFor uses the engine day.
    Engine e;
    e.configure(c);
    e.update(25.0, 0);
    CHECK(e.stageFor(Region::Temperate, 77u) == stageAt(c, Region::Temperate, 25.0, 77u));
    CHECK(e.allStages().size() == 20);
}

// ---------------------------------------------------------------------------------------------
static void testRegionOffsets() {
    SeasonConfig c = defaults();
    // North: offset -3 and winter x1.5 renormalised -> winter starts earlier and lasts longer.
    float wn = c.phaseStartDay(Phase::Winter, Region::NorthCold);
    float wt = c.phaseStartDay(Phase::Winter, Region::Temperate);
    CHECK(wn < wt);
    CHECK(c.phaseLength(Phase::Winter, Region::NorthCold) > c.phaseLength(Phase::Winter, Region::Temperate) * 1.3f);
    CHECK(c.phaseLength(Phase::Summer, Region::NorthCold) < c.phaseLength(Phase::Summer, Region::Temperate));
    double year = 0;
    for (int i = 0; i < kPhaseCount; ++i) year += c.phaseLength(Phase(i), Region::NorthCold);
    NEAR(year, 38, 1e-4);  // regions never drift apart
    Engine e;
    e.configure(c);
    e.update(wn + 0.01, 0);
    CHECK(e.state().regionPhase[int(Region::NorthCold)] == Phase::Winter);
    CHECK(e.state().regionPhase[int(Region::Temperate)] == Phase::Autumn);
    // Pure offset: shifts by exactly that many days.
    SeasonConfig o = c;
    o.regions[int(Region::Aquatic)].dayOffset = 2.5f;
    for (double d = 0; d < 38; d += 0.37)
        CHECK(stageAt(o, Region::Aquatic, d + 2.5) == stageAt(o, Region::Temperate, d));
    NEAR(o.phaseStartDay(Phase::Spring, Region::Aquatic), 2.5, 1e-6);
    o.regions[int(Region::Aquatic)].dayOffset = -2.5f;
    NEAR(o.phaseStartDay(Phase::Spring, Region::Aquatic), 35.5, 1e-6);
    NEAR(o.jumpToPhase(40.0, Phase::Spring, Region::Aquatic), 38.0 + 35.5, 1e-6);
}

// ---------------------------------------------------------------------------------------------
static void testTemperatureWeather() {
    SeasonConfig c = defaults();
    Engine e;
    e.configure(c);
    const int T = int(Region::Temperate), N = int(Region::NorthCold), S = int(Region::SwampJungle);
    e.update(33.5, 0, 12.0f);  // mid-winter
    float winterT = e.state().dailyMeanTempC[T];
    NEAR(winterT, -4.0, 1e-3);
    CHECK(e.state().dailyMeanTempC[N] < winterT - 3);
    CHECK(e.state().dailyMeanTempC[S] > 10);
    CHECK(e.state().weather[T].snowy > 0.25f);
    CHECK(e.state().weather[N].snowy > 0.25f);
    CHECK(e.state().weather[S].snowy == 0.0f);
    CHECK(e.state().breathVisible[T]);
    e.update(33.5, 0, 3.0f);
    float night = e.state().temperatureC[T];
    e.update(33.5, 0, 15.0f);
    float noon = e.state().temperatureC[T];
    NEAR(noon - night, 2 * c.regions[T].diurnalTempC, 1e-3);
    // The two-argument update takes the hour from the day fraction.
    e.update(33.5 + 15.0 / 24.0 - 0.5, 0);
    NEAR(e.state().hourOfDay, 15.0, 1e-3);

    e.update(12.0, 0, 12.0f);  // summer
    CHECK(e.state().dailyMeanTempC[T] > 20);
    CHECK(e.state().weather[T].snowy == 0.0f);
    CHECK(e.state().weather[T].sunny > 0.6f);
    CHECK(e.state().weather[N].snowy == 0.0f);
    CHECK(!e.state().breathVisible[T]);
    float summerSunny = e.state().weather[T].sunny;
    e.update(24.0, 0, 12.0f);  // autumn plateau
    const WeatherWeights& a = e.state().weather[T];
    CHECK(a.rainy > 0.25f && a.windy > 0.1f && a.sunny < summerSunny);
    CHECK(e.state().weather[S].rainy + e.state().weather[S].storm > a.rainy + a.storm);  // swamp is wetter
    CHECK(e.state().weather[int(Region::DesertDry)].snowy == 0.0f);

    // Weights always sum to 1 and are non-negative.
    bool ok = true;
    for (double d = 0; d < 38; d += 0.25) {
        e.update(d, 0);
        for (int r = 0; r < kRegionCount; ++r) {
            const WeatherWeights& w = e.state().weather[r];
            double s = double(w.sunny) + w.cloudy + w.rainy + w.storm + w.snowy + w.windy;
            if (std::fabs(s - 1.0) > 1e-5) ok = false;
            if (w.sunny < 0 || w.cloudy < 0 || w.rainy < 0 || w.storm < 0 || w.snowy < 0 || w.windy < 0) ok = false;
        }
    }
    CHECK(ok);
}

// ---------------------------------------------------------------------------------------------
static void testSnow() {
    SeasonConfig c = defaults();
    Engine e;
    e.configure(c);
    const int T = int(Region::Temperate), N = int(Region::NorthCold);
    // Walk a year and a half hour by hour from mid-summer.
    double maxWinter = 0, endSpring = 1, summer = 0, northWinter = 0;
    for (double d = 12.0; d < 12.0 + 38.0 * 1.5; d += 1.0 / 24.0) {
        e.update(d, 1.0f);
        double doy = c.dayOfYear(d);
        float s = e.state().snowCover[T];
        if (doy > 33 && doy < 37) maxWinter = std::max(maxWinter, double(s));
        if (doy > 7.5 && doy < 8.0) endSpring = std::min(endSpring, double(s));
        if (doy > 8 && doy < 21) summer = std::max(summer, double(s));
        if (doy > 33 && doy < 37) northWinter = std::max(northWinter, double(e.state().snowCover[N]));
        CHECK(e.state().snowCover[int(Region::DesertDry)] == 0.0f);
        CHECK(e.state().snowCover[int(Region::SwampJungle)] == 0.0f);
    }
    CHECK(maxWinter > 0.6);  // accumulates in winter
    CHECK(endSpring < 0.01);  // melted by the end of spring
    CHECK(summer == 0.0);
    CHECK(northWinter > 0.9);

    // A jump re-simulates: same result as walking there (deterministic, path independent).
    Engine j;
    j.configure(c);
    j.update(38.0 + 34.0, 1.0f);
    float jumped = j.state().snowCover[T];
    Engine w;
    w.configure(c);
    w.update(38.0 + 34.0 - 3.0, 1.0f);
    for (double d = 38.0 + 34.0 - 3.0; d < 38.0 + 34.0 - 1e-9;) {
        d += 1.0 / 24.0;
        w.update(d, 1.0f);
    }
    NEAR(jumped, w.state().snowCover[T], 0.05);
    CHECK(jumped > 0.5f);

    // Derived hours (inGameHours < 0) equal explicit hours.
    Engine a, b;
    a.configure(c);
    b.configure(c);
    a.update(30.0, 0);
    b.update(30.0, 0);
    for (double d = 30.0; d < 32.0;) {
        d += 0.1;
        a.update(d, -1);
        b.update(d, 2.4f);
    }
    NEAR(a.state().snowCover[T], b.state().snowCover[T], 1e-6);

    // Disabled accumulation.
    SeasonConfig off = c;
    off.snowAccumulation = false;
    Engine o;
    o.configure(off);
    o.update(34.0, 1.0f);
    CHECK(o.state().snowCover[T] == 0.0f);
}

// ---------------------------------------------------------------------------------------------
static void testLightAndParticles() {
    SeasonConfig c = defaults();
    Engine e;
    e.configure(c);
    const int T = int(Region::Temperate);
    // Summer plateau is vanilla.
    for (double d = 8.0; d < 12.7; d += 0.2) {
        e.update(d, 0);
        const SeasonState& s = e.state();
        CHECK(s.stage[T].isVanilla());
        CHECK(s.lightTint[0] == 1.0f && s.lightTint[1] == 1.0f && s.lightTint[2] == 1.0f);
        CHECK(s.saturation == 1.0f);
        NEAR(s.sunElevationOffsetDeg, 0, 1e-6);
        NEAR(s.sunriseHour, c.vanillaSunriseHour, 1e-6);
        NEAR(s.sunsetHour, c.vanillaSunsetHour, 1e-6);
        CHECK(s.fallingLeaves == 0.0f);
    }
    e.update(12.0, 0);
    float sunriseSummer = e.state().sunriseHour;
    e.update(33.5, 0);
    const SeasonState& w = e.state();
    CHECK(sunriseSummer < w.sunriseHour);  // sunrise earlier in summer
    CHECK(w.sunsetHour < c.vanillaSunsetHour);
    CHECK(w.dayLengthHours < 10 && w.dayLengthHours >= 4);
    NEAR(w.sunElevationOffsetDeg, -2 * 23.44, 1e-3);
    CHECK(w.lightTint[2] > w.lightTint[0]);  // cooler
    CHECK(w.saturation < 0.9f);              // desaturated
    e.update(24.0, 0);
    CHECK(e.state().lightTint[0] > e.state().lightTint[2]);  // warmer autumn

    // Day length is monotonic from mid-summer to mid-winter.
    float prev = 0;
    bool mono = true;
    for (double d = 12.0; d <= 33.5; d += 0.25) {
        e.update(d, 0);
        if (e.state().sunriseHour < prev - 1e-5f) mono = false;
        prev = e.state().sunriseHour;
    }
    CHECK(mono);
    // Equator: no day length change.
    SeasonConfig eq = c;
    eq.virtualLatitudeDeg = 0;
    Engine q;
    q.configure(eq);
    q.update(33.5, 0);
    NEAR(q.state().dayLengthHours, 16, 1e-4);
    NEAR(q.state().dayScale, 1, 1e-5);
    NEAR(q.state().nightScale, 1, 1e-5);
    // Controls off.
    SeasonConfig off = c;
    off.lightControl = off.dayLengthControl = off.particles = false;
    Engine o;
    o.configure(off);
    o.update(26.0, 0);
    CHECK(o.state().lightTint[0] == 1.0f && o.state().saturation == 1.0f);
    CHECK(o.state().sunriseHour == c.vanillaSunriseHour);
    CHECK(o.state().fallingLeaves == 0.0f);
    CHECK(!o.state().breathVisible[T]);

    // Falling leaves: rise through autumn, peak in the A>W tail, gone mid-winter.
    float lvA0 = 0, peak = 0, peakDay = 0;
    for (double d = 16.0; d < 38.0; d += 0.05) {
        e.update(d, 0);
        float lv = e.state().fallingLeaves;
        CHECK(lv >= 0 && lv <= 1);
        if (lv > peak) peak = lv, peakDay = float(d);
        if (std::fabs(d - 21.5) < 0.026) lvA0 = lv;
    }
    CHECK(peak > 0.95f);
    CHECK(peakDay > 21.0f + 8 * 0.6f && peakDay < 29.0f);
    CHECK(lvA0 > 0.2f && lvA0 < 0.5f);
    e.update(35.0, 0);
    CHECK(e.state().fallingLeaves == 0.0f);
    CHECK(e.state().leaves[int(Region::SwampJungle)] <= 0.2f);
}

// ---------------------------------------------------------------------------------------------
static void testIni() {
    SeasonConfig c = defaults();
    std::string text = c.save();
    CHECK(text == defaultIniText());
    CHECK(text.find("[Seasons]") != std::string::npos);
    CHECK(text.find("[Region.NorthCold]") != std::string::npos);
    CHECK(text.find("\xe2\x80\x94") == std::string::npos);  // no em dash in the user template

    // Round trip of defaults.
    qs::Ini ini;
    ini.parse(text);
    SeasonConfig l;
    std::vector<std::string> errs;
    CHECK(l.load(ini, errs));
    CHECK(errs.empty());
    for (const auto& er : errs) std::printf("  unexpected: %s\n", er.c_str());
    CHECK(l.save() == text);
    CHECK(!l.regions[int(Region::DesertDry)].enabled && !l.regions[int(Region::Abyss)].enabled);
    NEAR(l.regions[int(Region::NorthCold)].phaseScale[4], 1.5, 1e-6);

    // Round trip of a modified config.
    SeasonConfig m = c;
    m.phaseDays[3] = 12.5f;
    m.transitionSteps = 5;
    m.transitionFraction = 0.25f;
    m.daySource = DaySource::Manual;
    m.particles = false;
    m.regions[int(Region::Aquatic)].enabled = false;
    m.regions[int(Region::NorthCold)].dayOffset = -4.25f;
    m.regions[int(Region::SwampJungle)].humidity = 0.9f;
    ini.parse(m.save());
    errs.clear();
    CHECK(l.load(ini, errs));
    CHECK(l.save() == m.save());
    NEAR(l.phaseDays[3], 12.5, 1e-6);
    CHECK(l.transitionSteps == 5 && l.daySource == DaySource::Manual && !l.particles);
    CHECK(!l.regions[int(Region::Aquatic)].enabled);
    NEAR(l.regions[int(Region::NorthCold)].dayOffset, -4.25, 1e-6);

    // Errors: out of range (clamped), malformed (default kept), unknown region, bad enum/bool, comma.
    ini.parse(
        "[General]\nEnabled=1\n"
        "[Seasons]\nSpringDays = 500\nSummerDays = abc\nAutumnDays = 6,5\nTransitionSteps = 12\n"
        "TransitionFraction=-1\nDaySource = sundial\nParticles = maybe\nWeatherControl = no\n"
        "AssetJitterDays = 1.5 ; inline comment\n"
        "[Region.Mars]\nEnabled=1\n"
        "[Region.North]\nWinterScale = 0\nEnabled = 0\n"
        "[region.desertdry]\nEnabled = 1\nHumidity = 2\n");
    errs.clear();
    CHECK(!l.load(ini, errs));
    for (const auto& er : errs) std::printf("  reported: %s\n", er.c_str());
    NEAR(l.phaseDays[0], 120, 1e-6);
    CHECK(hasError(errs, "SpringDays = 500 is out of range"));
    NEAR(l.phaseDays[1], 8, 1e-6);
    CHECK(hasError(errs, "SummerDays = 'abc' is not a number"));
    NEAR(l.phaseDays[3], 6.5, 1e-6);
    CHECK(!hasError(errs, "AutumnDays"));
    CHECK(l.transitionSteps == 8);
    CHECK(hasError(errs, "TransitionSteps = 12"));
    NEAR(l.transitionFraction, 0, 1e-6);
    CHECK(hasError(errs, "TransitionFraction"));
    CHECK(l.daySource == DaySource::Playtime);
    CHECK(hasError(errs, "DaySource = 'sundial'"));
    CHECK(l.particles);
    CHECK(hasError(errs, "Particles = 'maybe'"));
    CHECK(!l.weatherControl);
    NEAR(l.assetJitterDays, 1.5, 1e-6);
    CHECK(hasError(errs, "[region.mars] unknown region"));
    NEAR(l.regions[int(Region::NorthCold)].phaseScale[4], 0.1, 1e-6);  // alias section, clamped
    CHECK(hasError(errs, "[Region.NorthCold] WinterScale = 0 is out of range"));
    CHECK(!l.regions[int(Region::NorthCold)].enabled);
    CHECK(l.regions[int(Region::DesertDry)].enabled);  // user may opt in
    NEAR(l.regions[int(Region::DesertDry)].humidity, 1, 1e-6);
    CHECK(errs.size() == 9);

    // Missing keys keep defaults; empty ini is valid.
    ini.parse("");
    errs.clear();
    CHECK(l.load(ini, errs));
    CHECK(l.save() == text);

    // clamp() after menu edits.
    SeasonConfig k = c;
    k.virtualLatitudeDeg = 90;
    k.transitionSteps = -2;
    k.phaseDays[2] = std::nanf("");
    std::vector<std::string> ce;
    k.clamp(&ce);
    CHECK(ce.size() == 3);
    NEAR(k.virtualLatitudeDeg, 65, 1e-6);
    CHECK(k.transitionSteps == 0);
    NEAR(k.phaseDays[2], 0.5, 1e-6);
}

// ---------------------------------------------------------------------------------------------
static void testColdFloorAndSleep() {
    SeasonConfig c = defaults();
    c.regions[int(Region::NorthCold)].meanTempC = -35;
    c.regions[int(Region::NorthCold)].ampTempC = 20;
    Engine e;
    e.configure(c);
    for (double d = 0; d < 38; d += 0.1) {
        e.update(d, 0);
        for (int r = 0; r < kRegionCount; ++r) CHECK(e.state().temperatureC[r] >= kMinTempC);
    }
    // Sleeping in steps of 3..72 game hours stays bounded and close to the hour-by-hour walk.
    SeasonConfig d = defaults();
    d.daySource = DaySource::GameClock;
    Engine slow, fast;
    slow.configure(d);
    fast.configure(d);
    double t0 = 30.0;
    slow.update(t0, 0);
    fast.update(t0, 0);
    const float steps[] = {3, 6, 12, 72, 6, 12, 3};
    double day = t0;
    for (float h : steps) {
        double end = day + h / 24.0;
        for (double x = day; x < end - 1e-9;) {
            x = std::min(end, x + 1.0 / 24.0);
            slow.update(x, 1.0f);
        }
        fast.update(end, h);
        day = end;
        for (int r = 0; r < kRegionCount; ++r) {
            float s = fast.state().snowCover[r];
            CHECK(s >= 0 && s <= 1);
        }
        NEAR(fast.state().snowCover[int(Region::Temperate)], slow.state().snowCover[int(Region::Temperate)], 0.02);
    }
    // Winter day: day speed up, night slowed down, cycle duration unchanged.
    Engine w;
    w.configure(d);
    w.update(33.5, 0);
    const SeasonState& s = w.state();
    CHECK(s.dayScale > 1 && s.nightScale < 1);
    double vday = d.vanillaSunsetHour - d.vanillaSunriseHour;
    NEAR(vday / s.dayScale + (24 - vday) / s.nightScale, 24, 1e-3);
}

static void testEngineConfigureClamps() {
    SeasonConfig c = defaults();
    c.transitionSteps = 50;
    Engine e;
    e.configure(c);
    CHECK(e.config().transitionSteps == 8);
    CHECK(e.allStages().size() == size_t(kPhaseCount * 9));
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    testNames();
    testCalendar();
    testStages();
    testJitter();
    testRegionOffsets();
    testTemperatureWeather();
    testSnow();
    testLightAndParticles();
    testIni();
    testColdFloorAndSleep();
    testEngineConfigureClamps();
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
