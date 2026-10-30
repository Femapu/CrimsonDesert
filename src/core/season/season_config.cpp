// QuattroStagioni - season config: defaults, validation, ini load/save and the documented template.
//
// The saved text is the template the end user edits, so its comments are in Italian. Numbers are
// formatted and parsed by hand: the plugin lives inside the game process, where the C locale could
// have been changed, and Italian users often type "0,4" instead of "0.4".
#include <cmath>
#include <cstdio>
#include <string>

#include "season.h"
#include "season_internal.h"

namespace qs::season {
namespace {

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}

std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

// Locale independent decimal parser: [+-]digits[(.|,)digits]. No exponent, no garbage.
bool parseNum(const std::string& in, double& out) {
    std::string s = trim(in);
    size_t i = 0;
    bool neg = false;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) neg = s[i++] == '-';
    double v = 0, scale = 1;
    int digits = 0;
    bool dot = false;
    for (; i < s.size(); ++i) {
        char c = s[i];
        if (c >= '0' && c <= '9') {
            if (dot) {
                scale /= 10;
                v += (c - '0') * scale;
            } else {
                v = v * 10 + (c - '0');
            }
            ++digits;
        } else if ((c == '.' || c == ',') && !dot) {
            dot = true;
        } else {
            return false;
        }
    }
    if (!digits || v > 1e9) return false;
    out = neg ? -v : v;
    return true;
}

bool parseBool(const std::string& in, bool& out) {
    std::string s = lower(trim(in));
    if (s == "1" || s == "true" || s == "yes" || s == "on" || s == "si" || s == "s\xc3\xac") return out = true, true;
    if (s == "0" || s == "false" || s == "no" || s == "off") return out = false, true;
    return false;
}

bool parseDaySource(const std::string& in, DaySource& out) {
    std::string s = lower(trim(in));
    if (s == "gameclock" || s == "0") return out = DaySource::GameClock, true;
    if (s == "playtime" || s == "1") return out = DaySource::Playtime, true;
    if (s == "manual" || s == "2") return out = DaySource::Manual, true;
    return false;
}

// Shortest clean text for a config value: up to 4 decimals, trailing zeros removed, '.' separator.
std::string fmt(double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.4f", v);
    std::string s = buf;
    for (char& c : s)
        if (c == ',') c = '.';
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    if (s == "-0") s = "0";
    return s;
}

const char* kPhaseKey[kPhaseCount] = {"Spring", "Summer", "LateSummer", "Autumn", "Winter"};

struct FloatField {
    std::string section, key;
    float* p;
    float lo, hi;
};

// Every float field with its ini name and valid range (load and clamp share it).
std::vector<FloatField> floatFields(SeasonConfig& c) {
    std::vector<FloatField> f;
    for (int i = 0; i < kPhaseCount; ++i)
        f.push_back({"Seasons", std::string(kPhaseKey[i]) + "Days", &c.phaseDays[i], 0.5f, 120.0f});
    f.push_back({"Seasons", "TransitionFraction", &c.transitionFraction, 0.0f, 1.0f});
    f.push_back({"Seasons", "AssetJitterDays", &c.assetJitterDays, 0.0f, 10.0f});
    f.push_back({"Seasons", "RealMinutesPerDay", &c.realMinutesPerDay, 1.0f, 1440.0f});
    f.push_back({"Seasons", "StartDay", &c.startDay, 0.0f, 1000000.0f});
    f.push_back({"Seasons", "SnowAccumPerHour", &c.snowAccumPerHour, 0.0f, 5.0f});
    f.push_back({"Seasons", "SnowMeltPerHour", &c.snowMeltPerHour, 0.0f, 5.0f});
    f.push_back({"Seasons", "VirtualLatitude", &c.virtualLatitudeDeg, 0.0f, 65.0f});
    f.push_back({"Seasons", "VanillaSunrise", &c.vanillaSunriseHour, 0.0f, 10.0f});
    f.push_back({"Seasons", "VanillaSunset", &c.vanillaSunsetHour, 15.0f, 24.0f});
    f.push_back({"Seasons", "BreathThreshold", &c.breathThresholdC, -20.0f, 20.0f});
    for (int r = 0; r < kRegionCount; ++r) {
        std::string sec = std::string("Region.") + toString(Region(r));
        RegionConfig& rc = c.regions[r];
        f.push_back({sec, "DayOffset", &rc.dayOffset, -60.0f, 60.0f});
        for (int i = 0; i < kPhaseCount; ++i)
            f.push_back({sec, std::string(kPhaseKey[i]) + "Scale", &rc.phaseScale[i], 0.1f, 5.0f});
        f.push_back({sec, "MeanTemp", &rc.meanTempC, -40.0f, 50.0f});
        f.push_back({sec, "TempAmplitude", &rc.ampTempC, 0.0f, 40.0f});
        f.push_back({sec, "DailyAmplitude", &rc.diurnalTempC, 0.0f, 20.0f});
        f.push_back({sec, "Humidity", &rc.humidity, 0.0f, 1.0f});
        f.push_back({sec, "Leaves", &rc.leaves, 0.0f, 2.0f});
    }
    return f;
}

struct BoolField {
    const char* key;
    bool SeasonConfig::*p;
};
const BoolField kBools[] = {
    {"WeatherControl", &SeasonConfig::weatherControl},     {"SnowAccumulation", &SeasonConfig::snowAccumulation},
    {"LightControl", &SeasonConfig::lightControl},         {"DayLengthControl", &SeasonConfig::dayLengthControl},
    {"Particles", &SeasonConfig::particles},
};

}  // namespace

// ---------------------------------------------------------------------------------------------

void SeasonConfig::setDefaults() {
    *this = SeasonConfig();
    struct D {
        bool enabled;
        float offset, scale[kPhaseCount], mean, amp, diurnal, humidity, leaves;
    };
    // DESIGN.md section 4. Temperate: mid-winter about -4 C (snow), summer 24 C. North: winter earlier
    // and 50% longer, shorter summer, mid-winter about -13 C. Swamp: evergreen and humid, never freezes.
    const D d[kRegionCount] = {
        {true, 0.0f, {1, 1, 1, 1, 1}, 10.0f, 14.0f, 4.0f, 0.5f, 1.0f},           // Temperate
        {true, -3.0f, {1, 0.7f, 0.8f, 1, 1.5f}, 1.0f, 14.0f, 3.5f, 0.55f, 0.6f},  // NorthCold
        {true, 0.0f, {1, 1, 1, 1, 1}, 22.0f, 5.0f, 3.0f, 0.85f, 0.2f},           // SwampJungle
        {true, 0.0f, {1, 1, 1, 1, 1}, 12.0f, 11.0f, 2.5f, 0.7f, 0.3f},           // Aquatic
        {false, 0.0f, {1, 1, 1, 1, 1}, 27.0f, 10.0f, 9.0f, 0.05f, 0.0f},         // DesertDry (excluded)
        {false, 0.0f, {1, 1, 1, 1, 1}, 15.0f, 0.0f, 0.0f, 0.3f, 0.0f},           // Abyss (excluded)
    };
    for (int r = 0; r < kRegionCount; ++r) {
        RegionConfig& rc = regions[r];
        rc.enabled = d[r].enabled;
        rc.dayOffset = d[r].offset;
        for (int i = 0; i < kPhaseCount; ++i) rc.phaseScale[i] = d[r].scale[i];
        rc.meanTempC = d[r].mean;
        rc.ampTempC = d[r].amp;
        rc.diurnalTempC = d[r].diurnal;
        rc.humidity = d[r].humidity;
        rc.leaves = d[r].leaves;
    }
}

void SeasonConfig::clamp(std::vector<std::string>* errors) {
    for (const FloatField& f : floatFields(*this)) {
        float v = *f.p;
        float c = v >= f.lo ? (v <= f.hi ? v : f.hi) : f.lo;  // NaN -> lo
        if (c != v || std::isnan(v)) {
            if (errors)
                errors->push_back("[" + f.section + "] " + f.key + " = " + fmt(v) + " is out of range (" +
                                  fmt(f.lo) + " .. " + fmt(f.hi) + "), using " + fmt(c));
            *f.p = c;
        }
    }
    int s = transitionSteps < 0 ? 0 : (transitionSteps > 8 ? 8 : transitionSteps);
    if (s != transitionSteps) {
        if (errors)
            errors->push_back("[Seasons] TransitionSteps = " + std::to_string(transitionSteps) +
                              " is out of range (0 .. 8), using " + std::to_string(s));
        transitionSteps = s;
    }
    if (int(daySource) > 2) {
        if (errors) errors->push_back("[Seasons] DaySource invalid, using playtime");
        daySource = DaySource::Playtime;
    }
}

bool SeasonConfig::load(const Ini& ini, std::vector<std::string>& errors) {
    setDefaults();
    size_t firstError = errors.size();

    // Region sections may use aliases ([Region.North]); map each present section to its region.
    std::string regionSection[kRegionCount];
    for (int r = 0; r < kRegionCount; ++r) regionSection[r] = std::string("Region.") + toString(Region(r));
    for (const std::string& sec : ini.sections()) {
        if (sec.compare(0, 7, "region.") != 0) continue;
        Region r;
        if (regionFromName(sec.substr(7), r)) {
            regionSection[int(r)] = sec;
        } else {
            errors.push_back("[" + sec + "] unknown region (valid: Temperate, NorthCold, SwampJungle, Aquatic, "
                             "DesertDry, Abyss), section ignored");
        }
    }
    auto sectionOf = [&](const std::string& canonical) {
        for (int r = 0; r < kRegionCount; ++r)
            if (canonical == std::string("Region.") + toString(Region(r))) return regionSection[r];
        return canonical;
    };

    for (const FloatField& f : floatFields(*this)) {
        std::string sec = sectionOf(f.section);
        if (!ini.has(sec, f.key)) continue;
        std::string raw = ini.get(sec, f.key);
        double v;
        if (parseNum(raw, v)) *f.p = float(v);
        else errors.push_back("[" + f.section + "] " + f.key + " = '" + raw + "' is not a number, using " + fmt(*f.p));
    }
    if (ini.has("Seasons", "TransitionSteps")) {
        std::string raw = ini.get("Seasons", "TransitionSteps");
        double v;
        if (!parseNum(raw, v)) {
            errors.push_back("[Seasons] TransitionSteps = '" + raw + "' is not a number, using " +
                             std::to_string(transitionSteps));
        } else {
            int n = int(std::lround(v));
            if (double(n) != v)
                errors.push_back("[Seasons] TransitionSteps = " + raw + " must be a whole number, using " +
                                 std::to_string(n));
            transitionSteps = n;
        }
    }
    if (ini.has("Seasons", "DaySource")) {
        std::string raw = ini.get("Seasons", "DaySource");
        if (!parseDaySource(raw, daySource))
            errors.push_back("[Seasons] DaySource = '" + raw + "' is not one of playtime, gameclock, manual; using " +
                             toString(daySource));
    }
    for (const BoolField& b : kBools) {
        if (!ini.has("Seasons", b.key)) continue;
        std::string raw = ini.get("Seasons", b.key);
        if (!parseBool(raw, this->*b.p))
            errors.push_back(std::string("[Seasons] ") + b.key + " = '" + raw + "' is not 0 or 1, using " +
                             (this->*b.p ? "1" : "0"));
    }
    for (int r = 0; r < kRegionCount; ++r) {
        const std::string& sec = regionSection[r];
        if (!ini.has(sec, "Enabled")) continue;
        std::string raw = ini.get(sec, "Enabled");
        if (!parseBool(raw, regions[r].enabled))
            errors.push_back(std::string("[Region.") + toString(Region(r)) + "] Enabled = '" + raw +
                             "' is not 0 or 1, using " + (regions[r].enabled ? "1" : "0"));
    }
    clamp(&errors);
    return errors.size() == firstError;
}

std::string SeasonConfig::save() const {
    std::string o;
    auto line = [&](const std::string& s) { o += s + "\n"; };
    auto kv = [&](const char* key, const std::string& v, const char* comment = nullptr) {
        std::string s = std::string(key) + " = " + v;
        if (comment) {
            if (s.size() < 28) s.append(28 - s.size(), ' ');
            s += " ; ";
            s += comment;
        }
        line(s);
    };
    auto b = [](bool v) { return std::string(v ? "1" : "0"); };

    line("; ============================================================================");
    line("; QuattroStagioni: calendario delle stagioni");
    line("; Si modifica anche dal menu in gioco (F5). Decimali con il punto (0.4), la");
    line("; virgola (0,4) è accettata. I valori fuori intervallo vengono corretti e");
    line("; segnalati nel file di log.");
    line("; ============================================================================");
    line("[Seasons]");
    line("; Durata di ogni stagione in giorni di gioco (0.5 .. 120). Il ciclo è:");
    line("; Primavera -> Estate -> Fine estate (secca) -> Autunno -> Inverno -> Primavera.");
    line("; L'estate è il gioco originale, senza modifiche.");
    for (int i = 0; i < kPhaseCount; ++i) kv((std::string(kPhaseKey[i]) + "Days").c_str(), fmt(phaseDays[i]));
    line("; Parte finale di ogni stagione dedicata al passaggio alla successiva (0 .. 1).");
    line("; 0.4 = nell'ultimo 40% della stagione il paesaggio cambia verso la prossima.");
    kv("TransitionFraction", fmt(transitionFraction));
    line("; Stadi intermedi di ogni passaggio (0 .. 8; 0 = cambio netto). Più stadi danno");
    line("; un cambio più graduale ma richiedono più spazio su disco per i pacchetti.");
    kv("TransitionSteps", std::to_string(transitionSteps));
    line("; Varietà: ogni pianta anticipa o ritarda il proprio cambio fino a questi giorni");
    line("; (0 .. 10), così un bosco non cambia colore tutto insieme.");
    kv("AssetJitterDays", fmt(assetJitterDays));
    line("; Come avanza il calendario:");
    line(";   playtime  = tempo di gioco reale (vedi RealMinutesPerDay)");
    line(";   gameclock = segue l'orologio del gioco (quando agganciato)");
    line(";   manual    = fermo, il giorno si sceglie dal menu F5");
    kv("DaySource", toString(daySource));
    line("; Minuti reali per un giorno del calendario, con DaySource = playtime (1 .. 1440).");
    line("; Un giorno del gioco dura circa 60-120 minuti reali: 90 tiene il calendario");
    line("; vicino all'orologio del gioco.");
    kv("RealMinutesPerDay", fmt(realMinutesPerDay));
    line("; Giorno di partenza (0 = primo giorno di primavera). Con la durata predefinita:");
    line("; primavera 0, estate 8, fine estate 16, autunno 21, inverno 29; anno di 38 giorni.");
    kv("StartDay", fmt(startDay));
    line("; Interruttori (1 = attivo, 0 = spento).");
    kv("WeatherControl", b(weatherControl), "meteo coerente con la stagione");
    kv("SnowAccumulation", b(snowAccumulation), "neve che si accumula e si scioglie");
    kv("LightControl", b(lightControl), "luce e altezza del sole per stagione");
    kv("DayLengthControl", b(dayLengthControl), "giornate più corte in inverno");
    kv("Particles", b(particles), "foglie che cadono, fiato visibile");
    line("; Velocità della neve, frazione di copertura per ora di gioco (0 .. 5).");
    kv("SnowAccumPerHour", fmt(snowAccumPerHour), "quando nevica sotto zero");
    kv("SnowMeltPerHour", fmt(snowMeltPerHour), "a +3 gradi (più veloce se fa più caldo)");
    line("; Le temperature non scendono mai sotto i -30 gradi (il gioco infligge danni da freddo).");
    line("; Latitudine virtuale in gradi (0 .. 65): più alta = inverni più bui, sole più basso.");
    kv("VirtualLatitude", fmt(virtualLatitudeDeg));
    line("; Finestra del giorno del gioco originale (l'estate): l'orologio del gioco ha una");
    line("; velocità per il giorno (03:00-18:59) e una per la notte. In inverno la mod accelera");
    line("; il giorno e rallenta la notte, così le ore di luce si accorciano (mai sotto 4).");
    kv("VanillaSunrise", fmt(vanillaSunriseHour));
    kv("VanillaSunset", fmt(vanillaSunsetHour));
    line("; Sotto questa temperatura (gradi C) il fiato diventa visibile.");
    kv("BreathThreshold", fmt(breathThresholdC));
    line("");
    line("; ============================================================================");
    line("; Regioni");
    line(";   Enabled         0 = la regione resta sempre come in estate (gioco originale)");
    line(";   DayOffset       sposta il calendario della regione in giorni (negativo = in anticipo)");
    line(";   <Stagione>Scale allunga (>1) o accorcia (<1) una stagione (0.1 .. 5); l'anno resta");
    line(";                   della stessa lunghezza, le altre stagioni si adattano");
    line(";   MeanTemp        temperatura media annua (gradi C)");
    line(";   TempAmplitude   differenza tra la media e il cuore dell'inverno o dell'estate");
    line(";   DailyAmplitude  metà dell'escursione tra giorno e notte");
    line(";   Humidity        0 secco .. 1 molto umido (più pioggia e nuvole)");
    line(";   Leaves          intensità delle foglie che cadono in autunno (0 .. 2)");
    line("; ============================================================================");
    static const char* kRegionNote[kRegionCount] = {
        "Zone temperate: ciclo completo delle stagioni.",
        "Nord freddo: inverno anticipato e più lungo, estate breve, molta neve.",
        "Paludi e giungla: sempreverdi, umide, non gela mai.",
        "Piante acquatiche: variazioni leggere.",
        "Deserto: escluso per scelta (sempre come il gioco originale).",
        "Abisso: escluso per scelta (sempre come il gioco originale).",
    };
    for (int r = 0; r < kRegionCount; ++r) {
        const RegionConfig& rc = regions[r];
        line("");
        line(std::string("[Region.") + toString(Region(r)) + "]");
        line(std::string("; ") + kRegionNote[r]);
        kv("Enabled", b(rc.enabled));
        kv("DayOffset", fmt(rc.dayOffset));
        for (int i = 0; i < kPhaseCount; ++i) kv((std::string(kPhaseKey[i]) + "Scale").c_str(), fmt(rc.phaseScale[i]));
        kv("MeanTemp", fmt(rc.meanTempC));
        kv("TempAmplitude", fmt(rc.ampTempC));
        kv("DailyAmplitude", fmt(rc.diurnalTempC));
        kv("Humidity", fmt(rc.humidity));
        kv("Leaves", fmt(rc.leaves));
    }
    return o;
}

std::string defaultIniText() {
    SeasonConfig c;
    c.setDefaults();
    return c.save();
}

}  // namespace qs::season
