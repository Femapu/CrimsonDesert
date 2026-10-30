// QuattroStagioni - season applier controller (see applier.h).
#include "applier.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace qs::apply {
namespace {

// Expected vtables are kept XOR-ed so a heap scan for a vtable never finds our own bookkeeping.
constexpr uint64_t kVtXor = 0xA5C3A5C3A5C3A5C3ull;

uint32_t mix32(uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ull;
    x ^= x >> 33;
    return uint32_t(x);
}

double unit(uint32_t seed, long long slot, uint32_t k) {
    uint32_t h = mix32(uint64_t(seed) * 0x9E3779B97F4A7C15ull ^ uint64_t(slot) * 0xD6E8FEB86659FD93ull ^ (uint64_t(k) << 56));
    return double(h >> 8) / double(1u << 24);
}

float weightOf(const season::WeatherWeights& w, int i) {
    const float v[kWeatherCount] = {w.sunny, w.cloudy, w.rainy, w.storm, w.snowy, w.windy};
    return v[i] > 0 ? v[i] : 0.0f;
}

#if defined(__GNUC__)
std::string fmt(const char* f, ...) __attribute__((format(gnu_printf, 1, 2)));
#endif
std::string fmt(const char* f, ...) {
    char b[512];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(b, sizeof b, f, ap);
    va_end(ap);
    return b;
}

bool isBitField(const TargetSpec& t) { return t.bit >= 0 || t.kind == FieldKind::Flags32; }

uint32_t rawBits(FieldKind k, const uint8_t* p) {
    if (k == FieldKind::U8) return p[0];
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

double autoTol(const TargetSpec& t, double v) {
    if (t.tolerance >= 0) return t.tolerance;
    if (t.kind == FieldKind::F32 || t.kind == FieldKind::F64) return 1e-5 * std::max(1.0, std::fabs(v));
    return 0.5;
}

}  // namespace

// ------------------------------------------------------------------------------- scheduler
void WeatherScheduler::configure(double holdHours, double blendHours, double persistence, uint32_t seed) {
    hold_ = holdHours > 0.05 ? holdHours : 0.05;
    blend_ = std::max(0.0, std::min(blendHours, hold_));
    persistence_ = std::max(0.0, std::min(persistence, 0.95));
    seed_ = seed;
    have_ = false;
}

Weather WeatherScheduler::choose(long long slot, const season::WeatherWeights& w, int prev) const {
    float total = 0;
    for (int i = 0; i < kWeatherCount; ++i) total += weightOf(w, i);
    if (total <= 0) return Weather::Sunny;
    if (prev >= 0 && weightOf(w, prev) / total > 0.02f && unit(seed_, slot, 1) < persistence_) return Weather(prev);
    double u = unit(seed_, slot, 2) * total, acc = 0;
    for (int i = 0; i < kWeatherCount; ++i) {
        acc += weightOf(w, i);
        if (u < acc && weightOf(w, i) > 0) return Weather(i);
    }
    for (int i = kWeatherCount - 1; i >= 0; --i)
        if (weightOf(w, i) > 0) return Weather(i);
    return Weather::Sunny;
}

const WeatherMix& WeatherScheduler::update(double hours, const season::WeatherWeights& w) {
    long long slot = (long long)std::floor(hours / hold_);
    if (!have_ || slot < mix_.slot || slot > mix_.slot + 64) {
        // (re)anchor: previous slot drawn without history, then this one
        Weather p = choose(slot - 1, w, -1);
        mix_.previous = p;
        mix_.current = choose(slot, w, int(p));
        have_ = true;
    } else {
        for (long long k = mix_.slot + 1; k <= slot; ++k) {
            mix_.previous = mix_.current;
            mix_.current = choose(k, w, int(mix_.previous));
        }
    }
    // the current weather became impossible (season moved on, e.g. snow above zero): redraw now
    if (weightOf(w, int(mix_.current)) <= 0) mix_.current = choose(slot, w, -1);
    mix_.slot = slot;
    mix_.slotStartHours = double(slot) * hold_;
    double into = hours - mix_.slotStartHours;
    mix_.blend = blend_ <= 0 || mix_.previous == mix_.current ? 1.0f : float(std::min(1.0, std::max(0.0, into / blend_)));
    for (int i = 0; i < kWeatherCount; ++i) mix_.mix[i] = 0;
    mix_.mix[int(mix_.previous)] += 1.0f - mix_.blend;
    mix_.mix[int(mix_.current)] += mix_.blend;
    return mix_;
}

// ------------------------------------------------------------------------------- clock
void ClockTracker::reset() { *this = ClockTracker(); }

void ClockTracker::sample(bool haveDay, double dayCounter, bool haveHour, double hourOfDay) {
    if (!haveHour && !haveDay) {
        known_ = false;
        havePrev_ = false;
        return;
    }
    double h = haveHour ? hourOfDay : 0.0;
    double delta = 0;
    if (haveDay) {
        double total = dayCounter * 24.0 + h;
        if (havePrev_ && haveDay_) {
            delta = total - prevTotal_;
            if (delta < 0 || delta > 240) delta = 0;  // load / reset / nonsense: resync
        }
        prevTotal_ = total;
    } else if (havePrev_) {
        delta = h - prevHour_;
        if (delta < -12) delta += 24;  // midnight
        if (delta < 0) delta = 0;      // clock set back (load)
    }
    if (havePrev_) pending_ += delta;
    prevHour_ = h;
    hour_ = h;
    day_ = dayCounter;
    haveDay_ = haveDay;
    havePrev_ = true;
    known_ = haveHour || haveDay;
}

bool ClockTracker::take(double& day, double& hoursElapsed) {
    if (!known_) return false;
    day = this->day();
    hoursElapsed = pending_;
    pending_ = 0;
    return true;
}

// ------------------------------------------------------------------------------- mapping
bool featureOn(Feature f, const TargetsConfig& cfg, const season::SeasonState& s, const season::SeasonConfig& sc) {
    if (!cfg.features[int(f)]) return false;
    const bool regionOk = cfg.region >= 0 && cfg.region < season::kRegionCount && s.regionActive[cfg.region];
    switch (f) {
        case Feature::Weather: return sc.weatherControl && regionOk;
        case Feature::Snow: return sc.snowAccumulation && regionOk;
        case Feature::Light: return sc.lightControl;
        case Feature::DayLength: return sc.dayLengthControl;
        case Feature::Particles: return sc.particles && regionOk;
        case Feature::Clock: return true;
        default: return false;
    }
}

bool seasonValue(const TargetSpec& t, const TargetsConfig& cfg, const season::SeasonState& s, const season::SeasonConfig& sc,
                 const WeatherMix& wm, double hourOfDay, double& out) {
    const int r = std::max(0, std::min(cfg.region, season::kRegionCount - 1));
    const season::WeatherWeights& ww = s.weather[r];
    switch (t.semantic) {
        case Semantic::Hour:
        case Semantic::Minute:
        case Semantic::DayCounter: return false;  // read only
        case Semantic::TimeScaleDay: out = s.dayScale; return true;
        case Semantic::TimeScaleNight: out = s.nightScale; return true;
        case Semantic::TimeScaleCurrent: {
            bool day = hourOfDay >= sc.vanillaSunriseHour && hourOfDay < sc.vanillaSunsetHour;
            out = day ? s.dayScale : s.nightScale;
            return true;
        }
        case Semantic::WeatherManualFlag: out = 1; return true;
        case Semantic::WeatherStateFlags: out = double(cfg.flag[int(wm.current)]); return true;
        case Semantic::WeatherIndex: out = double(cfg.index[int(wm.current)]); return true;
        case Semantic::WeatherWeight: {
            float total = 0;
            for (int i = 0; i < kWeatherCount; ++i) total += weightOf(ww, i);
            out = total > 0 ? weightOf(ww, int(t.weather)) / total : 0.0;
            return true;
        }
        case Semantic::WeatherMix: out = wm.mix[int(t.weather)]; return true;
        case Semantic::Precipitation: out = wm.precipitation(); return true;
        case Semantic::RainAmount: out = wm.rain(); return true;
        case Semantic::SnowRate: out = wm.mix[int(Weather::Snowy)]; return true;
        case Semantic::TemperatureOffset:
            out = double(s.dailyMeanTempC[r]) - double(sc.regions[r].meanTempC + sc.regions[r].ampTempC);
            return true;
        case Semantic::Temperature: out = s.temperatureC[r]; return true;
        case Semantic::SnowAmount: out = s.snowCover[r]; return true;
        case Semantic::SunElevationOffset: out = s.sunElevationOffsetDeg; return true;
        case Semantic::LightTintR: out = s.lightTint[0]; return true;
        case Semantic::LightTintG: out = s.lightTint[1]; return true;
        case Semantic::LightTintB: out = s.lightTint[2]; return true;
        case Semantic::Saturation: out = s.saturation; return true;
        case Semantic::LeavesParticles: out = s.leaves[r]; return true;
        case Semantic::BreathVisible: out = s.breathVisible[r] ? 1.0 : 0.0; return true;
        default: return false;
    }
}

void encodeField(FieldKind k, double v, uint8_t out[8]) {
    std::memset(out, 0, 8);
    auto sat = [](double x, double lo, double hi) { return x != x ? 0.0 : x < lo ? lo : x > hi ? hi : x; };
    switch (k) {
        case FieldKind::F32: {
            float f = float(v);
            std::memcpy(out, &f, 4);
            break;
        }
        case FieldKind::F64: std::memcpy(out, &v, 8); break;
        case FieldKind::I32: {
            int32_t i = int32_t(std::llround(sat(v, -2147483648.0, 2147483647.0)));
            std::memcpy(out, &i, 4);
            break;
        }
        case FieldKind::U32:
        case FieldKind::Flags32: {
            uint32_t u = uint32_t(std::llround(sat(v, 0.0, 4294967295.0)));
            std::memcpy(out, &u, 4);
            break;
        }
        case FieldKind::U8: out[0] = uint8_t(std::llround(sat(v, 0.0, 255.0))); break;
    }
}

double decodeField(FieldKind k, const uint8_t* in) {
    switch (k) {
        case FieldKind::F32: {
            float f;
            std::memcpy(&f, in, 4);
            return f;
        }
        case FieldKind::F64: {
            double d;
            std::memcpy(&d, in, 8);
            return d;
        }
        case FieldKind::I32: {
            int32_t i;
            std::memcpy(&i, in, 4);
            return i;
        }
        case FieldKind::U32:
        case FieldKind::Flags32: {
            uint32_t u;
            std::memcpy(&u, in, 4);
            return u;
        }
        case FieldKind::U8: return in[0];
    }
    return 0;
}

const char* toString(TargetState s) {
    switch (s) {
        case TargetState::Disabled: return "disabled";
        case TargetState::Off: return "off";
        case TargetState::Waiting: return "waiting";
        case TargetState::Active: return "active";
        case TargetState::Error: return "error";
    }
    return "?";
}

const char* toString(FeatureState s) {
    switch (s) {
        case FeatureState::Inactive: return "inactive";
        case FeatureState::NotFound: return "not found";
        case FeatureState::Active: return "active";
        case FeatureState::Error: return "error";
    }
    return "?";
}

// ------------------------------------------------------------------------------- controller
void Applier::configure(const TargetsConfig& cfg) {
    cfg_ = cfg;
    trt_.assign(cfg_.targets.size(), TargetRt());
    ort_.assign(cfg_.objects.size(), ObjectRt());
    sched_.configure(cfg_.holdHours, cfg_.blendHours, cfg_.persistence, cfg_.seed);
    clock_.reset();
    for (size_t i = 0; i < trt_.size(); ++i) {
        trt_[i].state = cfg_.targets[i].enabled ? TargetState::Waiting : TargetState::Disabled;
        if (cfg_.targets[i].enabled) ort_[size_t(cfg_.targets[i].objectIndex)].needed = cfg_.features[int(cfg_.targets[i].feature)];
    }
}

void Applier::log(const std::string& s) {
    if (log_.size() < 512) log_.push_back(s);
}

std::vector<std::string> Applier::takeLog() {
    std::vector<std::string> out;
    out.swap(log_);
    return out;
}

void Applier::bindObject(int obj, const std::vector<uint64_t>& addrs, uint64_t expectVtable) {
    if (obj < 0 || size_t(obj) >= ort_.size() || addrs.empty()) return;
    ObjectRt& o = ort_[size_t(obj)];
    o.addrs = addrs;
    o.vtableX = expectVtable ? (expectVtable ^ kVtXor) : 0;
    o.bound = true;
    for (size_t i = 0; i < trt_.size(); ++i)
        if (cfg_.targets[i].objectIndex == obj) trt_[i].inst.assign(addrs.size(), FieldRt());
    log(fmt("object %s bound: %zu instance(s), first at 0x%llX%s", cfg_.objects[size_t(obj)].name.c_str(), addrs.size(),
            (unsigned long long)addrs[0], expectVtable ? " (vtable guarded)" : ""));
}

void Applier::unbindObject(int obj) {
    if (obj < 0 || size_t(obj) >= ort_.size()) return;
    ObjectRt& o = ort_[size_t(obj)];
    if (!o.bound) return;
    o.bound = false;
    o.addrs.clear();
    o.vtableX = 0;
    for (size_t i = 0; i < trt_.size(); ++i)
        if (cfg_.targets[i].objectIndex == obj) {
            trt_[i].inst.clear();
            if (trt_[i].state == TargetState::Active) trt_[i].state = TargetState::Waiting;
        }
}

bool Applier::objectBound(int obj) const { return obj >= 0 && size_t(obj) < ort_.size() && ort_[size_t(obj)].bound; }

std::vector<uint64_t> Applier::objectInstances(int obj) const {
    return objectBound(obj) ? ort_[size_t(obj)].addrs : std::vector<uint64_t>();
}

bool Applier::objectNeeded(int obj) const { return obj >= 0 && size_t(obj) < ort_.size() && ort_[size_t(obj)].needed; }

bool Applier::restoreField(const TargetSpec& t, uint64_t addr, const FieldRt& f, Memory& mem) {
    if (!f.attached || !t.restore || t.mode == Mode::Read) return true;
    uint8_t cur[8] = {}, out[8] = {};
    const size_t n = fieldBytes(t.kind);
    if (isBitField(t)) {
        if (!mem.read(addr, cur, n)) return false;
        uint32_t raw = rawBits(t.kind, cur);
        uint32_t m = t.bit >= 0 ? (1u << t.bit) : t.mask;
        uint32_t v = (raw & ~m) | (f.originalBits & m);
        if (v == raw) return true;
        encodeField(t.kind, double(v), out);
    } else {
        encodeField(t.kind, f.original, out);
    }
    std::string why;
    bool ok = mem.write(addr, out, n, &why);
    if (ok) ++writes_;
    else log(fmt("target %s: restore at 0x%llX failed: %s", t.name.c_str(), (unsigned long long)addr, why.c_str()));
    return ok;
}

void Applier::detach(size_t ti, Memory* mem, bool restore) {
    const TargetSpec& t = cfg_.targets[ti];
    TargetRt& rt = trt_[ti];
    const ObjectRt& o = ort_[size_t(t.objectIndex)];
    for (size_t k = 0; k < rt.inst.size(); ++k) {
        if (restore && mem && o.bound && k < o.addrs.size()) restoreField(t, o.addrs[k] + t.offset, rt.inst[k], *mem);
        rt.inst[k] = FieldRt();
    }
}

void Applier::fail(size_t ti, Memory& mem, const std::string& why) {
    TargetRt& rt = trt_[ti];
    detach(ti, &mem, true);
    rt.state = TargetState::Error;
    rt.error = why;
    log(fmt("target %s DISABLED: %s (original restored)", cfg_.targets[ti].name.c_str(), why.c_str()));
}

bool Applier::weatherDriverActive(size_t except) const {
    for (size_t i = 0; i < trt_.size(); ++i) {
        const TargetSpec& t = cfg_.targets[i];
        if (i == except || t.feature != Feature::Weather || t.mode == Mode::Read) continue;
        if (t.semantic == Semantic::WeatherManualFlag) continue;
        if (trt_[i].state == TargetState::Active) return true;
    }
    return false;
}

void Applier::applyTarget(size_t ti, const season::SeasonState& s, const season::SeasonConfig& sc, double dt, Memory& mem) {
    const TargetSpec& t = cfg_.targets[ti];
    TargetRt& rt = trt_[ti];
    if (!t.enabled) {
        rt.state = TargetState::Disabled;
        return;
    }
    if (rt.state == TargetState::Error) return;
    ObjectRt& o = ort_[size_t(t.objectIndex)];
    if (!featureOn(t.feature, cfg_, s, sc)) {
        if (rt.state == TargetState::Active) log(fmt("target %s: feature %s off, original restored", t.name.c_str(), toString(t.feature)));
        detach(ti, &mem, true);
        rt.state = TargetState::Off;
        return;
    }
    if (!o.bound) {
        rt.state = TargetState::Waiting;
        return;
    }
    const size_t n = fieldBytes(t.kind);
    const uint32_t limit = cfg_.anomalyLimit;

    // ---- read-only (clock) targets
    if (t.mode == Mode::Read) {
        uint8_t cur[8] = {};
        if (!mem.read(o.addrs[0] + t.offset, cur, n)) {
            ++rt.anomalies;
            if (++rt.readStreak >= limit) fail(ti, mem, "field unreadable");
            return;
        }
        double raw = isBitField(t) ? double((rawBits(t.kind, cur) >> (t.bit >= 0 ? t.bit : 0)) & (t.bit >= 0 ? 1u : 0xFFFFFFFFu))
                                   : decodeField(t.kind, cur);
        double v = (raw - t.bias) / t.scale;
        rt.current = raw;
        bool plausible = std::isfinite(v) && (std::isnan(t.minV) || raw >= t.minV) && (std::isnan(t.maxV) || raw <= t.maxV);
        if (t.semantic == Semantic::Hour) plausible = plausible && v >= 0 && v <= 24.001;
        if (t.semantic == Semantic::Minute) plausible = plausible && v >= 0 && v < 60.001;
        if (t.semantic == Semantic::DayCounter) plausible = plausible && v >= 0 && v < 1e7;
        if (!plausible) {
            ++rt.anomalies;
            if (++rt.readStreak >= limit) fail(ti, mem, fmt("implausible value %g", raw));
            return;
        }
        rt.readStreak = 0;
        rt.desired = v;
        if (rt.state != TargetState::Active) log(fmt("target %s: reading %s = %g", t.name.c_str(), toString(t.semantic), v));
        rt.state = TargetState::Active;
        if (t.semantic == Semantic::Hour) clockHour_ = v, haveHour_ = true;
        else if (t.semantic == Semantic::Minute) clockMinute_ = v, haveMinute_ = true;
        else if (t.semantic == Semantic::DayCounter) clockDay_ = v, haveDay_ = true;
        return;
    }

    // ---- written targets
    if (t.semantic == Semantic::WeatherManualFlag && !weatherDriverActive(ti)) {
        // never take the weather away from the game while nothing else drives it
        if (rt.state == TargetState::Active) detach(ti, &mem, true);
        rt.state = TargetState::Waiting;
        return;
    }
    const double hourOfDay = clock_.known() ? clock_.hourOfDay() : double(s.hourOfDay);
    double v = 0;
    if (!seasonValue(t, cfg_, s, sc, sched_.mix(), hourOfDay, v)) {
        rt.state = TargetState::Waiting;
        return;
    }
    if (t.feature == Feature::Weather) schedUsed_ = true;
    bool anyFail = false, anyOk = false;
    std::string lastErr;
    for (size_t k = 0; k < o.addrs.size() && k < rt.inst.size(); ++k) {
        FieldRt& f = rt.inst[k];
        const uint64_t addr = o.addrs[k] + t.offset;
        uint8_t cur[8] = {};
        if (!mem.read(addr, cur, n)) {
            anyFail = true;
            lastErr = "field unreadable";
            continue;
        }
        const uint32_t bits = rawBits(t.kind, cur);
        const double raw = decodeField(t.kind, cur);
        if (!f.attached) {
            f.attached = true;
            f.original = raw;
            f.originalBits = bits;
            f.lastValue = raw;
            f.wrote = false;
            f.stickStreak = 0;
            log(fmt("target %s: attached at 0x%llX, original %g", t.name.c_str(), (unsigned long long)addr, raw));
        } else if (f.wrote) {
            const uint32_t own = t.bit >= 0 ? (1u << t.bit) : t.mask;  // bits outside are the game's
            bool changed = isBitField(t) ? (bits & own) != (f.lastWrittenBits & own)
                                         : std::fabs(raw - f.lastWritten) > autoTol(t, f.lastWritten);
            if (changed) {
                ++rt.anomalies;
                if (++f.stickStreak >= limit) {
                    fail(ti, mem, fmt("value not sticking: the game rewrote it %u ticks in a row (now %g, ours %g)", f.stickStreak, raw,
                                      f.lastWritten));
                    return;
                }
                // the game set a new base value (settings, cutscene...): follow it
                if (t.mode == Mode::Scale || t.mode == Mode::Offset) f.original = raw;
                f.lastValue = raw;
            } else {
                f.stickStreak = 0;
            }
        }
        // desired game value
        uint8_t out[8] = {};
        double g;
        bool needWrite;
        if (isBitField(t)) {
            uint32_t m = t.bit >= 0 ? (1u << t.bit) : t.mask;
            uint32_t want = t.bit >= 0 ? ((v * t.scale + t.bias) >= 0.5 ? m : 0u) : (uint32_t(std::llround(std::max(0.0, v))) & m);
            uint32_t nb = (bits & ~m) | want;
            g = double(nb);
            needWrite = nb != bits;
            encodeField(t.kind, g, out);
            f.lastWrittenBits = nb;
        } else {
            switch (t.mode) {
                case Mode::Offset: g = f.original + v * t.scale + t.bias; break;
                case Mode::Scale: g = f.original * (1.0 + (v - 1.0) * t.scale); break;
                default: g = v * t.scale + t.bias; break;
            }
            if (!std::isnan(t.minV)) g = std::max(g, t.minV);
            if (!std::isnan(t.maxV)) g = std::min(g, t.maxV);
            if (t.maxRate > 0 && dt > 0) {
                double step = t.maxRate * dt;
                g = f.lastValue + std::max(-step, std::min(step, g - f.lastValue));
            }
            encodeField(t.kind, g, out);
            g = decodeField(t.kind, out);  // as stored
            needWrite = std::memcmp(out, cur, n) != 0;
            f.lastWrittenBits = rawBits(t.kind, out);
        }
        if (needWrite) {
            std::string why;
            if (!mem.write(addr, out, n, &why)) {
                anyFail = true;
                lastErr = "write refused or not verified: " + why;
                continue;
            }
            ++writes_;
            if (cfg_.logLevel >= 2) log(fmt("target %s: %g -> %g", t.name.c_str(), raw, g));
        }
        f.wrote = true;
        f.lastWritten = g;
        f.lastValue = g;
        anyOk = true;
        if (k == 0) {
            rt.desired = g;
            rt.current = g;
        }
    }
    if (anyFail) {
        ++rt.anomalies;
        if (++rt.failStreak >= limit) {
            fail(ti, mem, lastErr);
            return;
        }
    } else {
        rt.failStreak = 0;
    }
    if (anyOk && rt.state != TargetState::Active) log(fmt("target %s active (%s, %s)", t.name.c_str(), toString(t.semantic), toString(t.mode)));
    if (anyOk) rt.state = TargetState::Active;
}

std::vector<int> Applier::tick(const season::SeasonState& s, const season::SeasonConfig& sc, double realDt, Memory& mem) {
    std::vector<int> lost;
    ++ticks_;
    // 1. object liveness (vtable guard)
    for (size_t oi = 0; oi < ort_.size(); ++oi) {
        ObjectRt& o = ort_[oi];
        if (!o.bound) continue;
        bool alive = true;
        for (uint64_t a : o.addrs) {
            uint64_t vt = 0;
            if (!mem.read(a, &vt, 8) || (o.vtableX && (vt ^ kVtXor) != o.vtableX)) {
                alive = false;
                break;
            }
        }
        if (!alive) {
            log(fmt("object %s lost (vtable changed or memory gone at 0x%llX): nothing more is written to it",
                    cfg_.objects[oi].name.c_str(), (unsigned long long)o.addrs[0]));
            unbindObject(int(oi));
            lost.push_back(int(oi));
        }
    }
    // 2. clock first (the weather schedule and time_scale_current use it)
    haveHour_ = haveMinute_ = haveDay_ = false;
    for (size_t i = 0; i < trt_.size(); ++i)
        if (cfg_.targets[i].mode == Mode::Read) applyTarget(i, s, sc, realDt, mem);
    if (haveHour_ || haveDay_) {
        double h = haveHour_ ? clockHour_ + (haveMinute_ ? clockMinute_ / 60.0 : 0.0) : 0.0;
        clock_.sample(haveDay_, clockDay_, haveHour_, h);
    } else {
        clock_.sample(false, 0, false, 0);
    }
    // 3. weather schedule on the season day (follows the game clock once season_tick uses it)
    const int r = std::max(0, std::min(cfg_.region, season::kRegionCount - 1));
    sched_.update(s.day * 24.0, s.weather[r]);
    // 4. written targets: drivers before the manual flag, so the flag sees their state this tick
    schedUsed_ = false;
    for (int pass = 0; pass < 2; ++pass)
        for (size_t i = 0; i < trt_.size(); ++i) {
            const TargetSpec& t = cfg_.targets[i];
            if (t.mode == Mode::Read) continue;
            if ((t.semantic == Semantic::WeatherManualFlag) != (pass == 1)) continue;
            applyTarget(i, s, sc, realDt, mem);
        }
    for (size_t oi = 0; oi < ort_.size(); ++oi) {
        bool need = false;
        for (size_t i = 0; i < trt_.size(); ++i) {
            const TargetSpec& t = cfg_.targets[i];
            need = need || (size_t(t.objectIndex) == oi && t.enabled && featureOn(t.feature, cfg_, s, sc) &&
                            trt_[i].state != TargetState::Error);
        }
        ort_[oi].needed = need;
    }
    return lost;
}

int Applier::restoreAll(Memory& mem) {
    int n = 0;
    for (size_t i = 0; i < trt_.size(); ++i) {
        const TargetSpec& t = cfg_.targets[i];
        TargetRt& rt = trt_[i];
        const ObjectRt& o = ort_[size_t(t.objectIndex)];
        if (o.bound) {
            // only into objects that are still the same (vtable guard)
            bool alive = true;
            for (uint64_t a : o.addrs) {
                uint64_t vt = 0;
                if (!mem.read(a, &vt, 8) || (o.vtableX && (vt ^ kVtXor) != o.vtableX)) alive = false;
            }
            if (alive)
                for (size_t k = 0; k < rt.inst.size() && k < o.addrs.size(); ++k)
                    if (rt.inst[k].attached && t.restore && t.mode != Mode::Read && restoreField(t, o.addrs[k] + t.offset, rt.inst[k], mem)) ++n;
        }
        for (FieldRt& f : rt.inst) f = FieldRt();
        if (rt.state == TargetState::Active) rt.state = TargetState::Waiting;
    }
    if (n) log(fmt("restored %d original value(s)", n));
    return n;
}

void Applier::status(ApplyStatus& out) const {
    out.targets.clear();
    for (int f = 0; f < kFeatureCount; ++f) out.features[f] = FeatureStatus();
    for (size_t i = 0; i < trt_.size(); ++i) {
        const TargetSpec& t = cfg_.targets[i];
        const TargetRt& rt = trt_[i];
        TargetStatus ts;
        ts.name = t.name;
        ts.semantic = t.semantic;
        ts.weather = t.weather;
        ts.feature = t.feature;
        ts.mode = t.mode;
        ts.state = rt.state;
        const ObjectRt& o = ort_[size_t(t.objectIndex)];
        ts.instances = uint32_t(o.bound ? o.addrs.size() : 0);
        ts.address = o.bound && !o.addrs.empty() ? o.addrs[0] + t.offset : 0;
        ts.haveOriginal = !rt.inst.empty() && rt.inst[0].attached && t.mode != Mode::Read;
        ts.original = ts.haveOriginal ? rt.inst[0].original : 0;
        ts.desired = rt.desired;
        ts.current = rt.current;
        ts.anomalies = rt.anomalies;
        ts.error = rt.error;
        out.targets.push_back(ts);
        FeatureStatus& fs = out.features[int(t.feature)];
        if (!t.enabled || rt.state == TargetState::Off) continue;
        ++fs.enabledTargets;
        if (rt.state == TargetState::Active) ++fs.activeTargets;
        if (rt.state == TargetState::Error) ++fs.errorTargets;
    }
    for (int f = 0; f < kFeatureCount; ++f) {
        FeatureStatus& fs = out.features[f];
        if (!fs.enabledTargets) fs.state = FeatureState::Inactive;
        else if (fs.activeTargets) fs.state = FeatureState::Active;
        else if (fs.errorTargets) fs.state = FeatureState::Error;
        else fs.state = FeatureState::NotFound;
    }
    out.weather = sched_.mix();
    out.weatherScheduled = schedUsed_;
    out.clockKnown = clock_.known();
    out.gameHour = clock_.hourOfDay();
    out.gameDay = clock_.day();
    out.ticks = ticks_;
    out.writes = writes_;
}

}  // namespace qs::apply
