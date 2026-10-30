// QuattroStagioni - season engine: calendar, stages, temperature, weather, snow, light.
// Config parsing and the ini template live in season_config.cpp.
#include "season.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

#include "season_internal.h"

namespace qs::season {

// ---------------------------------------------------------------------------------------------
// names

const char* toString(Phase p) {
    switch (p) {
        case Phase::Spring: return "Spring";
        case Phase::Summer: return "Summer";
        case Phase::LateSummer: return "LateSummer";
        case Phase::Autumn: return "Autumn";
        case Phase::Winter: return "Winter";
    }
    return "?";
}

char toChar(Phase p) { return "SULAW"[int(p) % kPhaseCount]; }

const char* phaseNameIt(Phase p) {
    switch (p) {
        case Phase::Spring: return "Primavera";
        case Phase::Summer: return "Estate";
        case Phase::LateSummer: return "Fine estate";
        case Phase::Autumn: return "Autunno";
        case Phase::Winter: return "Inverno";
    }
    return "?";
}

const char* toString(Region r) {
    switch (r) {
        case Region::Temperate: return "Temperate";
        case Region::NorthCold: return "NorthCold";
        case Region::SwampJungle: return "SwampJungle";
        case Region::Aquatic: return "Aquatic";
        case Region::DesertDry: return "DesertDry";
        case Region::Abyss: return "Abyss";
        case Region::Count: break;
    }
    return "?";
}

const char* toString(DaySource s) {
    switch (s) {
        case DaySource::GameClock: return "gameclock";
        case DaySource::Playtime: return "playtime";
        case DaySource::Manual: return "manual";
    }
    return "?";
}

namespace {

std::string lowerAlnum(const std::string& s) {  // "North_Cold" -> "northcold"
    std::string o;
    for (char c : s) {
        if (c >= 'A' && c <= 'Z') o += char(c - 'A' + 'a');
        else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) o += c;
    }
    return o;
}

}  // namespace

bool regionFromName(const std::string& name, Region& out) {
    static const struct { const char* key; Region r; } kNames[] = {
        {"temperate", Region::Temperate},     {"sharedtemperate", Region::Temperate},
        {"northcold", Region::NorthCold},     {"north", Region::NorthCold},
        {"swampjungle", Region::SwampJungle}, {"swamp", Region::SwampJungle},
        {"jungle", Region::SwampJungle},      {"aquatic", Region::Aquatic},
        {"desertdry", Region::DesertDry},     {"desert", Region::DesertDry},
        {"abyss", Region::Abyss},
    };
    std::string k = lowerAlnum(name);
    for (const auto& n : kNames)
        if (k == n.key) {
            out = n.r;
            return true;
        }
    return false;
}

Region regionFromHint(const std::string& hint) {
    Region r = Region::Temperate;
    return regionFromName(hint, r) ? r : Region::Temperate;
}

// ---------------------------------------------------------------------------------------------
// calendar internals (season_internal.h)

namespace detail {

double wrap(double x, double y) {
    if (!(y > 0)) return 0;
    double r = x - y * std::floor(x / y);
    if (r < 0 || r >= y) r = 0;  // rounding at the edge
    return r;
}

Cal makeCal(const SeasonConfig& c, int region) {
    Cal cal;
    cal.year = c.yearLength();
    double raw[kPhaseCount], sum = 0;
    bool unscaled = true;
    for (int i = 0; i < kPhaseCount; ++i) {
        double scale = region >= 0 ? c.regions[region].phaseScale[i] : 1.0;
        unscaled = unscaled && scale == 1.0;
        raw[i] = double(c.phaseDays[i]) * scale;
        sum += raw[i];
    }
    double k = sum > 0 ? cal.year / sum : 1.0;
    double s = 0;
    for (int i = 0; i < kPhaseCount; ++i) {
        cal.start[i] = s;
        // unscaled phases keep their exact configured length (clean boundaries for the global calendar)
        cal.len[i] = unscaled ? double(c.phaseDays[i]) : raw[i] * k;
        s += cal.len[i];
    }
    cal.offset = region >= 0 ? c.regions[region].dayOffset : 0.0;
    return cal;
}

Pos locate(const SeasonConfig& c, const Cal& cal, double local) {
    Pos p;
    p.local = local;
    p.phase = kPhaseCount - 1;
    for (int i = 0; i < kPhaseCount; ++i)
        if (local < cal.start[i] + cal.len[i]) {
            p.phase = i;
            break;
        }
    double len = cal.len[p.phase];
    p.progress = len > 0 ? (local - cal.start[p.phase]) / len : 0.0;
    p.progress = std::min(std::max(p.progress, 0.0), std::nextafter(1.0, 0.0));
    double tf = c.transitionFraction;
    double tailStart = 1.0 - tf;
    p.blend = (tf > 0 && p.progress >= tailStart) ? std::min(1.0, (p.progress - tailStart) / tf) : 0.0;
    return p;
}

int effectiveSteps(const SeasonConfig& c) { return c.transitionFraction > 0 ? c.transitionSteps : 0; }

VisualStage stageFromPos(const SeasonConfig& c, const Pos& p) {
    VisualStage s;
    Phase ph = Phase(p.phase);
    int n = effectiveSteps(c);
    s.from = s.to = ph;
    if (n <= 0 || p.progress < 1.0 - double(c.transitionFraction)) return s;
    int k = int(std::floor(p.blend * n));
    k = std::min(std::max(k, 0), n - 1);
    s.to = nextPhase(ph);
    s.step = k + 1;
    s.t = float(s.step) / float(n + 1);
    return s;
}

double localDay(const SeasonConfig& c, const Cal& cal, double day) { return wrap(day - cal.offset, cal.year); }

}  // namespace detail

using namespace detail;

// ---------------------------------------------------------------------------------------------
// VisualStage

std::string VisualStage::id() const {
    std::string s(1, toChar(from));
    if (step == 0) return s;
    s += '>';
    s += toChar(to);
    s += '.';
    s += std::to_string(step);
    return s;
}

bool parseStageId(const std::string& id, int transitionSteps, VisualStage& out) {
    auto phaseOf = [](char ch, Phase& p) {
        const char* letters = "SULAW";
        for (int i = 0; i < kPhaseCount; ++i)
            if (letters[i] == ch) {
                p = Phase(i);
                return true;
            }
        return false;
    };
    VisualStage s;
    if (id.empty() || !phaseOf(id[0], s.from)) return false;
    if (id.size() == 1) {
        s.to = s.from;
        out = s;
        return true;
    }
    if (id.size() < 5 || id[1] != '>' || !phaseOf(id[2], s.to) || id[3] != '.') return false;
    if (s.to != nextPhase(s.from)) return false;
    int step = 0;
    for (size_t i = 4; i < id.size(); ++i) {
        if (id[i] < '0' || id[i] > '9' || step > 1000) return false;
        step = step * 10 + (id[i] - '0');
    }
    if (step < 1 || step > transitionSteps) return false;
    s.step = step;
    s.t = float(step) / float(transitionSteps + 1);
    out = s;
    return true;
}

float assetJitter(const SeasonConfig& cfg, uint32_t seed) {
    // murmur3 fmix32 on a salted seed: well spread even for sequential seeds.
    uint32_t h = seed ^ 0x5EA5011Du;
    h ^= h >> 16;
    h *= 0x85EBCA6Bu;
    h ^= h >> 13;
    h *= 0xC2B2AE35u;
    h ^= h >> 16;
    double u = double(h >> 8) / double(0xFFFFFF);  // 0..1, exact in double
    return float((u * 2.0 - 1.0) * double(cfg.assetJitterDays));
}

VisualStage stageAt(const SeasonConfig& cfg, Region r, double day) {
    int ri = int(r);
    if (ri < 0 || ri >= kRegionCount || !cfg.regions[ri].enabled) return VisualStage();
    Cal cal = makeCal(cfg, ri);
    return stageFromPos(cfg, locate(cfg, cal, localDay(cfg, cal, day)));
}

VisualStage stageAt(const SeasonConfig& cfg, Region r, double day, uint32_t assetSeed) {
    return stageAt(cfg, r, day + double(assetJitter(cfg, assetSeed)));
}

std::vector<VisualStage> allStages(const SeasonConfig& cfg) {
    std::vector<VisualStage> v;
    int n = effectiveSteps(cfg);
    for (int i = 0; i < kPhaseCount; ++i) {
        VisualStage p;
        p.from = p.to = Phase(i);
        v.push_back(p);
        for (int k = 1; k <= n; ++k) {
            VisualStage s;
            s.from = Phase(i);
            s.to = nextPhase(s.from);
            s.step = k;
            s.t = float(k) / float(n + 1);
            v.push_back(s);
        }
    }
    return v;
}

VisualStage nextStage(const SeasonConfig& cfg, const VisualStage& s) {
    int n = effectiveSteps(cfg);
    VisualStage o;
    if (s.step < n) {
        o.from = s.from;
        o.to = nextPhase(s.from);
        o.step = s.step + 1;
        o.t = float(o.step) / float(n + 1);
    } else {
        o.from = o.to = s.step == 0 ? nextPhase(s.from) : s.to;
    }
    return o;
}

// ---------------------------------------------------------------------------------------------
// SeasonConfig calendar helpers

float SeasonConfig::yearLength() const {
    double s = 0;
    for (float d : phaseDays) s += d;
    return float(s);
}

int SeasonConfig::stageCount() const { return kPhaseCount * (1 + effectiveSteps(*this)); }

float SeasonConfig::phaseLength(Phase p, Region r) const {
    return float(makeCal(*this, int(r)).len[int(p)]);
}

float SeasonConfig::phaseStartDay(Phase p, Region r) const {
    Cal cal = makeCal(*this, int(r));
    return float(wrap(cal.start[int(p)] + cal.offset, cal.year));
}

double SeasonConfig::dayOfYear(double day) const { return wrap(day, yearLength()); }

long long SeasonConfig::yearIndex(double day) const {
    double y = yearLength();
    return y > 0 ? (long long)std::floor(day / y) : 0;
}

std::string SeasonConfig::dayToLabel(double day, Region r) const {
    Cal cal = makeCal(*this, int(r));
    Pos p = locate(*this, cal, localDay(*this, cal, day));
    double len = cal.len[p.phase];
    int total = std::max(1, int(std::lround(len)));
    int cur = int(std::floor(p.progress * len)) + 1;
    cur = std::min(std::max(cur, 1), total);
    char buf[96];
    std::snprintf(buf, sizeof buf, "%s, giorno %d di %d", phaseNameIt(Phase(p.phase)), cur, total);
    return buf;
}

double SeasonConfig::jumpToPhase(double currentDay, Phase p, Region r) const {
    double y = yearLength();
    return double(yearIndex(currentDay)) * y + double(phaseStartDay(p, r));
}

double SeasonConfig::advance(double day, double realSecondsElapsed, double gameHoursElapsed) const {
    switch (daySource) {
        case DaySource::Playtime:
            return day + realSecondsElapsed / (60.0 * std::max(1.0, double(realMinutesPerDay)));
        case DaySource::GameClock: return day + gameHoursElapsed / 24.0;
        case DaySource::Manual: break;
    }
    return day;
}

double SeasonConfig::nextStageChangeDay(double day, Region r) const {
    int ri = int(r);
    if (ri < 0 || ri >= kRegionCount || !regions[ri].enabled) return std::numeric_limits<double>::infinity();
    Cal cal = makeCal(*this, ri);
    double local = localDay(*this, cal, day);
    int n = effectiveSteps(*this);
    double tf = transitionFraction;
    // Walk the boundaries of this year and the first phase of the next one.
    for (int pass = 0; pass < 2; ++pass)
        for (int i = 0; i < kPhaseCount; ++i) {
            double base = cal.start[i] + pass * cal.year, len = cal.len[i];
            double cand[10];
            int m = 0;
            if (n > 0) {
                double tailStart = base + len * (1.0 - tf);
                cand[m++] = tailStart;
                for (int k = 1; k < n && m < 9; ++k) cand[m++] = tailStart + len * tf * k / n;
            }
            cand[m++] = base + len;
            for (int j = 0; j < m; ++j)
                if (cand[j] > local + 1e-9) return day + (cand[j] - local);
        }
    return std::numeric_limits<double>::infinity();
}

// ---------------------------------------------------------------------------------------------
// temperature, weather, leaves, light

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kMaxDeclination = 23.44;

double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }
double lerp(double a, double b, double t) { return a + (b - a) * t; }

double dailyMeanTemp(const RegionConfig& rc, const Cal& cal, double local) {
    double midWinter = cal.start[4] + cal.len[4] * 0.5;
    double t = rc.meanTempC - rc.ampTempC * std::cos(2.0 * kPi * (local - midWinter) / cal.year);
    return std::max(t, double(kMinTempC));
}

double diurnal(const RegionConfig& rc, double hour) {
    return -double(rc.diurnalTempC) * std::cos(2.0 * kPi * (hour - 3.0) / 24.0);
}

// Base weather mix per phase: sunny, cloudy, precipitation, storm, windy (temperate, humidity 0.5).
// Precipitation is split into rain and snow by temperature afterwards.
const double kWeatherBase[kPhaseCount][5] = {
    {0.40, 0.25, 0.25, 0.04, 0.06},  // Spring: showers
    {0.72, 0.13, 0.07, 0.05, 0.03},  // Summer: clear
    {0.62, 0.15, 0.08, 0.07, 0.08},  // LateSummer: dry, some thunderstorms
    {0.20, 0.27, 0.33, 0.05, 0.15},  // Autumn: rainy and windy
    {0.25, 0.30, 0.35, 0.02, 0.08},  // Winter: overcast, snowy when cold
};

WeatherWeights weatherFor(const RegionConfig& rc, const Pos& pos, double tMean) {
    const double* a = kWeatherBase[pos.phase];
    const double* b = kWeatherBase[(pos.phase + 1) % kPhaseCount];
    double v[5];
    for (int i = 0; i < 5; ++i) v[i] = lerp(a[i], b[i], pos.blend);
    double h = clampd(rc.humidity, 0, 1);
    double wet = 0.3 + 1.4 * h;
    double sunny = v[0] * (1.5 - h), cloudy = v[1] * (0.6 + 0.8 * h), precip = v[2] * wet,
           storm = v[3] * wet, windy = v[4];
    double snowFrac = clampd((2.0 - tMean) / 4.0, 0, 1);  // half snow at 0 C, all snow at -2 C
    WeatherWeights w;
    w.sunny = float(sunny);
    w.cloudy = float(cloudy);
    w.rainy = float(precip * (1 - snowFrac));
    w.snowy = float((precip + storm) * snowFrac);  // a cold storm is a snowstorm
    w.storm = float(storm * (1 - snowFrac));
    w.windy = float(windy);
    double sum = double(w.sunny) + w.cloudy + w.rainy + w.storm + w.snowy + w.windy;
    float k = float(1.0 / sum);
    w.sunny *= k, w.cloudy *= k, w.rainy *= k, w.storm *= k, w.snowy *= k, w.windy *= k;
    return w;
}

// Falling leaves along the regional calendar: start in the late-summer tail, grow through autumn,
// peak early in the autumn -> winter transition, fade out in the first half of the winter plateau.
double leavesAt(const SeasonConfig& c, const Cal& cal, double local) {
    const int L = 2, A = 3, W = 4;
    double tf = c.transitionFraction;
    double tailL = cal.len[L] * std::max(tf, 0.15);  // a short lead-in even with hard switches
    double tailA = cal.len[A] * std::max(tf, 0.15);
    double plateauW = cal.len[W] * (1.0 - tf);
    double k0 = cal.start[A] - tailL;
    double xs[6] = {k0, cal.start[A], cal.start[A] + cal.len[A] - tailA,
                    cal.start[A] + cal.len[A] - tailA * 0.7, cal.start[W], cal.start[W] + plateauW * 0.5};
    const double ys[6] = {0.0, 0.25, 0.8, 1.0, 0.3, 0.0};
    double x = wrap(local - k0, cal.year);
    for (int i = 0; i < 6; ++i) xs[i] -= k0;
    for (int i = 0; i < 5; ++i)
        if (x >= xs[i] && x < xs[i + 1]) {
            double span = xs[i + 1] - xs[i];
            return span > 0 ? lerp(ys[i], ys[i + 1], (x - xs[i]) / span) : ys[i + 1];
        }
    return 0.0;
}

// Light per phase: r, g, b tint and saturation (Summer is vanilla).
const double kLight[kPhaseCount][4] = {
    {1.00, 1.02, 0.99, 1.05},  // Spring: fresh, slightly greener
    {1.00, 1.00, 1.00, 1.00},  // Summer: vanilla
    {1.04, 1.00, 0.93, 0.97},  // LateSummer: golden, dusty
    {1.07, 0.98, 0.88, 1.02},  // Autumn: warm
    {0.92, 0.97, 1.07, 0.82},  // Winter: cold, desaturated
};

// Seasonal solar curve: +1 on the whole Summer plateau (vanilla light), -1 at mid-winter, cosine ramps
// in between. Smooth at both ends because the cosine has zero slope at 0 and pi.
double solarCurve(const SeasonConfig& c, const Cal& cal, double doy) {
    double us = cal.start[1];
    double ue = cal.len[1] * (1.0 - c.transitionFraction);
    double mw = wrap(cal.start[4] + cal.len[4] * 0.5 - us, cal.year);
    double x = wrap(doy - us, cal.year);
    if (x <= ue) return 1.0;
    if (x <= mw) return std::cos(kPi * (x - ue) / std::max(1e-9, mw - ue));
    return -std::cos(kPi * (x - mw) / std::max(1e-9, cal.year - mw));
}

double dayLengthHours(double latDeg, double declDeg) {
    double phi = latDeg * kPi / 180.0, d = declDeg * kPi / 180.0;
    double x = clampd(-std::tan(phi) * std::tan(d), -1.0, 1.0);
    return 2.0 * (std::acos(x) * 180.0 / kPi) / 15.0;
}

double noonElevation(double latDeg, double declDeg) { return 90.0 - std::fabs(latDeg - declDeg); }

struct Sample {
    Pos pos;
    double tMean = 0, tNow = 0;
    WeatherWeights w;
};

Sample sampleRegion(const SeasonConfig& c, int r, double day, double hour) {
    const RegionConfig& rc = c.regions[r];
    Cal cal = makeCal(c, r);
    Sample s;
    s.pos = locate(c, cal, localDay(c, cal, day));
    s.tMean = dailyMeanTemp(rc, cal, s.pos.local);
    s.tNow = std::max(s.tMean + diurnal(rc, hour), double(kMinTempC));
    Pos wp = s.pos;
    if (!rc.enabled) {  // excluded regions: summer weather, no snow
        wp.phase = int(Phase::Summer);
        wp.blend = 0;
    }
    s.w = weatherFor(rc, wp, s.tMean);
    return s;
}

// Expected snow change over `hours` at one sample (no RNG: probability-weighted rates).
double snowStep(const SeasonConfig& c, const Sample& s, double cover, double hours) {
    double cold = clampd((1.0 - s.tNow) / 2.0, 0, 1);  // snow settles below about 0 C
    double acc = double(c.snowAccumPerHour) * s.w.snowy * cold;
    double melt = double(c.snowMeltPerHour) * clampd(s.tNow / 3.0, 0, 2);
    return clampd(cover + hours * (acc - melt), 0, 1);
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Engine

void Engine::configure(const SeasonConfig& cfg) {
    cfg_ = cfg;
    cfg_.clamp(nullptr);
    hasPrev_ = false;  // calendar may have changed: re-simulate the snow on the next update
}

void Engine::update(double day, float inGameHours) {
    double frac = day - std::floor(day);
    update(day, inGameHours, float(frac * 24.0));
}

void Engine::update(double day, float inGameHours, float hourOfDay) {
    const SeasonConfig& c = cfg_;
    SeasonState& st = state_;
    double hour = wrap(double(hourOfDay), 24.0);
    double hoursElapsed = inGameHours >= 0 ? double(inGameHours) : std::max(0.0, (day - prevDay_) * 24.0);

    // ---- snow: integrate since the previous update, or re-simulate after a jump
    if (c.snowAccumulation) {
        bool jump = !hasPrev_ || day < prevDay_ - 1e-9 || day - prevDay_ > 4.0 || hoursElapsed > 96.0;
        double fromDay = prevDay_, hours = hoursElapsed;
        if (jump) {
            for (double& s : snow_) s = 0;
            fromDay = day - 14.0;
            hours = 14.0 * 24.0;
        }
        if (hours > 0) {
            int n = std::min(2000, std::max(1, int(std::ceil(hours / 0.5))));
            double dt = hours / n;
            for (int k = 1; k <= n; ++k) {
                double f = double(k) / n;
                double d = lerp(fromDay, day, f);
                double h = wrap(hour - (hours - hours * f), 24.0);  // the hour runs with game time
                for (int r = 0; r < kRegionCount; ++r) {
                    if (!c.regions[r].enabled) continue;
                    snow_[r] = snowStep(c, sampleRegion(c, r, d, h), snow_[r], dt);
                }
            }
        }
    } else {
        for (double& s : snow_) s = 0;
    }
    prevDay_ = day;
    prevHour_ = hour;
    hasPrev_ = true;

    // ---- global calendar
    Cal g = makeCal(c, -1);
    double doy = wrap(day, g.year);
    Pos gp = locate(c, g, doy);
    st.day = day;
    st.dayOfYear = float(doy);
    st.phase = Phase(gp.phase);
    st.phaseProgress = float(gp.progress);
    st.hourOfDay = float(hour);
    st.breathThresholdC = c.breathThresholdC;

    // ---- regions
    for (int r = 0; r < kRegionCount; ++r) {
        const RegionConfig& rc = c.regions[r];
        Sample s = sampleRegion(c, r, day, hour);
        Cal cal = makeCal(c, r);
        st.regionActive[r] = rc.enabled;
        st.regionPhase[r] = rc.enabled ? Phase(s.pos.phase) : Phase::Summer;
        st.regionPhaseProgress[r] = rc.enabled ? float(s.pos.progress) : 0.0f;
        st.stage[r] = rc.enabled ? stageFromPos(c, s.pos) : VisualStage();
        st.temperatureC[r] = float(s.tNow);
        st.dailyMeanTempC[r] = float(s.tMean);
        st.weather[r] = s.w;
        st.snowCover[r] = rc.enabled ? float(snow_[r]) : 0.0f;
        double lv = rc.enabled && c.particles ? leavesAt(c, cal, s.pos.local) * rc.leaves : 0.0;
        st.leaves[r] = float(clampd(lv, 0, 1));
        st.breathVisible[r] = c.particles && s.tNow < c.breathThresholdC;
    }
    st.fallingLeaves = st.leaves[int(Region::Temperate)];

    // ---- light: global calendar, Summer plateau = vanilla
    if (c.lightControl) {
        const double* a = kLight[gp.phase];
        const double* b = kLight[(gp.phase + 1) % kPhaseCount];
        for (int i = 0; i < 3; ++i) st.lightTint[i] = float(lerp(a[i], b[i], gp.blend));
        st.saturation = float(lerp(a[3], b[3], gp.blend));
    } else {
        st.lightTint[0] = st.lightTint[1] = st.lightTint[2] = 1.0f;
        st.saturation = 1.0f;
    }
    double lat = c.virtualLatitudeDeg;
    double decl = kMaxDeclination * solarCurve(c, g, doy);
    st.sunElevationOffsetDeg =
        c.lightControl ? float(noonElevation(lat, decl) - noonElevation(lat, kMaxDeclination)) : 0.0f;
    double rise = c.vanillaSunriseHour, set = c.vanillaSunsetHour;
    if (c.dayLengthControl) {
        double shorten = dayLengthHours(lat, kMaxDeclination) - dayLengthHours(lat, decl);
        double maxShorten = std::max(0.0, (set - rise) - 4.0);  // keep at least 4 hours of daylight
        shorten = clampd(shorten, 0.0, maxShorten);
        rise += shorten * 0.5;
        set -= shorten * 0.5;
    }
    st.sunriseHour = float(rise);
    st.sunsetHour = float(set);
    st.dayLengthHours = float(set - rise);
    double vday = double(c.vanillaSunsetHour) - double(c.vanillaSunriseHour);
    st.dayScale = float(vday / (set - rise));
    st.nightScale = float((24.0 - vday) / (24.0 - (set - rise)));
}

VisualStage Engine::stageFor(Region r, uint32_t assetSeed) const { return stageAt(cfg_, r, state_.day, assetSeed); }

std::vector<VisualStage> Engine::allStages() const { return season::allStages(cfg_); }

VisualStage Engine::nextStage(const VisualStage& s) const { return season::nextStage(cfg_, s); }

}  // namespace qs::season
