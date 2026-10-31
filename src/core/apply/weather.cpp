// QuattroStagioni - seasonal weather (see weather.h).
#include "weather.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <sstream>

#include "targets.h"

namespace qs::apply {

namespace {

using season::Phase;

float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
float toward(float v, float t, float step) { return v < t ? std::min(v + step, t) : std::max(v - step, t); }
float mix(float a, float b, float f) { return a + (b - a) * f; }

std::string fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));
std::string fmt(const char* f, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, f);
    vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}

const char* const kPhaseKey[season::kPhaseCount] = {"Spring", "Summer", "LateSummer", "Autumn", "Winter"};
const char* const kKindName[kWeatherKinds] = {"game", "clear", "cloudy", "overcast", "rain", "storm"};

// Comma separated numbers; false when the count differs or one is not a number.
bool parseList(const std::string& s, std::vector<float>& out, size_t n) {
    out.clear();
    std::stringstream ss(s);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        double d;
        if (!parseDouble(tok, d) || !std::isfinite(d)) return false;
        out.push_back(float(d));
    }
    return out.size() == n;
}

std::string list(const float* v, size_t n, const char* f = "%g") {
    std::string s;
    for (size_t i = 0; i < n; ++i) {
        if (i) s += ", ";
        char b[32];
        snprintf(b, sizeof b, f, double(v[i]));
        s += b;
    }
    return s;
}

}  // namespace

const char* toString(WeatherKind k) {
    int i = int(k);
    return i >= 0 && i < kWeatherKinds ? kKindName[i] : "?";
}

const char* weatherKindIt(WeatherKind k, bool snowing) {
    switch (k) {
        case WeatherKind::Game: return "meteo del gioco";
        case WeatherKind::Clear: return "sereno";
        case WeatherKind::Cloudy: return "nuvoloso";
        case WeatherKind::Overcast: return "coperto";
        case WeatherKind::Rain: return snowing ? "neve" : "pioggia";
        case WeatherKind::Storm: return snowing ? "bufera di neve" : "temporale";
    }
    return "?";
}

WeatherConfig::WeatherConfig() {
    // Stefano's rules. Weights: Game, Clear, Cloudy, Overcast, Rain, Storm.
    struct D {
        float w[kWeatherKinds];
        float h0, h1, s0, s1, r0, r1, smin, native;
    };
    const D d[season::kPhaseCount] = {
        {{15, 50, 22, 0, 10, 3}, 3, 8, 1, 2.5f, 0.25f, 0.55f, 0.85f, 0.5f},     // spring: sunny, little rain, rare storms
        {{15, 58, 12, 0, 5, 10}, 2, 6, 1, 2.5f, 0.2f, 0.5f, 0.85f, 0.25f},      // summer: little rain, thunderstorms
        {{15, 50, 20, 0, 8, 7}, 3, 7, 1, 3, 0.25f, 0.6f, 0.85f, 0.35f},         // late summer: little rain, some storms
        {{10, 15, 22, 13, 25, 15}, 3, 9, 1.5f, 4, 0.4f, 0.9f, 0.85f, 1.0f},     // autumn: rain and storms, cloudier
        {{0, 5, 25, 37, 28, 5}, 4, 10, 2, 5, 0.45f, 1.0f, 0.85f, 1.0f},         // winter: overcast, frequent snow
    };
    for (int p = 0; p < season::kPhaseCount; ++p) {
        PhaseWeather& w = phase[p];
        std::copy(d[p].w, d[p].w + kWeatherKinds, w.weight);
        w.minHours = d[p].h0;
        w.maxHours = d[p].h1;
        w.stormMinHours = d[p].s0;
        w.stormMaxHours = d[p].s1;
        w.rainMin = d[p].r0;
        w.rainMax = d[p].r1;
        w.stormMin = d[p].smin;
        w.nativeRain = d[p].native;
    }
}

void WeatherConfig::load(const Ini& ini, std::vector<std::string>& warnings) {
    const char* S = "Weather";
    auto num = [&](const char* key, float& out, float lo, float hi) {
        if (!ini.has(S, key)) return;
        double d;
        if (parseDouble(ini.get(S, key), d) && std::isfinite(d)) out = clampf(float(d), lo, hi);
        else warnings.push_back(std::string("[Weather] ") + key + "=" + ini.get(S, key) + " not a number: default kept");
    };
    if (ini.has(S, "Enabled")) enabled = ini.getInt(S, "Enabled", 1) != 0;
    if (ini.has(S, "KeepGameSnow")) nativeSnowOutsideWinter = ini.getInt(S, "KeepGameSnow", 1) != 0;
    num("RampPerHour", rampPerHour, 0.1f, 60.0f);
    num("ClearCover", clearCover, 0.0f, 1.0f);
    num("DesertTopTempC", desertTopC, 0.0f, 60.0f);
    num("DesertFadeSec", desertFadeSec, 0.0f, 60.0f);
    num("LogIntervalSec", logIntervalSec, 0.0f, 3600.0f);
    if (ini.has(S, "DesertRadius")) desertRadius = int(std::clamp<long long>(ini.getInt(S, "DesertRadius", 4), 0, 12));
    if (ini.has(S, "StartDelayMs")) startDelayMs = uint32_t(std::clamp<long long>(ini.getInt(S, "StartDelayMs", 20000), 0, 600000));

    for (int p = 0; p < season::kPhaseCount; ++p) {
        PhaseWeather& w = phase[p];
        std::vector<float> v;
        auto key = [&](const char* k) { return std::string(kPhaseKey[p]) + "." + k; };
        auto bad = [&](const std::string& k) {
            warnings.push_back("[Weather] " + k + "=" + ini.get(S, k) + " malformed: default kept");
        };
        std::string k = key("Weights");
        if (ini.has(S, k)) {
            if (parseList(ini.get(S, k), v, kWeatherKinds) && std::all_of(v.begin(), v.end(), [](float x) { return x >= 0; })) {
                std::copy(v.begin(), v.end(), w.weight);
            } else bad(k);
        }
        auto pair = [&](const char* name, float& a, float& b, float lo, float hi) {
            std::string kk = key(name);
            if (!ini.has(S, kk)) return;
            if (parseList(ini.get(S, kk), v, 2)) {
                a = clampf(std::min(v[0], v[1]), lo, hi);
                b = clampf(std::max(v[0], v[1]), lo, hi);
            } else bad(kk);
        };
        pair("Hours", w.minHours, w.maxHours, 0.25f, 72.0f);
        pair("StormHours", w.stormMinHours, w.stormMaxHours, 0.25f, 72.0f);
        pair("Rain", w.rainMin, w.rainMax, 0.0f, 1.0f);
        std::string ks = key("StormMin");
        if (ini.has(S, ks)) {
            double d;
            if (parseDouble(ini.get(S, ks), d) && std::isfinite(d)) w.stormMin = clampf(float(d), 0.0f, 1.0f);
            else bad(ks);
        }
        std::string kn = key("GameRain");
        if (ini.has(S, kn)) {
            double d;
            if (parseDouble(ini.get(S, kn), d) && std::isfinite(d)) w.nativeRain = clampf(float(d), 0.0f, 1.0f);
            else bad(kn);
        }
    }
}

std::string weatherIniTemplate() {
    WeatherConfig c;
    std::string s =
        "[Weather]\n"
        "; Meteo stagionale: la mod sceglie a turni (\"periodi\") il tempo della stagione sull'orologio del\n"
        "; gioco (dormire o aspettare lo fa avanzare) e lo sovrappone al meteo del gioco con passaggi graduali.\n"
        "; Nel deserto il meteo resta sempre quello del gioco. Si spegne anche dal menu F5 (\"Meteo stagionale\").\n"
        "; Tutte le chiavi sono facoltative: se mancano valgono questi default.\n"
        "Enabled=1\n"
        "; variazione per ora di gioco (1.5 = passaggio completo in circa 40 minuti di gioco)\n"
        "RampPerHour=1.5\n"
        "; copertura nuvolosa massima con il sereno (0..1; il gioco di solito circa 0.45)\n"
        "ClearCover=0.25\n"
        "; deserto: temperatura massima del giorno (mappa del clima) da cui una zona conta come deserto\n"
        "DesertTopTempC=28\n"
        "; texel (80 m) intorno alla telecamera controllati per il deserto\n"
        "DesertRadius=4\n"
        "; secondi reali per restituire il cielo al gioco entrando nel deserto\n"
        "DesertFadeSec=3\n"
        "; 1 = fuori dall'inverno resta la neve del gioco (montagne innevate); 0 = nessuna neve fuori dall'inverno\n"
        "KeepGameSnow=1\n"
        "; ogni quanti secondi reali scrivere nel log i valori composti (0 = mai)\n"
        "LogIntervalSec=60\n"
        "StartDelayMs=20000\n"
        "; Per fase (Spring, Summer, LateSummer, Autumn, Winter):\n"
        ";   Weights    pesi relativi del prossimo periodo: gioco, sereno, nuvoloso, coperto, pioggia, temporale\n"
        ";              (d'inverno pioggia = neve, temporale = bufera di neve)\n"
        ";   Hours      durata di un periodo in ore di gioco (min, max); StormHours per i temporali\n"
        ";   Rain       intensità della pioggia o neve (min, max, 0..1); StormMin intensità minima del temporale\n"
        ";   GameRain   quota della pioggia del gioco che resta (0..1; d'inverno diventa neve)\n"
        "; I temporali per ora sono pioggia fortissima con cielo chiuso: i fulmini non sono ancora controllati.\n";
    const char* comment[season::kPhaseCount] = {"; primavera: soleggiata, poca pioggia, temporali rari",
                                                 "; estate: poca pioggia ma temporali estivi",
                                                 "; fine estate: poca pioggia, qualche temporale",
                                                 "; autunno: pioggia e temporali frequenti, più nuvole",
                                                 "; inverno: cielo coperto o nuvoloso, neve frequente, mai sereno a lungo"};
    for (int p = 0; p < season::kPhaseCount; ++p) {
        const PhaseWeather& w = c.phase[p];
        const std::string k = kPhaseKey[p];
        s += std::string(comment[p]) + "\n";
        s += k + ".Weights=" + list(w.weight, kWeatherKinds) + "\n";
        float h[2] = {w.minHours, w.maxHours}, sh[2] = {w.stormMinHours, w.stormMaxHours}, r[2] = {w.rainMin, w.rainMax};
        s += k + ".Hours=" + list(h, 2) + "\n";
        s += k + ".StormHours=" + list(sh, 2) + "\n";
        s += k + ".Rain=" + list(r, 2) + "\n";
        s += k + ".StormMin=" + list(&w.stormMin, 1) + "\n";
        s += k + ".GameRain=" + list(&w.nativeRain, 1) + "\n";
    }
    return s;
}

float snowShare(float winterK) { return clamp01((winterK - 0.3f) / 0.4f); }

float WeatherDirector::uniform() {
    // xorshift32: deterministic for the tests, seeded from the clock in the plugin.
    uint32_t x = rng_;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_ = x;
    return float(x >> 8) / float(1u << 24);
}

WeatherLevels WeatherDirector::target(const WeatherConfig& c, const WeatherSpell& s) {
    WeatherLevels t;
    t.nativeRain = c.phase[int(s.phase)].nativeRain;
    const float i = clamp01(s.intensity);
    switch (s.kind) {
        case WeatherKind::Game: break;
        case WeatherKind::Clear:
            t.coverCap = c.clearCover;
            t.cloudCap = std::max(c.clearCover, 0.3f);
            t.suppress = 1;
            break;
        case WeatherKind::Cloudy:
            t.coverFloor = 0.6f;
            t.cloudFloor = 0.6f;
            break;
        case WeatherKind::Overcast:
            t.coverFloor = 0.9f;
            t.cloudFloor = 0.85f;
            t.humid = 0.6f;
            break;
        case WeatherKind::Rain:
            t.precip = i;
            t.coverFloor = 0.75f + 0.25f * i;
            t.cloudFloor = 0.5f + 0.5f * i;
            t.humid = i;
            break;
        case WeatherKind::Storm:
            t.precip = i;
            t.coverFloor = 1;
            t.cloudFloor = 1;
            t.humid = 1;
            break;
    }
    return t;
}

void WeatherDirector::roll(const WeatherSeason& s) {
    // In the transition tail the next phase's rules come in as the tail goes on.
    Phase ph = s.phase;
    const float tf = clampf(s.transitionFraction, 0.0f, 0.95f);
    if (tf > 0 && s.progress > 1.0f - tf) {
        float t = (s.progress - (1.0f - tf)) / tf;
        if (uniform() < t) ph = season::nextPhase(ph);
    }
    const PhaseWeather& r = cfg_.phase[int(ph)];
    float sum = 0;
    for (float w : r.weight) sum += std::max(0.0f, w);
    WeatherKind k = WeatherKind::Game;
    if (sum > 0) {
        float u = uniform() * sum;
        for (int i = 0; i < kWeatherKinds; ++i) {
            float w = std::max(0.0f, r.weight[i]);
            if (w <= 0) continue;
            k = WeatherKind(i);
            if (u < w) break;
            u -= w;
        }
    }
    WeatherSpell sp;
    sp.kind = k;
    sp.phase = ph;
    const bool storm = k == WeatherKind::Storm;
    sp.hours = storm ? mix(r.stormMinHours, r.stormMaxHours, uniform()) : mix(r.minHours, r.maxHours, uniform());
    sp.hours = std::max(sp.hours, 0.25f);
    if (k == WeatherKind::Rain) sp.intensity = mix(r.rainMin, r.rainMax, uniform());
    else if (storm) sp.intensity = mix(r.stormMin, 1.0f, uniform());
    else sp.intensity = 0;
    spell_ = sp;
    rolled_ = true;
    until_ = clock_ + sp.hours;
    const bool snowing = snowShare(s.winterK) >= 0.5f;
    log_.push_back(fmt("weather: spell %s (%s) for %.1f game h, intensity %.0f%%, season %s%s", toString(k),
                       weatherKindIt(k, snowing), sp.hours, sp.intensity * 100.0f, season::toString(ph),
                       storm ? " (storm = heavy rain tier: lightning not yet controlled)" : ""));
}

void WeatherDirector::advance(double gameHours, const WeatherSeason& s) {
    if (!(gameHours > 0)) return;
    double left = std::min(gameHours, 48.0);
    while (left > 1e-9) {
        double h = std::min(left, 0.25);
        left -= h;
        clock_ += h;
        if (!rolled_ || clock_ >= until_) roll(s);
        const WeatherLevels t = target(cfg_, spell_);
        const float step = float(cfg_.rampPerHour * h);
        lv_.precip = toward(lv_.precip, t.precip, step);
        lv_.coverFloor = toward(lv_.coverFloor, t.coverFloor, step);
        lv_.coverCap = toward(lv_.coverCap, t.coverCap, step);
        lv_.cloudFloor = toward(lv_.cloudFloor, t.cloudFloor, step);
        lv_.cloudCap = toward(lv_.cloudCap, t.cloudCap, step);
        lv_.humid = toward(lv_.humid, t.humid, step);
        lv_.suppress = toward(lv_.suppress, t.suppress, step);
        lv_.nativeRain = toward(lv_.nativeRain, t.nativeRain, step);
    }
}

WeatherLayer WeatherDirector::compose(const WeatherLayer& g, float winterK, float bypass) const {
    const float in[6] = {g.precipitation, g.cloudiness, g.humidity, g.snow, g.rain, g.cover};
    for (int i = 0; i < 6; ++i)
        if (!std::isfinite(in[i])) return g;
    bypass = clamp01(std::isfinite(bypass) ? bypass : 1.0f);
    if (bypass >= 1.0f) return g;

    const WeatherLevels& L = lv_;
    const float w = snowShare(winterK);
    const float keep = clamp01(1.0f - L.suppress);
    const float nativeRain = clamp01(g.rain) * clamp01(L.nativeRain) * keep;
    // The game's own snow: always kept in winter, outside it only with KeepGameSnow.
    const float gameSnow = clamp01(g.snow) * keep * (w > 0 || cfg_.nativeSnowOutsideWinter ? 1.0f : 0.0f);
    // The game's rain turns to snow as winter comes; ours the same.
    const float rainN = nativeRain * (1.0f - w), snowN = std::max(gameSnow, nativeRain * w);
    WeatherLayer o = g;
    o.rain = std::max(rainN, L.precip * (1.0f - w));
    o.snow = std::max(snowN, L.precip * w);
    o.precipitation = std::max(clamp01(g.precipitation) * keep, std::max(o.rain, o.snow));
    o.humidity = std::max(clamp01(g.humidity), L.humid);
    const float cloudCap = std::max(L.cloudCap, L.cloudFloor);
    o.cloudiness = clampf(std::max(clamp01(g.cloudiness), L.cloudFloor), 0.0f, cloudCap);
    if (g.hasCover) {
        const float coverCap = std::max(L.coverCap, L.coverFloor);
        o.cover = clampf(std::max(clamp01(g.cover), L.coverFloor), 0.0f, coverCap);
    }
    if (bypass > 0) {
        o.precipitation = mix(o.precipitation, g.precipitation, bypass);
        o.cloudiness = mix(o.cloudiness, g.cloudiness, bypass);
        o.humidity = mix(o.humidity, g.humidity, bypass);
        o.snow = mix(o.snow, g.snow, bypass);
        o.rain = mix(o.rain, g.rain, bypass);
        o.cover = mix(o.cover, g.cover, bypass);
    }
    return o;
}

std::vector<std::string> WeatherDirector::takeLog() {
    std::vector<std::string> out;
    out.swap(log_);
    return out;
}

GameClock::Step GameClock::feed(float hour, double maxRunHours, double maxJumpHours) {
    Step s;
    if (!std::isfinite(hour) || hour < 0.0f || hour > 24.0f) return s;
    if (last_ < 0) {
        last_ = hour;
        return s;
    }
    double d = double(hour) - double(last_);
    if (d < 0) d += 24.0;  // past midnight (or a sleep over it)
    last_ = hour;
    if (d <= 0) return s;
    if (d <= maxRunHours) {
        s.hours = d;
        return s;
    }
    s.jump = true;
    s.hours = std::min(d, maxJumpHours);
    return s;
}

float climateTopC(uint32_t texel) { return (float(texel >> 24) / 255.0f * 2.0f - 1.0f) * 50.0f; }

bool climateCell(float x, float z, int& row, int& col, int size) {
    if (!std::isfinite(x) || !std::isfinite(z)) return false;
    const float c = std::floor(x / kClimateTexelM) + float(size / 2), r = std::floor(z / kClimateTexelM) + float(size / 2);
    if (c < 0 || r < 0 || c >= float(size) || r >= float(size)) return false;
    row = int(r);
    col = int(c);
    return true;
}

bool desertBlock(const std::vector<uint32_t>& block, float thresholdC, float* hottest) {
    float hot = -1000.0f;
    for (uint32_t t : block) hot = std::max(hot, climateTopC(t));
    if (hottest) *hottest = hot;
    return !block.empty() && hot >= thresholdC;
}

}  // namespace qs::apply

namespace qs::apply {

int hudWeatherIndex(uint32_t flags) {
    for (int i = 0; i < 7; ++i)
        if (flags & (1u << i)) return i;
    return -1;
}

const char* hudWeatherName(int index) {
    static const char* const n[7] = {"rainy", "snowy", "windy", "fog", "cloudy", "sunny", "storm"};
    return index >= 0 && index < 7 ? n[index] : "unknown";
}

}  // namespace qs::apply
