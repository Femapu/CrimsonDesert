// QuattroStagioni - seasonal sunrise and sunset (see daylength.h).
#include "daylength.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <sstream>

#include "targets.h"

namespace qs::apply {

namespace {

float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

float wrap24(float h) {
    if (!std::isfinite(h)) return 0.0f;
    float w = std::fmod(h, 24.0f);
    if (w < 0) w += 24.0f;
    if (w >= 24.0f) w = 0.0f;
    return w;
}

int wrapBin(int i) { return ((i % kSunBins) + kSunBins) % kSunBins; }

std::string fmtHex(uint32_t v) {
    char b[16];
    snprintf(b, sizeof b, "0x%08X", v);
    return b;
}

// Keeps a valid day: sunrise in [0.5, 23], sunset in [sunrise + 0.5, 23.5].
SunTimes sane(float sr, float ss) {
    SunTimes t;
    t.sunrise = clampf(std::isfinite(sr) ? sr : 5.5f, 0.5f, 23.0f);
    t.sunset = clampf(std::isfinite(ss) ? ss : 19.5f, t.sunrise + 0.5f, 23.5f);
    return t;
}

SunTimes seasonal(const DayLengthConfig& c, float shiftR, float shiftS) {
    float sr = c.gameSunrise + shiftR, ss = c.gameSunset + shiftS;
    const float minDay = clampf(c.minDaylightHours, 0.5f, 23.0f);
    if (ss - sr < minDay) {  // cut shared by both ends
        const float d = minDay - (ss - sr);
        sr -= d * 0.5f;
        ss += d * 0.5f;
    }
    return sane(sr, ss);
}

}  // namespace

DayLengthConfig::DayLengthConfig() {
    // S U L A W: summer is the game's own day, winter about 3.5 hours shorter.
    const float r[season::kPhaseCount] = {0.5f, 0.0f, 0.3f, 1.0f, 1.75f};
    const float s[season::kPhaseCount] = {-0.5f, 0.0f, -0.3f, -1.0f, -1.75f};
    for (int i = 0; i < season::kPhaseCount; ++i) {
        sunriseShift.v[i] = r[i];
        sunsetShift.v[i] = s[i];
    }
}

void DayLengthConfig::load(const Ini& ini, std::vector<std::string>& warnings) {
    const char* S = "DayLength";
    auto num = [&](const char* key, float& out, float lo, float hi) {
        if (!ini.has(S, key)) return;
        double d;
        if (parseDouble(ini.get(S, key), d) && std::isfinite(d)) out = clampf(float(d), lo, hi);
        else warnings.push_back(std::string("[DayLength] ") + key + "=" + ini.get(S, key) + " not a number: default kept");
    };
    if (ini.has(S, "Enabled")) enabled = ini.getInt(S, "Enabled", 1) != 0;
    num("GameSunrise", gameSunrise, 0.5f, 12.0f);
    num("GameSunset", gameSunset, 12.0f, 23.5f);
    num("MinDaylightHours", minDaylightHours, 0.5f, 23.0f);
    num("RampPerHour", rampPerHour, 0.05f, 24.0f);
    num("SaveIntervalSec", saveIntervalSec, 10.0f, 3600.0f);
    num("LogIntervalSec", logIntervalSec, 0.0f, 3600.0f);
    if (ini.has(S, "MaxGapBins")) maxGapBins = int(std::clamp<long long>(ini.getInt(S, "MaxGapBins", 2), 0, 8));
    if (ini.has(S, "MinSamples")) minSamples = int(std::clamp<long long>(ini.getInt(S, "MinSamples", 4), 1, 1000));
    if (ini.has(S, "StartDelayMs")) startDelayMs = uint32_t(std::clamp<long long>(ini.getInt(S, "StartDelayMs", 25000), 0, 600000));
    loadPhaseValues(ini, S, "SunriseShift", sunriseShift, -6.0f, 6.0f, warnings);
    loadPhaseValues(ini, S, "SunsetShift", sunsetShift, -6.0f, 6.0f, warnings);
}

std::string dayLengthIniTemplate() {
    DayLengthConfig c;
    char b[2048];
    snprintf(b, sizeof b,
             "[DayLength]\n"
             "; Durata del giorno: alba e tramonto seguono la stagione (giorni lunghi d'estate, corti d'inverno)\n"
             "; SENZA cambiare la velocità dell'orologio del gioco. La mod sposta solo il sole: all'ora h del gioco\n"
             "; mostra il sole che il gioco avrebbe a un'altra ora. Serve il campo del sole in targets.ini\n"
             "; (sezione [daylength], guida docs/dev/durata_giorno.md); finché manca, il log riporta solo gli orari.\n"
             "; Si spegne anche dal menu F5 (\"Durata del giorno\"). Tutte le chiavi sono facoltative.\n"
             "Enabled=%d\n"
             "; alba e tramonto visibili del gioco senza mod (ore di gioco, da verificare in gioco)\n"
             "GameSunrise=%g\n"
             "GameSunset=%g\n"
             "; spostamento in ore per fase (primavera, estate, fine estate, autunno, inverno):\n"
             "; alba positiva = più tardi, tramonto negativo = più presto\n"
             "SunriseShift=%s\n"
             "SunsetShift=%s\n"
             "; durata minima del giorno stagionale in ore\n"
             "MinDaylightHours=%g\n"
             "; variazione massima di alba e tramonto per ora di gioco (passaggi graduali, anche spegnendo dal menu)\n"
             "RampPerHour=%g\n"
             "; tabella del sole imparata dal gioco: buchi colmati (in fasce da 15 minuti) e letture minime per fascia\n"
             "MaxGapBins=%d\n"
             "MinSamples=%d\n"
             "; ogni quanti secondi reali salvare la tabella e scrivere lo stato nel log (0 = mai)\n"
             "SaveIntervalSec=%g\n"
             "LogIntervalSec=%g\n"
             "StartDelayMs=%u\n",
             c.enabled ? 1 : 0, double(c.gameSunrise), double(c.gameSunset), formatPhaseValues(c.sunriseShift).c_str(),
             formatPhaseValues(c.sunsetShift).c_str(), double(c.minDaylightHours), double(c.rampPerHour), c.maxGapBins,
             c.minSamples, double(c.saveIntervalSec), double(c.logIntervalSec), c.startDelayMs);
    return b;
}

const char* toString(SunKind k) { return k == SunKind::Hour ? "hour" : "sundir"; }

void DayLengthTarget::load(const Ini& ini, std::vector<std::string>& warnings) {
    const char* S = "daylength";
    if (ini.has(S, "Enabled")) enabled = ini.getInt(S, "Enabled", 0) != 0;
    if (ini.has(S, "Kind")) {
        std::string k = ini.get(S, "Kind");
        for (char& ch : k) ch = char(std::tolower(static_cast<unsigned char>(ch)));
        if (k == "sundir") kind = SunKind::SunDir;
        else if (k == "hour") kind = SunKind::Hour;
        else warnings.push_back("[daylength] Kind=" + ini.get(S, "Kind") + " unknown (sundir or hour): sundir kept");
    }
    std::string err;
    LayerRef r;
    if (!parseLayerRef(ini.get(S, "Field"), r, &err)) {
        warnings.push_back("[daylength] Field: " + err + " (target off)");
        field = LayerRef();
        enabled = false;
        return;
    }
    field = r;
    if (kind == SunKind::Hour && field.valid && field.count != 1) {
        warnings.push_back("[daylength] Kind=hour uses one float: count set to 1");
        field.count = 1;
    }
}

SunTimes gameSunTimes(const DayLengthConfig& c) { return sane(c.gameSunrise, c.gameSunset); }

SunTimes seasonalSunTimes(const DayLengthConfig& c, season::Phase phase, float progress, float tf) {
    return seasonal(c, phaseValue(c.sunriseShift, phase, progress, tf), phaseValue(c.sunsetShift, phase, progress, tf));
}

SunTimes seasonalSunTimes(const DayLengthConfig& c, const season::VisualStage& st) {
    return seasonal(c, stageValue(c.sunriseShift, st), stageValue(c.sunsetShift, st));
}

float remapHour(float h, const SunTimes& s, const SunTimes& g) {
    h = wrap24(h);
    const bool ok = std::isfinite(s.sunrise) && std::isfinite(s.sunset) && std::isfinite(g.sunrise) && std::isfinite(g.sunset) &&
                    s.sunset > s.sunrise && g.sunset > g.sunrise && s.sunset - s.sunrise < 24.0f && g.sunset - g.sunrise < 24.0f;
    if (!ok) return h;
    const float sr = wrap24(s.sunrise), ss = sr + (s.sunset - s.sunrise);
    const float gr = g.sunrise, gs = g.sunset;
    // position of h on the seasonal cycle that starts at sunrise
    float x = h < sr ? h + 24.0f : h;
    float out;
    if (x <= ss) out = gr + (x - sr) * (gs - gr) / (ss - sr);
    else out = gs + (x - ss) * (gr + 24.0f - gs) / (sr + 24.0f - ss);
    return wrap24(out);
}

float hourDelta(float a, float b) {
    float d = std::fmod(a - b, 24.0f);
    if (d > 12.0f) d -= 24.0f;
    if (d < -12.0f) d += 24.0f;
    return d;
}

std::string formatHour(float h) {
    int m = int(std::lround(double(wrap24(h)) * 60.0)) % (24 * 60);
    char b[16];
    snprintf(b, sizeof b, "%d:%02d", m / 60, m % 60);
    return b;
}

// ---- SunTable -----------------------------------------------------------------------------------
void SunTable::reset(int count) {
    count_ = std::max(1, std::min(kSunMaxFloats, count));
    for (int i = 0; i < kSunBins; ++i) {
        n_[i] = 0;
        for (float& v : v_[i]) v = 0;
    }
}

int SunTable::binOf(float hour) { return std::min(kSunBins - 1, int(wrap24(hour) * (kSunBins / 24.0f))); }

bool SunTable::record(float hour, const float* v, int capSamples) {
    if (!std::isfinite(hour) || !v) return false;
    for (int k = 0; k < count_; ++k)
        if (!std::isfinite(v[k])) return false;
    const int b = binOf(hour);
    const uint32_t cap = uint32_t(std::max(1, capSamples));
    const float w = n_[b] < cap ? 1.0f / float(n_[b] + 1) : 1.0f / float(cap);
    for (int k = 0; k < count_; ++k) v_[b][k] += (v[k] - v_[b][k]) * w;
    if (n_[b] < cap) ++n_[b];
    return true;
}

bool SunTable::lookup(float hour, float* out, int minSamples, int maxGap) const {
    if (!out || !std::isfinite(hour)) return false;
    const uint32_t need = uint32_t(std::max(1, minSamples));
    maxGap = std::max(0, maxGap);
    const float x = wrap24(hour) * (kSunBins / 24.0f) - 0.5f;  // in bin-centre units
    const int i0 = int(std::floor(x));
    int dl = -1, dr = -1;
    for (int k = 0; k <= maxGap && dl < 0; ++k)
        if (n_[wrapBin(i0 - k)] >= need) dl = k;
    for (int k = 0; k <= maxGap && dr < 0; ++k)
        if (n_[wrapBin(i0 + 1 + k)] >= need) dr = k;
    if (dl < 0 || dr < 0 || dl + dr > maxGap) return false;
    const int L = i0 - dl, R = i0 + 1 + dr;
    const float t = (x - float(L)) / float(R - L);
    const float* a = v_[wrapBin(L)];
    const float* b = v_[wrapBin(R)];
    for (int k = 0; k < count_; ++k) out[k] = a[k] + (b[k] - a[k]) * t;
    return true;
}

int SunTable::known(int minSamples) const {
    int n = 0;
    for (int i = 0; i < kSunBins; ++i) n += n_[i] >= uint32_t(std::max(1, minSamples)) ? 1 : 0;
    return n;
}

std::string SunTable::serialize(uint32_t exeStamp, const std::string& field) const {
    std::string s =
        "; QuattroStagioni: tabella del sole imparata dal gioco su questo PC (dato locale, non distribuire).\n"
        "; Si ricostruisce da sola giocando; cancellarla è sicuro. Bxx = fascia di 15 minuti: letture | valori.\n"
        "[SunTable]\nVersion=1\nExe=" + fmtHex(exeStamp) + "\nField=" + field + "\nCount=" + std::to_string(count_) + "\n[Bins]\n";
    for (int i = 0; i < kSunBins; ++i) {
        if (!n_[i]) continue;
        char b[160];
        int p = snprintf(b, sizeof b, "B%02d=%u |", i, n_[i]);
        for (int k = 0; k < count_ && p > 0 && p < int(sizeof b); ++k) p += snprintf(b + p, sizeof b - size_t(p), " %.9g", double(v_[i][k]));
        s += b;
        s += "\n";
    }
    return s;
}

bool SunTable::parse(const std::string& text, uint32_t exeStamp, const std::string& field, std::string* err) {
    auto fail = [&](const std::string& why) {
        if (err) *err = why;
        return false;
    };
    Ini ini;
    ini.parse(text);
    if (ini.getInt("SunTable", "Version", 0) != 1) return fail("not a sun table (Version)");
    uint64_t stamp = 0;
    if (!parseU64(ini.get("SunTable", "Exe"), stamp) || uint32_t(stamp) != exeStamp) return fail("table of another exe build");
    if (ini.get("SunTable", "Field") != field) return fail("table of another field");
    const long long cnt = ini.getInt("SunTable", "Count", 0);
    if (cnt < 1 || cnt > kSunMaxFloats) return fail("bad Count");
    SunTable t{int(cnt)};
    for (int i = 0; i < kSunBins; ++i) {
        char key[8];
        snprintf(key, sizeof key, "B%02d", i);
        if (!ini.has("Bins", key)) continue;
        const std::string line = ini.get("Bins", key);
        const size_t bar = line.find('|');
        double n = 0;
        if (bar == std::string::npos || !parseDouble(line.substr(0, bar), n) || n < 1 || n > 1e6)
            return fail(std::string("bad bin ") + key);
        std::stringstream ss(line.substr(bar + 1));
        std::string tok;
        int k = 0;
        while (ss >> tok) {
            double d;
            if (k >= t.count_ || !parseDouble(tok, d)) return fail(std::string("bad bin ") + key);
            t.v_[i][k++] = float(d);
        }
        if (k != t.count_) return fail(std::string("bad bin ") + key);
        t.n_[i] = uint32_t(n);
    }
    *this = t;
    return true;
}

}  // namespace qs::apply
