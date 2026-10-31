// QuattroStagioni - unit tests of the seasonal weather rules (src/core/apply/weather.h).
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "../src/core/apply/weather.h"

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

static WeatherSeason seasonOf(Phase p, float winterK = -1) {
    WeatherSeason s;
    s.phase = p;
    s.progress = 0.3f;  // plateau: no transition
    s.winterK = winterK >= 0 ? winterK : (p == Phase::Winter ? 1.0f : 0.0f);
    return s;
}

static WeatherLayer gameLayer(float rain = 0, float snow = 0) {
    WeatherLayer g;
    g.precipitation = std::max(rain, snow);
    g.cloudiness = 0.4f;
    g.humidity = 0.3f;
    g.rain = rain;
    g.snow = snow;
    g.cover = 0.45f;
    g.hasCover = true;
    return g;
}

static void testConfig() {
    Ini ini;
    std::vector<std::string> w;
    WeatherConfig c;
    ini.parse("[General]\nEnabled=1\n");  // old ini without [Weather]
    c.load(ini, w);
    CHECK(w.empty() && c.enabled);
    CHECK(near(c.phase[int(Phase::Winter)].weight[int(WeatherKind::Overcast)], 37));
    CHECK(c.phase[int(Phase::Winter)].weight[int(WeatherKind::Game)] == 0);

    // the template parses back to the defaults, with no warning
    WeatherConfig t;
    w.clear();
    ini.parse(weatherIniTemplate());
    t.load(ini, w);
    CHECK(w.empty());
    bool same = true;
    for (int p = 0; p < season::kPhaseCount; ++p)
        for (int k = 0; k < kWeatherKinds; ++k) same &= near(t.phase[p].weight[k], c.phase[p].weight[k]);
    CHECK(same);
    CHECK(near(t.phase[int(Phase::Summer)].nativeRain, 0.25));

    WeatherConfig b;
    w.clear();
    ini.parse("[Weather]\nEnabled=0\nRampPerHour=abc\nSummer.Weights=1,2,3\nAutumn.Hours=9,2\nWinter.Rain=0.2,0.4\nClearCover=7\n");
    b.load(ini, w);
    CHECK(!b.enabled);
    CHECK(near(b.rampPerHour, 1.5));          // malformed: default kept
    CHECK(w.size() == 2);                     // RampPerHour, Summer.Weights
    CHECK(near(b.phase[int(Phase::Autumn)].minHours, 2) && near(b.phase[int(Phase::Autumn)].maxHours, 9));
    CHECK(near(b.phase[int(Phase::Winter)].rainMax, 0.4));
    CHECK(near(b.clearCover, 1.0));           // clamped
}

// Share of game time spent in each kind over a long run.
static void shares(Phase p, double out[kWeatherKinds], uint32_t seed = 12345) {
    WeatherDirector d;
    d.seed(seed);
    WeatherSeason s = seasonOf(p);
    double t[kWeatherKinds] = {};
    for (int i = 0; i < 20000; ++i) {
        d.advance(0.5, s);
        t[int(d.spell().kind)] += 0.5;
    }
    double sum = 0;
    for (double v : t) sum += v;
    for (int k = 0; k < kWeatherKinds; ++k) out[k] = t[k] / sum;
    d.takeLog();
}

static void testRules() {
    double sp[kWeatherKinds], su[kWeatherKinds], ls[kWeatherKinds], au[kWeatherKinds], wi[kWeatherKinds];
    shares(Phase::Spring, sp);
    shares(Phase::Summer, su);
    shares(Phase::LateSummer, ls);
    shares(Phase::Autumn, au);
    shares(Phase::Winter, wi);
    const int C = int(WeatherKind::Clear), R = int(WeatherKind::Rain), T = int(WeatherKind::Storm),
              O = int(WeatherKind::Overcast), Y = int(WeatherKind::Cloudy), G = int(WeatherKind::Game);
    CHECK(sp[C] > 0.45);                      // spring: mostly sunny
    CHECK(sp[R] < 0.15 && sp[T] < 0.05);      // little rain, rare storms
    CHECK(su[R] < 0.08 && su[T] > sp[T]);     // summer: little rain but thunderstorms
    CHECK(ls[R] < 0.12 && ls[T] > 0.02);      // late summer: some storms
    CHECK(au[R] + au[T] > 0.2 && au[R] + au[T] < 0.35 && au[R] > sp[R]);  // autumn: rain now and then, not always
    CHECK(au[Y] + au[O] > sp[Y] + sp[O]);     // autumn cloudier
    CHECK(wi[O] + wi[Y] > 0.55);              // winter: overcast / cloudy prevalent
    CHECK(wi[C] < 0.15 && wi[G] == 0);        // rarely a clear sky, never the game's own
    CHECK(wi[R] > 0.12 && wi[R] + wi[T] < 0.3);  // snow now and then, not always
}

static void testCompose() {
    WeatherConfig cfg;
    // Rain spell, no winter: rain, never snow; the game's snow is 0 too.
    WeatherSpell rain{WeatherKind::Rain, Phase::Autumn, 0.8f, 3};
    WeatherLevels lr = WeatherDirector::target(cfg, rain);
    CHECK(near(lr.precip, 0.8) && lr.coverFloor > 0.9f);

    // compose with a director whose levels are the target: run a long Autumn until a Rain spell holds
    WeatherDirector a;
    a.configure(cfg);
    a.seed(99);
    WeatherSeason au = seasonOf(Phase::Autumn);
    bool sawRain = false, snowOutside = false, sawStorm = false;
    for (int i = 0; i < 4000; ++i) {
        a.advance(0.25, au);
        WeatherLayer o = a.compose(gameLayer(0.3f, 0.0f), 0.0f, 0.0f);
        snowOutside |= o.snow > 0.0f;
        if (a.spell().kind == WeatherKind::Rain && a.levels().precip > 0.3f) sawRain |= o.rain >= a.levels().precip - 1e-5f;
        if (a.spell().kind == WeatherKind::Storm && a.levels().precip > 0.8f) sawStorm |= o.rain > 0.8f && o.cover > 0.95f;
    }
    CHECK(sawRain && sawStorm);
    CHECK(!snowOutside);  // no snow outside winter

    // KeepGameSnow=0: the game's own snow removed outside winter; kept (=1) by default.
    WeatherConfig ks = cfg;
    ks.nativeSnowOutsideWinter = false;
    WeatherDirector k0;
    k0.configure(ks);
    CHECK(k0.compose(gameLayer(0, 0.6f), 0.0f, 0.0f).snow == 0.0f);
    WeatherDirector k1;
    k1.configure(cfg);
    CHECK(near(k1.compose(gameLayer(0, 0.6f), 0.0f, 0.0f).snow, 0.6));
    // ...but only in a cold place: elsewhere the game's snow falls as rain (autumn storm, 31/10)
    WeatherLayer warm = k1.compose(gameLayer(0, 0.6f), 0.0f, 0.0f, false);
    CHECK(warm.snow == 0.0f && near(warm.rain, 0.6));
    CHECK(near(k1.compose(gameLayer(0, 0.6f), 1.0f, 0.0f, false).snow, 0.6));  // winter: kept anywhere

    // Winter: our precipitation and the game's rain become snow.
    WeatherDirector w;
    w.configure(cfg);
    w.seed(3);
    WeatherSeason wi = seasonOf(Phase::Winter);
    bool snowOk = true, sawSnow = false, sawGrey = true;
    for (int i = 0; i < 4000; ++i) {
        w.advance(0.25, wi);
        WeatherLayer o = w.compose(gameLayer(0.5f, 0.0f), 1.0f, 0.0f);
        snowOk &= o.rain == 0.0f;
        sawSnow |= o.snow > 0.5f;
        if (w.spell().kind == WeatherKind::Overcast && w.hoursLeft() < w.spell().hours - 1.0f) sawGrey &= o.cover >= 0.89f;
    }
    CHECK(snowOk && sawSnow && sawGrey);

    // Clear: capped cloud cover, the game's rain removed once the levels settle.
    WeatherSpell clear{WeatherKind::Clear, Phase::Summer, 0, 5};
    WeatherLevels lc = WeatherDirector::target(cfg, clear);
    CHECK(near(lc.coverCap, cfg.clearCover) && near(lc.suppress, 1));
}

static void testBypassAndSafety() {
    WeatherDirector d;
    d.seed(11);
    WeatherSeason wi = seasonOf(Phase::Winter);
    d.advance(10, wi);
    WeatherLayer g = gameLayer(0.4f, 0.1f);
    WeatherLayer o = d.compose(g, 1.0f, 1.0f);  // desert / off: the game's own, exactly
    CHECK(o.rain == g.rain && o.snow == g.snow && o.cover == g.cover && o.cloudiness == g.cloudiness &&
          o.precipitation == g.precipitation && o.humidity == g.humidity);
    WeatherLayer bad = g;
    bad.rain = std::nanf("");
    WeatherLayer ob = d.compose(bad, 1.0f, 0.0f);
    CHECK(std::isnan(ob.rain) && ob.snow == g.snow);  // untouched
    WeatherLayer h = d.compose(g, 1.0f, 0.5f), f = d.compose(g, 1.0f, 0.0f);
    CHECK(near(h.snow, (f.snow + g.snow) / 2, 1e-4));
}

static void testSmooth() {
    WeatherConfig cfg;
    WeatherDirector d;
    d.configure(cfg);
    d.seed(5);
    WeatherSeason s = seasonOf(Phase::Autumn);
    WeatherLevels prev = d.levels();
    bool ok = true;
    const float maxStep = cfg.rampPerHour * 0.25f + 1e-5f;
    for (int i = 0; i < 2000; ++i) {
        d.advance(0.25, s);
        const WeatherLevels& l = d.levels();
        ok &= std::fabs(l.precip - prev.precip) <= maxStep && std::fabs(l.coverFloor - prev.coverFloor) <= maxStep &&
              std::fabs(l.coverCap - prev.coverCap) <= maxStep && std::fabs(l.suppress - prev.suppress) <= maxStep;
        prev = l;
    }
    CHECK(ok);
    // a sleep of 8 h: one call, same result as 32 quarter hours (same seed)
    WeatherDirector a, b;
    a.seed(42);
    b.seed(42);
    a.advance(8.0, s);
    for (int i = 0; i < 32; ++i) b.advance(0.25, s);
    CHECK(a.spell().kind == b.spell().kind && near(a.levels().precip, b.levels().precip, 1e-5) && near(a.hoursLeft(), b.hoursLeft(), 1e-3));
    // the log names every spell
    std::vector<std::string> lines = a.takeLog();
    CHECK(!lines.empty() && lines[0].find("weather: spell ") == 0);
}

static void testSeasonChangeAndJump() {
    // a winter spell does not outlive a switch to summer (F5, 31/10: rain went on into summer)
    WeatherDirector d;
    d.seed(8);
    WeatherSeason wi = seasonOf(Phase::Winter), su = seasonOf(Phase::Summer);
    d.advance(1.0, wi);
    CHECK(d.spell().phase == Phase::Winter);
    d.advance(0.25, su);
    CHECK(d.spell().phase == Phase::Summer);
    // a jump of the clock (loading, sleep): one spell, levels at its target at once
    d.takeLog();
    d.jump(22.0, wi);
    std::vector<std::string> l = d.takeLog();
    CHECK(l.size() == 1 && d.spell().phase == Phase::Winter);
    WeatherLevels t = WeatherDirector::target(d.config(), d.spell());
    CHECK(near(d.levels().precip, t.precip) && near(d.levels().coverFloor, t.coverFloor) && near(d.levels().suppress, t.suppress));
    CHECK(near(d.hoursLeft(), d.spell().hours, 1e-3));
}

static void testTransition() {
    // late in autumn's tail some spells already follow winter's rules
    WeatherDirector d;
    d.seed(77);
    WeatherSeason s = seasonOf(Phase::Autumn);
    s.progress = 0.99f;
    int winter = 0;
    for (int i = 0; i < 3000; ++i) {
        d.advance(0.5, s);
        winter += d.spell().phase == Phase::Winter;
    }
    CHECK(winter > 2000);
    CHECK(near(snowShare(0.0f), 0) && near(snowShare(1.0f), 1) && near(snowShare(0.5f), 0.5));
}

static void testClock() {
    GameClock c;
    CHECK(c.feed(10.0f).hours == 0);         // first reading
    GameClock::Step s = c.feed(10.2f);
    CHECK(near(s.hours, 0.2) && !s.jump);
    s = c.feed(23.9f);
    CHECK(s.jump && near(s.hours, 13.7));    // a sleep
    s = c.feed(0.1f);
    CHECK(near(s.hours, 0.2) && !s.jump);    // midnight
    s = c.feed(0.1f);
    CHECK(s.hours == 0);                     // paused
    s = c.feed(-3.0f);
    CHECK(s.hours == 0 && near(c.hour(), 0.1));  // invalid: ignored
}

static void testDesert() {
    CHECK(near(climateTopC(0xFFu << 24), 50.0));
    CHECK(near(climateTopC(25u << 24), -40.196, 1e-2));
    CHECK(near(climateTopC(200u << 24), 28.43, 1e-2));
    int r, c;
    CHECK(climateCell(0, 0, r, c) && r == 256 && c == 256);
    CHECK(climateCell(-81, 160, r, c) && c == 254 && r == 258);
    CHECK(!climateCell(30000, 0, r, c));
    std::vector<uint32_t> land(81, 190u << 24), desert = land;
    desert[40] = 245u << 24;
    float hot = 0;
    CHECK(!desertBlock(land, 28.0f, &hot) && hot < 25.0f);
    CHECK(desertBlock(desert, 28.0f, &hot) && hot > 40.0f);
    CHECK(!desertBlock({}, 28.0f));
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
    Ini ini;
    ini.parse(text);
    WeatherConfig c, d;
    std::vector<std::string> w;
    c.load(ini, w);
    CHECK(w.empty());
    CHECK(ini.has("Weather", "Winter.Weights"));
    bool same = c.enabled == d.enabled && near(c.rampPerHour, d.rampPerHour) && near(c.desertTopC, d.desertTopC);
    for (int p = 0; p < season::kPhaseCount; ++p)
        for (int k = 0; k < kWeatherKinds; ++k) same &= near(c.phase[p].weight[k], d.phase[p].weight[k]);
    CHECK(same);
}

static void testHud() {
    CHECK(hudWeatherIndex(0) == -1 && std::string(hudWeatherName(-1)) == "unknown");
    CHECK(hudWeatherIndex(0x20) == 5 && std::string(hudWeatherName(5)) == "sunny");
    CHECK(hudWeatherIndex(0x41) == 0);  // rain shown before storm
    CHECK(hudWeatherIndex(kHudStormBit) == 6);
}

int main(int, char**) {
    testHud();
    testShippedIni();
    testConfig();
    testRules();
    testCompose();
    testBypassAndSafety();
    testSmooth();
    testSeasonChangeAndJump();
    testTransition();
    testClock();
    testDesert();
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
