// QuattroStagioni - native unit tests for the season applier core (src/core/apply/).
// Usage: test_apply [data_dir]   (data_dir unused: everything is synthetic)
// Covers: targets.ini parsing and validation, the deterministic weather schedule, the game clock
// tracker, the SeasonState -> target value mapping, and the controller against a fake memory:
// originals saved at attach and restored on stop, scale/offset/write modes, Min/Max, MaxRate,
// "value not sticking" auto-disable, refused writes, object loss (vtable change), bit/mask
// preservation, the weather manual flag dependency, feature switches and the status snapshot.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../src/core/apply/applier.h"
#include "../src/core/apply/targets.h"

using namespace qs::apply;
namespace season = qs::season;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond)                                                                         \
    do {                                                                                    \
        if (cond) ++g_pass;                                                                 \
        else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); }       \
    } while (0)

static bool near(double a, double b, double eps = 1e-4) { return std::fabs(a - b) <= eps; }

// ---------------------------------------------------------------------------------------------
// Fake memory: a few regions, each readable and optionally writable. Counts writes per address.
struct FakeMem : Memory {
    struct Region {
        uint64_t base;
        std::vector<uint8_t> bytes;
        bool writable;
    };
    std::vector<Region> regions;
    std::map<uint64_t, int> writes;
    uint64_t add(uint64_t base, size_t size, bool writable) {
        regions.push_back({base, std::vector<uint8_t>(size, 0), writable});
        return base;
    }
    Region* find(uint64_t a, size_t n) {
        for (Region& r : regions)
            if (a >= r.base && a + n <= r.base + r.bytes.size()) return &r;
        return nullptr;
    }
    bool read(uint64_t a, void* out, size_t n) override {
        Region* r = find(a, n);
        if (!r) return false;
        std::memcpy(out, r->bytes.data() + (a - r->base), n);
        return true;
    }
    bool write(uint64_t a, const void* in, size_t n, std::string* why) override {
        Region* r = find(a, n);
        if (!r || !r->writable) {
            if (why) *why = "not a read-write data page";
            return false;
        }
        std::memcpy(r->bytes.data() + (a - r->base), in, n);
        ++writes[a];
        return true;
    }
    template <class T>
    void put(uint64_t a, T v) {
        Region* r = find(a, sizeof(T));
        if (r) std::memcpy(r->bytes.data() + (a - r->base), &v, sizeof(T));
    }
    template <class T>
    T get(uint64_t a) {
        T v{};
        read(a, &v, sizeof(T));
        return v;
    }
};

static season::SeasonState stateAt(double day, season::SeasonConfig* cfgOut = nullptr) {
    season::SeasonConfig cfg;
    cfg.setDefaults();
    season::Engine e;
    e.configure(cfg);
    e.update(day, -1.0f);
    if (cfgOut) *cfgOut = cfg;
    return e.state();
}

// ---------------------------------------------------------------------------------------------
static void testParse() {
    TargetsConfig c;
    std::vector<std::string> errors;
    CHECK(!parseTargetsText("[Features]\nWeather=1\n", c, errors));  // no [General]
    errors.clear();
    const char* text = R"(
[General]
Enabled=1
IntervalMs=100
AnomalyLimit=3
Region=NorthCold
AllowHooks=1

[Features]
Particles=0

[Weather]
HoldHours=4
BlendHours=1
FlagStorm=0x40
IndexSnowy=7

[object.time]
Signature=48 8B 05 ?? ?? ?? ?? 48 85 C0
Resolve=rip:3:7
Chain=0x0, 0x60
Check=0x10:f32:0:24

[object.weather]
Class=pa::PAWeatherManager
Instance=scan
Multiple=all
MaxInstances=2

[object.light]
Class=pa::Light
Instance=capture
Slot=3

[target.day]
Enabled=1
Object=time
Semantic=time_scale_day
Offset=0x18
Min=0.25
Max=4
MaxRate=0.5

[target.hour]
Enabled=1
Object=time
Semantic=hour
Offset=0x10

[target.rainmix]
Object=weather
Semantic=weather_mix_rainy
Offset=0x40

[target.flags]
Enabled=1
Object=weather
Semantic=weather_state_flags
Type=flags32
Mask=0x3F
Offset=0x38

[target.manual]
Enabled=1
Object=weather
Semantic=weather_manual_flag
Type=u8
Offset=0x31

[target.bad_semantic]
Object=weather
Semantic=moon_phase
Offset=0x10
[target.bad_object]
Object=nope
Semantic=snow_amount
Offset=0x10
[target.bad_mode]
Object=time
Semantic=hour
Mode=write
Offset=0x10
[target.bad_bit]
Object=weather
Semantic=snow_amount
Type=f32
Bit=3
Offset=0x10
[target.bad_offset]
Object=weather
Semantic=snow_amount
[target.bad_minmax]
Object=weather
Semantic=snow_amount
Offset=0x10
Min=2
Max=1
[target.bad_vtable_offset]
Object=weather
Semantic=snow_amount
Offset=0x4
[object.bad_scan]
Instance=scan
[object.bad_global]
Instance=global
[object.bad_capture]
Class=pa::X
Instance=capture
)";
    CHECK(parseTargetsText(text, c, errors));
    for (const auto& e : errors) std::printf("  (expected) %s\n", e.c_str());
    CHECK(errors.size() == 10);
    CHECK(c.enabled && c.allowHooks && c.anomalyLimit == 3 && c.intervalMs == 100);
    CHECK(c.region == int(season::Region::NorthCold));
    CHECK(!c.features[int(Feature::Particles)] && c.features[int(Feature::Weather)]);
    CHECK(near(c.holdHours, 4) && near(c.blendHours, 1));
    CHECK(c.flag[int(Weather::Storm)] == 0x40 && c.index[int(Weather::Snowy)] == 7);
    CHECK(c.flag[int(Weather::Rainy)] == 0x01 && c.index[int(Weather::Sunny)] == 1);
    CHECK(c.objects.size() == 3);
    CHECK(c.objects[0].name == "time" && c.objects[0].instance == InstanceKind::Global && c.objects[0].chain.size() == 2);
    CHECK(c.objects[0].resolve.enabled && c.objects[0].resolve.insnLen == 7 && c.objects[0].checks.size() == 1);
    CHECK(c.objects[1].instance == InstanceKind::Scan && c.objects[1].multiple == MultiplePolicy::All && c.objects[1].maxInstances == 2);
    CHECK(c.objects[2].instance == InstanceKind::Capture && c.objects[2].slot == 3);
    CHECK(c.targets.size() == 5);
    const TargetSpec& d = c.targets[0];
    CHECK(d.name == "day" && d.enabled && d.semantic == Semantic::TimeScaleDay && d.mode == Mode::Scale);
    CHECK(d.feature == Feature::DayLength && d.offset == 0x18 && near(d.minV, 0.25) && near(d.maxV, 4) && near(d.maxRate, 0.5));
    CHECK(c.targets[1].mode == Mode::Read && c.targets[1].feature == Feature::Clock);
    CHECK(!c.targets[2].enabled && c.targets[2].semantic == Semantic::WeatherMix && c.targets[2].weather == Weather::Rainy);
    CHECK(c.targets[3].kind == FieldKind::Flags32 && c.targets[3].mask == 0x3F);
    CHECK(c.targets[4].objectIndex == 1 && c.targets[4].kind == FieldKind::U8);

    // defaults: everything off
    TargetsConfig d0;
    errors.clear();
    CHECK(parseTargetsText("[General]\n[object.o]\nClass=pa::A\n[target.t]\nObject=o\nSemantic=snow_amount\nOffset=0x10\n", d0, errors));
    CHECK(errors.empty() && !d0.enabled && !d0.allowHooks && d0.targets.size() == 1 && !d0.targets[0].enabled);
    CHECK(d0.targets[0].restore && d0.targets[0].mode == Mode::Write);

    // names round-trip
    for (int i = 0; i < int(Semantic::Count); ++i) {
        Semantic s = Semantic(i), back;
        if (s == Semantic::WeatherWeight || s == Semantic::WeatherMix) continue;
        CHECK(semanticFromString(toString(s), back, nullptr) && back == s);
    }
    Semantic s;
    Weather w;
    CHECK(semanticFromString("weather_weight_snowy", s, &w) && s == Semantic::WeatherWeight && w == Weather::Snowy);
    CHECK(!semanticFromString("weather_weight_fog", s, &w));
    std::vector<ObjectCheck> ch;
    CHECK(parseCheckList("0x10:f32:0:24, 0x14:u32:0:100000", ch, nullptr) && ch.size() == 2 && ch[1].kind == FieldKind::U32);
    CHECK(!parseCheckList("0x10:f32:5:1", ch, nullptr));
}

static void testShippedIni() {
    FILE* f = std::fopen("dist/bin64/scripts/QuattroStagioni/targets.ini", "rb");
    if (!f) {
        std::printf("  (skipping shipped targets.ini check: not found from cwd)\n");
        return;
    }
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    std::fclose(f);
    TargetsConfig c;
    std::vector<std::string> errors;
    CHECK(parseTargetsText(text, c, errors));
    for (const std::string& e : errors) std::printf("  shipped targets.ini: %s\n", e.c_str());
    CHECK(errors.empty());
    CHECK(!c.enabled && !c.allowHooks);
    bool anyEnabled = false;
    for (const TargetSpec& t : c.targets) anyEnabled = anyEnabled || t.enabled;
    CHECK(!anyEnabled);           // every target disabled by default
    CHECK(c.targets.size() >= 15);  // one example per semantic family
}

// ---------------------------------------------------------------------------------------------
static void testScheduler() {
    season::WeatherWeights w;
    w.sunny = 0.5f;
    w.cloudy = 0.3f;
    w.rainy = 0.2f;
    WeatherScheduler a, b;
    a.configure(6, 1, 0.35, 42);
    b.configure(6, 1, 0.35, 42);
    int counts[kWeatherCount] = {};
    bool sameAll = true;
    for (int h = 0; h < 6 * 3000; h += 6) {
        const WeatherMix& ma = a.update(h + 3.0, w);
        const WeatherMix& mb = b.update(h + 3.0, w);
        sameAll = sameAll && ma.current == mb.current;
        ++counts[int(ma.current)];
    }
    CHECK(sameAll);  // deterministic
    CHECK(counts[int(Weather::Snowy)] == 0 && counts[int(Weather::Storm)] == 0 && counts[int(Weather::Windy)] == 0);
    CHECK(counts[int(Weather::Sunny)] > counts[int(Weather::Cloudy)] && counts[int(Weather::Cloudy)] > counts[int(Weather::Rainy)]);
    CHECK(counts[int(Weather::Rainy)] > 3000 * 0.12 && counts[int(Weather::Rainy)] < 3000 * 0.28);

    // re-anchoring after a backward jump gives the same weather as a fresh scheduler
    WeatherScheduler c;
    c.configure(6, 1, 0.35, 42);
    a.update(5000, w);
    Weather back = a.update(100.5, w).current;
    Weather fresh = c.update(100.5, w).current;
    CHECK(back == fresh);

    // inside a slot: constant weather, blend ramps over BlendHours
    WeatherScheduler d;
    d.configure(6, 2, 0.0, 7);
    long long slotWithChange = -1;
    for (long long k = 1; k < 200 && slotWithChange < 0; ++k) {
        d.update(double(k) * 6 + 0.01, w);
        if (d.mix().previous != d.mix().current) slotWithChange = k;
    }
    CHECK(slotWithChange > 0);
    double t0 = double(slotWithChange) * 6;
    const WeatherMix& m0 = d.update(t0 + 0.0, w);
    CHECK(near(m0.blend, 0.0, 1e-6) && near(m0.mix[int(m0.previous)], 1.0, 1e-6));
    Weather cur = m0.current;
    const WeatherMix& m1 = d.update(t0 + 1.0, w);
    CHECK(near(m1.blend, 0.5, 1e-6) && m1.current == cur);
    CHECK(near(m1.mix[int(m1.previous)] + m1.mix[int(m1.current)], 1.0, 1e-6));
    const WeatherMix& m2 = d.update(t0 + 5.9, w);
    CHECK(near(m2.blend, 1.0) && m2.current == cur && near(m2.mix[int(cur)], 1.0));

    // persistence keeps the weather longer
    WeatherScheduler p0, p9;
    p0.configure(6, 0, 0.0, 3);
    p9.configure(6, 0, 0.9, 3);
    int ch0 = 0, ch9 = 0;
    for (int k = 0; k < 2000; ++k) {
        ch0 += p0.update(k * 6.0 + 1, w).previous != p0.mix().current;
        ch9 += p9.update(k * 6.0 + 1, w).previous != p9.mix().current;
    }
    CHECK(ch9 * 3 < ch0);

    // a weather that becomes impossible (snow above zero) is replaced at once
    season::WeatherWeights snow;
    snow.snowy = 1;
    WeatherScheduler e;
    e.configure(6, 1, 0.9, 1);
    CHECK(e.update(10, snow).current == Weather::Snowy);
    CHECK(e.update(10.5, w).current != Weather::Snowy);
}

static void testClock() {
    ClockTracker c;
    double day, h;
    CHECK(!c.take(day, h));
    c.sample(false, 0, true, 22.0);
    CHECK(c.take(day, h) && near(h, 0) && day < 0);
    c.sample(false, 0, true, 23.5);
    c.sample(false, 0, true, 0.5);  // midnight
    CHECK(c.take(day, h) && near(h, 2.5));
    c.sample(false, 0, true, 0.2);  // clock set back (load): nothing elapsed
    CHECK(c.take(day, h) && near(h, 0));
    ClockTracker d;
    d.sample(true, 10, true, 6);
    d.sample(true, 10, true, 9);
    d.sample(true, 11, true, 6);  // slept 21 hours
    CHECK(d.take(day, h) && near(h, 24) && near(day, 11.25));
    d.sample(true, 0, true, 0);  // save loaded, counter reset
    CHECK(d.take(day, h) && near(h, 0));
    d.sample(false, 0, false, 0);
    CHECK(!d.known() && !d.take(day, h));
}

static void testMapping() {
    season::SeasonConfig sc;
    season::SeasonState win = stateAt(33.5, &sc);  // mid winter
    season::SeasonState sum = stateAt(12.0);       // summer plateau
    TargetsConfig cfg;
    WeatherMix wm;
    TargetSpec t;
    double v = 0;
    t.semantic = Semantic::TimeScaleDay;
    CHECK(seasonValue(t, cfg, win, sc, wm, 12, v) && near(v, win.dayScale) && v > 1.2);
    t.semantic = Semantic::TimeScaleNight;
    CHECK(seasonValue(t, cfg, win, sc, wm, 12, v) && v < 0.9);
    t.semantic = Semantic::TimeScaleCurrent;
    CHECK(seasonValue(t, cfg, win, sc, wm, 12, v) && near(v, win.dayScale));
    CHECK(seasonValue(t, cfg, win, sc, wm, 22, v) && near(v, win.nightScale));
    t.semantic = Semantic::SnowAmount;
    CHECK(seasonValue(t, cfg, win, sc, wm, 12, v) && near(v, win.snowCover[0]));
    t.semantic = Semantic::TemperatureOffset;
    double winOff = 0, sumOff = 0;
    CHECK(seasonValue(t, cfg, win, sc, wm, 12, winOff) && seasonValue(t, cfg, sum, sc, wm, 12, sumOff));
    CHECK(winOff < sumOff - 10 && sumOff <= 0.01);
    t.semantic = Semantic::LightTintB;
    CHECK(seasonValue(t, cfg, win, sc, wm, 12, v) && near(v, win.lightTint[2]));
    t.semantic = Semantic::SunElevationOffset;
    CHECK(seasonValue(t, cfg, win, sc, wm, 12, v) && v < 0);
    t.semantic = Semantic::Hour;
    CHECK(!seasonValue(t, cfg, win, sc, wm, 12, v));
    wm.current = Weather::Snowy;
    wm.previous = Weather::Rainy;
    wm.blend = 0.25f;
    for (float& m : wm.mix) m = 0;
    wm.mix[int(Weather::Snowy)] = 0.25f;
    wm.mix[int(Weather::Rainy)] = 0.75f;
    t.semantic = Semantic::WeatherStateFlags;
    CHECK(seasonValue(t, cfg, win, sc, wm, 12, v) && v == 0x02);
    t.semantic = Semantic::WeatherIndex;
    CHECK(seasonValue(t, cfg, win, sc, wm, 12, v) && v == 4);
    t.semantic = Semantic::Precipitation;
    CHECK(seasonValue(t, cfg, win, sc, wm, 12, v) && near(v, 1.0));
    t.semantic = Semantic::SnowRate;
    CHECK(seasonValue(t, cfg, win, sc, wm, 12, v) && near(v, 0.25));
    t.semantic = Semantic::WeatherMix;
    t.weather = Weather::Rainy;
    CHECK(seasonValue(t, cfg, win, sc, wm, 12, v) && near(v, 0.75));
    t.semantic = Semantic::WeatherWeight;
    t.weather = Weather::Sunny;
    CHECK(seasonValue(t, cfg, sum, sc, wm, 12, v) && v > 0.3);
    // feature switches
    CHECK(featureOn(Feature::Weather, cfg, win, sc));
    sc.weatherControl = false;
    CHECK(!featureOn(Feature::Weather, cfg, win, sc) && featureOn(Feature::Light, cfg, win, sc));
    cfg.region = int(season::Region::DesertDry);  // disabled region: leave the game alone
    CHECK(!featureOn(Feature::Snow, cfg, win, sc) && featureOn(Feature::DayLength, cfg, win, sc));
    cfg.features[int(Feature::DayLength)] = false;
    CHECK(!featureOn(Feature::DayLength, cfg, win, sc));
    // encoding
    uint8_t b[8];
    encodeField(FieldKind::U8, 300, b);
    CHECK(b[0] == 255);
    encodeField(FieldKind::I32, -2.6, b);
    CHECK(decodeField(FieldKind::I32, b) == -3);
    encodeField(FieldKind::F32, 0.1, b);
    CHECK(near(decodeField(FieldKind::F32, b), 0.1, 1e-7));
}

// ---------------------------------------------------------------------------------------------
// A fake "time manager" at 0x10000 (vtable 0x7000), "weather" at 0x20000 (vtable 0x7100).
static const char* kCtlIni = R"(
[General]
Enabled=1
AnomalyLimit=3
[Weather]
HoldHours=6
BlendHours=0
[object.time]
Class=pa::Time
[object.weather]
Class=pa::Weather
[target.hour]
Enabled=1
Object=time
Semantic=hour
Offset=0x10
[target.day]
Enabled=1
Object=time
Semantic=time_scale_day
Offset=0x18
Min=0.5
Max=1.5
MaxRate=1
[target.night]
Enabled=1
Object=time
Semantic=time_scale_night
Offset=0x1C
[target.snow]
Enabled=1
Object=weather
Semantic=snow_amount
Offset=0x40
Scale=100
[target.rewritten]
Enabled=1
Object=weather
Semantic=precipitation
Offset=0x4C
[target.flags]
Enabled=1
Object=weather
Semantic=weather_state_flags
Type=flags32
Mask=0x3F
Offset=0x38
[target.manual]
Enabled=1
Object=weather
Semantic=weather_manual_flag
Type=u8
Offset=0x31
[target.sunoff]
Enabled=1
Object=weather
Semantic=sun_elevation_offset
Mode=offset
Offset=0x5C
[target.tintb]
Enabled=1
Object=weather
Semantic=light_tint_b
Offset=0x58
[target.readonly_page]
Enabled=1
Object=weather
Semantic=leaves_particles
Offset=0x1800
)";

static void testController() {
    season::SeasonConfig sc;
    season::SeasonState win = stateAt(33.5, &sc);
    TargetsConfig cfg;
    std::vector<std::string> errors;
    CHECK(parseTargetsText(kCtlIni, cfg, errors) && errors.empty());
    for (const auto& e : errors) std::printf("  %s\n", e.c_str());
    FakeMem mem;
    const uint64_t T = mem.add(0x10000, 0x100, true), W = mem.add(0x20000, 0x1000, true);
    mem.add(0x21000, 0x1000, false);  // read-only page right after the weather object
    mem.put<uint64_t>(T, 0x7000);
    mem.put<float>(T + 0x10, 13.0f);
    mem.put<float>(T + 0x18, 1.0f);
    mem.put<float>(T + 0x1C, 1.0f);
    mem.put<uint64_t>(W, 0x7100);
    mem.put<uint8_t>(W + 0x31, 0);
    mem.put<uint32_t>(W + 0x38, 0x100 | 0x20);  // a game bit outside the mask + sunny
    mem.put<float>(W + 0x40, 0.0f);
    mem.put<float>(W + 0x4C, 0.25f);
    mem.put<float>(W + 0x58, 0.8f);
    mem.put<float>(W + 0x5C, 10.0f);
    mem.put<float>(W + 0x1800, 0.5f);  // on the read-only page

    Applier ap;
    ap.configure(cfg);
    CHECK(ap.objectNeeded(0) && ap.objectNeeded(1));
    ap.tick(win, sc, 0.1, mem);  // nothing bound: all waiting
    ApplyStatus st;
    ap.status(st);
    CHECK(st.features[int(Feature::DayLength)].state == FeatureState::NotFound);
    CHECK(st.features[int(Feature::Particles)].state == FeatureState::NotFound);
    CHECK(mem.writes.empty());

    ap.bindObject(0, {T}, 0x7000);
    ap.bindObject(1, {W}, 0x7100);
    // first tick: originals saved, day speed moves at most MaxRate*dt from 1.0
    ap.tick(win, sc, 0.1, mem);
    float d1 = mem.get<float>(T + 0x18);
    CHECK(d1 > 1.0f && near(d1, 1.1, 1e-5));                       // rate limited (1 per second, 0.1 s)
    CHECK(near(mem.get<float>(T + 0x1C), win.nightScale, 1e-5));  // no MaxRate: immediate
    CHECK(near(mem.get<float>(W + 0x40), win.snowCover[0] * 100, 1e-3));
    CHECK(near(mem.get<float>(W + 0x58), 0.8 * win.lightTint[2], 1e-5));
    CHECK(near(mem.get<float>(W + 0x5C), 10.0 + win.sunElevationOffsetDeg, 1e-4));
    uint32_t fl = mem.get<uint32_t>(W + 0x38);
    CHECK((fl & 0x100) && (fl & 0x3F) == cfg.flag[int(ap.weather().current)]);
    CHECK(mem.get<uint8_t>(W + 0x31) == 1);  // manual flag: written after the drivers became active
    double day = 0, hours = 0;
    CHECK(ap.clock().known() && ap.clock().take(day, hours) && near(ap.clock().hourOfDay(), 13.0));

    // a few ticks: rate limit reaches Max=1.5 (winter dayScale ~1.75 clamped), manual flag set
    for (int i = 0; i < 8; ++i) {
        float before = mem.get<float>(T + 0x18);
        mem.put<float>(W + 0x4C, 0.25f);  // the game rewrites this field every frame
        mem.put<float>(T + 0x10, 13.0f + 0.1f * float(i + 1));
        ap.tick(win, sc, 0.1, mem);
        float after = mem.get<float>(T + 0x18);
        CHECK(after - before <= 0.1f + 1e-5f);
    }
    CHECK(near(mem.get<float>(T + 0x18), 1.5, 1e-5));  // clamped by Max
    CHECK(mem.get<uint8_t>(W + 0x31) == 1);
    CHECK(ap.clock().take(day, hours) && near(hours, 0.8, 1e-4));
    ap.status(st);
    const TargetStatus* rew = nullptr;
    const TargetStatus* ro = nullptr;
    for (const auto& t : st.targets) {
        if (t.name == "rewritten") rew = &t;
        if (t.name == "readonly_page") ro = &t;
    }
    CHECK(rew && rew->state == TargetState::Error && rew->error.find("not sticking") != std::string::npos);
    CHECK(near(mem.get<float>(W + 0x4C), 0.25));  // its original back
    CHECK(ro && ro->state == TargetState::Error && ro->error.find("write refused") != std::string::npos);
    CHECK(st.features[int(Feature::DayLength)].state == FeatureState::Active);
    CHECK(st.features[int(Feature::Particles)].state == FeatureState::Error);
    CHECK(st.features[int(Feature::Weather)].state == FeatureState::Active);
    CHECK(st.clockKnown && near(st.gameHour, 13.8, 1e-4));

    // the game changes its own base speed: scale target re-bases (one external change is fine)
    mem.put<float>(T + 0x1C, 2.0f);
    ap.tick(win, sc, 0.1, mem);
    CHECK(near(mem.get<float>(T + 0x1C), 2.0 * win.nightScale, 1e-5));

    // feature switched off in the menu: original back at once
    season::SeasonConfig off = sc;
    off.dayLengthControl = false;
    ap.tick(win, off, 0.1, mem);
    CHECK(near(mem.get<float>(T + 0x18), 1.0) && near(mem.get<float>(T + 0x1C), 2.0));
    ap.status(st);
    CHECK(st.features[int(Feature::DayLength)].state == FeatureState::Inactive);
    ap.tick(win, sc, 0.1, mem);  // back on: fresh attach, rate limited again from 1.0
    CHECK(near(mem.get<float>(T + 0x18), 1.1, 1e-5));

    // stop: every original restored, game bits preserved
    mem.put<uint32_t>(W + 0x38, mem.get<uint32_t>(W + 0x38) | 0x400);  // game sets another bit meanwhile
    int restored = ap.restoreAll(mem);
    CHECK(restored >= 6);
    CHECK(near(mem.get<float>(T + 0x18), 1.0) && near(mem.get<float>(T + 0x1C), 2.0));
    CHECK(near(mem.get<float>(W + 0x40), 0.0) && near(mem.get<float>(W + 0x58), 0.8) && near(mem.get<float>(W + 0x5C), 10.0));
    CHECK(mem.get<uint8_t>(W + 0x31) == 0);
    CHECK(mem.get<uint32_t>(W + 0x38) == (0x100u | 0x400u | 0x20u));
}

static void testObjectLoss() {
    season::SeasonConfig sc;
    season::SeasonState win = stateAt(33.5, &sc);
    TargetsConfig cfg;
    std::vector<std::string> errors;
    CHECK(parseTargetsText(kCtlIni, cfg, errors));
    FakeMem mem;
    const uint64_t W = mem.add(0x20000, 0x1000, true);
    mem.add(0x10000, 0x100, true);
    mem.put<uint64_t>(W, 0x7100);
    mem.put<float>(W + 0x40, 0.0f);
    Applier ap;
    ap.configure(cfg);
    ap.bindObject(1, {W}, 0x7100);
    ap.tick(win, sc, 0.1, mem);
    CHECK(mem.writes[W + 0x40] == 1);
    // object freed and its memory reused: vtable gone
    mem.put<uint64_t>(W, 0x9999);
    mem.put<float>(W + 0x40, 7.0f);
    std::vector<int> lost = ap.tick(win, sc, 0.1, mem);
    CHECK(lost.size() == 1 && lost[0] == 1 && !ap.objectBound(1));
    CHECK(near(mem.get<float>(W + 0x40), 7.0));  // untouched
    CHECK(ap.restoreAll(mem) == 0 && near(mem.get<float>(W + 0x40), 7.0));
    std::vector<std::string> log = ap.takeLog();
    bool sawLost = false;
    for (const auto& l : log) sawLost = sawLost || l.find("lost") != std::string::npos;
    CHECK(sawLost);
    // rebinding a new object reads a fresh original
    const uint64_t W2 = mem.add(0x30000, 0x1000, true);
    mem.put<uint64_t>(W2, 0x7100);
    mem.put<float>(W2 + 0x40, 0.5f);
    ap.bindObject(1, {W2}, 0x7100);
    ap.tick(win, sc, 0.1, mem);
    ap.restoreAll(mem);
    CHECK(near(mem.get<float>(W2 + 0x40), 0.5));

    // Multiple=all: every instance gets the value
    const uint64_t W3 = mem.add(0x40000, 0x1000, true);
    mem.put<uint64_t>(W3, 0x7100);
    ap.bindObject(1, {W2, W3}, 0x7100);
    ap.tick(win, sc, 0.1, mem);
    CHECK(near(mem.get<float>(W2 + 0x40), win.snowCover[0] * 100, 1e-3) && near(mem.get<float>(W3 + 0x40), win.snowCover[0] * 100, 1e-3));

    // the manual flag never goes on alone
    TargetsConfig only;
    errors.clear();
    CHECK(parseTargetsText("[General]\n[object.w]\nClass=pa::W\n[target.m]\nEnabled=1\nObject=w\nSemantic=weather_manual_flag\nType=u8\nOffset=0x31\n",
                           only, errors));
    Applier a2;
    a2.configure(only);
    mem.put<uint8_t>(W3 + 0x31, 0);
    a2.bindObject(0, {W3}, 0x7100);
    for (int i = 0; i < 5; ++i) a2.tick(win, sc, 0.1, mem);
    CHECK(mem.get<uint8_t>(W3 + 0x31) == 0);
    ApplyStatus st;
    a2.status(st);
    CHECK(st.targets[0].state == TargetState::Waiting && st.features[int(Feature::Weather)].state == FeatureState::NotFound);
}

int main(int, char**) {
    testParse();
    testShippedIni();
    testScheduler();
    testClock();
    testMapping();
    testController();
    testObjectLoss();
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
