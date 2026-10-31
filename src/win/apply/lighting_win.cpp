// QuattroStagioni - seasonal light and sky in the game (see lighting_win.h).
#include "lighting_win.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include "../../core/i18n.h"
#include "../log.h"
#include "../qs_state.h"
#include "../winutil.h"
#include "weather_win.h"

namespace qs {
namespace lightingimpl {
namespace {

using i18n::tr;
using apply::kLightChannels;
using apply::kLightFields;

const char* const kKey = "lighting";
constexpr int kStaleTicks = 90;       // ticks in a row with our own value still in memory -> not recomposed
constexpr uint32_t kWriteFailLimit = 50;

// Trip codes set on the game thread, reported by the worker.
enum Trip : int { TripNone = 0, TripStale, TripNonFinite, TripRange };

struct FieldRt {                      // game thread only (set up before the consumer is registered)
    bool on = false;
    apply::LayerRef ref;
    float minV = 0, maxV = 0;
    int comps = 1;                    // components scaled (3 colours, 1 scalar)
    int channel = 0;                  // first LightChannel
    float last[4] = {};               // last values written
    float orig[4] = {};               // last values the game composed
    bool tracked[4] = {};             // last[j] differs from the game's value (ours is in memory)
    int same = 0;
};

FieldRt g_field[kLightFields];
std::atomic<float> g_mult[kLightChannels];
std::atomic<bool> g_active{false};
std::atomic<int> g_trip{TripNone}, g_tripField{-1};
std::atomic<float> g_tripValue{0};
std::atomic<uint32_t> g_writeFails{0};
std::atomic<uint64_t> g_writes{0};

std::mutex g_mu;  // g_info
LightingInfo g_info;
apply::LightingConfig g_cfg;
HANDLE g_thread = nullptr, g_stop = nullptr;
volatile LONG g_running = 0;

uintptr_t layerBase(const ComposeContext& c, apply::Layer l) {
    switch (l) {
        case apply::Layer::Weather: return c.weather;
        case apply::Layer::Atmosphere: return c.atmosphere;
        case apply::Layer::Manager: return c.manager;
    }
    return 0;
}

// Puts the game's value back where our value is still in memory (game thread).
void restoreAll(const ComposeContext& c) {
    for (FieldRt& f : g_field) {
        if (!f.on) continue;
        const uintptr_t base = layerBase(c, f.ref.layer);
        float v[4];
        const size_t n = size_t(f.ref.count) * sizeof(float);
        if (!base || !c.mem->read(base + f.ref.offset, v, n)) continue;
        bool ours = false;
        for (int j = 0; j < f.comps; ++j) ours |= f.tracked[j] && v[j] == f.last[j];
        if (!ours) continue;
        for (int j = 0; j < f.comps; ++j)
            if (f.tracked[j] && std::isfinite(f.orig[j])) v[j] = f.orig[j];
        std::string why;
        c.mem->write(base + f.ref.offset, v, n, &why);
        for (bool& t : f.tracked) t = false;
    }
}

void trip(const ComposeContext& c, Trip t, int field, float value) {
    g_active = false;
    restoreAll(c);
    g_tripField = field;
    g_tripValue = value;
    g_trip = t;
}

// After the game composed its atmosphere (game thread): game value * multiplier.
void onCompose(const ComposeContext& c) {
    if (!g_active.load(std::memory_order_acquire)) return;
    for (int fi = 0; fi < kLightFields; ++fi) {
        FieldRt& f = g_field[fi];
        if (!f.on) continue;
        const uintptr_t base = layerBase(c, f.ref.layer);
        if (!base) continue;
        float v[4];
        const size_t n = size_t(f.ref.count) * sizeof(float);
        if (!c.mem->read(base + f.ref.offset, v, n)) continue;
        bool anyTracked = false, stale = true;
        for (int j = 0; j < f.comps; ++j) {
            if (!std::isfinite(v[j])) return trip(c, TripNonFinite, fi, v[j]);
            if (f.tracked[j]) {
                anyTracked = true;
                stale &= v[j] == f.last[j];
            }
        }
        if (anyTracked && stale) {  // our value is still there: not composed anew (this tick at least)
            if (++f.same >= kStaleTicks) return trip(c, TripStale, fi, v[0]);
            continue;               // never scale our own value again
        }
        f.same = 0;
        bool write = false;
        for (int j = 0; j < f.comps; ++j) {
            if (v[j] < f.minV || v[j] > f.maxV) return trip(c, TripRange, fi, v[j]);
            f.orig[j] = v[j];
            const float out = apply::applyScale(v[j], g_mult[f.channel + j].load(std::memory_order_relaxed), c.bypass);
            f.tracked[j] = out != v[j];
            write |= f.tracked[j];
            v[j] = out;
        }
        if (!write) continue;
        std::string why;
        if (c.mem->write(base + f.ref.offset, v, n, &why)) {
            for (int j = 0; j < f.comps; ++j) f.last[j] = v[j];
            g_writes.fetch_add(1, std::memory_order_relaxed);
        } else {
            for (bool& t : f.tracked) t = false;
            g_writeFails.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

std::string fieldName(int f) { return f >= 0 && f < kLightFields ? apply::lightFieldKey(f) : "?"; }

void setState(FeatureState s, const std::string& why = std::string()) {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_info.state = s;
    }
    featureSet(kKey, s, why);
}

DWORD WINAPI worker(void*) {
    apply::LightingModel model;
    model.configure(g_cfg);
    uint64_t prev = GetTickCount64(), lastLog = prev;
    int lastState = -1;
    while (WaitForSingleObject(g_stop, 250) != WAIT_OBJECT_0) {
        const uint64_t now = GetTickCount64();
        float dt = float(now - prev) / 1000.0f;
        prev = now;
        if (dt > 1.0f) dt = 1.0f;

        const int t = g_trip.load();
        if (t != TripNone) {
            char b[256];
            const std::string fn = fieldName(g_tripField.load());
            if (t == TripStale)
                snprintf(b, sizeof b, "%s %s", fn.c_str(), tr("non ricomposto dal gioco: si accumulerebbe (valore originale rimesso)"));
            else if (t == TripNonFinite)
                snprintf(b, sizeof b, "%s %s", fn.c_str(), tr("ha un valore non finito: campo sbagliato?"));
            else
                snprintf(b, sizeof b, "%s %s %g", fn.c_str(), tr("fuori dai limiti Min/Max di targets.ini:"), double(g_tripValue.load()));
            featureDisable(kKey, b);
            std::lock_guard<std::mutex> lk(g_mu);
            g_info.state = FeatureState::Disabled;
            break;
        }
        if (g_writeFails.load() >= kWriteFailLimit) {
            g_active = false;
            featureDisable(kKey, tr("le scritture sui campi della luce non riescono"));
            std::lock_guard<std::mutex> lk(g_mu);
            g_info.state = FeatureState::Disabled;
            break;
        }

        season::SeasonState s;
        season::SeasonConfig sc;
        {
            std::lock_guard<std::mutex> lk(shared().mutex);
            s = shared().season;
            sc = shared().config;
        }
        const int r = int(season::Region::Temperate);
        const bool on = g_cfg.enabled && sc.lightControl && s.regionActive[r];
        model.step(s.stage[r], on, dt);
        const apply::LightingMultipliers& m = model.current();
        for (int i = 0; i < kLightChannels; ++i) g_mult[i].store(m.v[i], std::memory_order_relaxed);

        // the consumer writes as soon as the weather hook calls it; off from the menu, it fades back to 1
        const bool hook = weatherHookInstalled();
        if (hook) g_active = true;
        const int st = !hook ? int(FeatureState::Waiting) : on ? int(FeatureState::Active) : int(FeatureState::Off);
        if (st != lastState) {
            lastState = st;
            if (st == int(FeatureState::Waiting)) setState(FeatureState::Waiting, tr("in attesa dell'aggancio del meteo"));
            else if (st == int(FeatureState::Active)) setState(FeatureState::Active);
            else setState(FeatureState::Off, tr("spenta dal menu F5 o regione temperata disattivata"));
            QS_INFO("lighting: state %s", st == int(FeatureState::Waiting) ? "waiting for the weather hook" : on ? "active" : "off (menu or region)");
        }
        using C = apply::LightChannel;
        char d[160];
        snprintf(d, sizeof d, "%s %.2f, %s %.2f, %s %.2f, %s %.2f/%.2f/%.2f", tr("sole"), double(m[C::SunIntensity]), tr("ambiente"),
                 double(m[C::AmbientIntensity]), tr("nebbia"), double(m[C::FogDensity]), tr("cielo"), double(m[C::SkyR]),
                 double(m[C::SkyG]), double(m[C::SkyB]));
        featureDetail(kKey, d);
        {
            std::lock_guard<std::mutex> lk(g_mu);
            g_info.stage = s.stage[r].id();
            g_info.on = on;
            g_info.mult = m;
            g_info.writes = g_writes.load();
        }
        if (g_cfg.logIntervalSec > 0 && double(now - lastLog) >= g_cfg.logIntervalSec * 1000.0) {
            lastLog = now;
            QS_INFO("lighting: stage %s %s | sun %.3f rgb %.3f %.3f %.3f | ambient %.3f rgb %.3f %.3f %.3f | fog %.3f | sky %.3f %.3f %.3f | "
                    "writes %llu, write fails %u",
                    s.stage[r].id().c_str(), on ? "on" : "off", double(m[C::SunIntensity]), double(m[C::SunR]), double(m[C::SunG]),
                    double(m[C::SunB]), double(m[C::AmbientIntensity]), double(m[C::AmbientR]), double(m[C::AmbientG]),
                    double(m[C::AmbientB]), double(m[C::FogDensity]), double(m[C::SkyR]), double(m[C::SkyG]), double(m[C::SkyB]),
                    (unsigned long long)g_writes.load(), g_writeFails.load());
        }
    }
    return 0;
}

bool readIni(const std::wstring& path, Ini& ini) {
    std::vector<uint8_t> raw;
    if (!readWholeFile(path, raw)) return false;
    ini.parse(std::string(raw.begin(), raw.end()));
    return true;
}

}  // namespace
}  // namespace lightingimpl

bool lightingStart(const std::wstring& scriptsDir, bool versionOk) {
    using namespace lightingimpl;
    if (InterlockedCompareExchange(&g_running, 1, 0) != 0) return false;
    featureRegister(kKey, "Luce stagionale");
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_info.started = true;
    }
    for (std::atomic<float>& m : g_mult) m = 1.0f;

    apply::LightingConfig cfg;
    std::vector<std::string> warn;
    Ini ini;
    if (readIni(joinPath(scriptsDir, L"QuattroStagioni.ini"), ini)) cfg.load(ini, warn);
    apply::LightingTargets tg;
    Ini tini;
    if (readIni(joinPath(joinPath(scriptsDir, L"QuattroStagioni"), L"targets.ini"), tini)) tg.load(tini, warn);
    for (const std::string& w : warn) QS_ERR("lighting: %s", w.c_str());
    g_cfg = cfg;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_info.fields = tg.count();
    }

    if (!cfg.enabled) {
        QS_INFO("lighting: [Lighting] Enabled=0: seasonal light off");
        setState(FeatureState::Off, tr("spenta in QuattroStagioni.ini ([Lighting] Enabled=0)"));
        g_running = 0;
        return false;
    }
    if (!versionOk) {
        featureDisable(kKey, tr("controllo della versione non superato: il gioco non viene scritto"));
        std::lock_guard<std::mutex> lk(g_mu);
        g_info.state = FeatureState::Disabled;
        g_running = 0;
        return false;
    }
    if (!tg.enabled || tg.count() == 0) {
        QS_INFO("lighting: no light field %s in targets.ini [lighting]: seasonal light off (find them with the probes, docs/dev/luce.md)",
                tg.enabled ? "set" : "enabled");
        setState(FeatureState::Off, tr("campi della luce non ancora trovati (probe)"));
        g_running = 0;
        return false;
    }
    for (int f = 0; f < kLightFields; ++f) {
        FieldRt& r = g_field[f];
        r = FieldRt();
        const apply::LightingTarget& t = tg.field[f];
        if (!t.ref.valid) continue;
        r.on = true;
        r.ref = t.ref;
        r.minV = t.minV;
        r.maxV = t.maxV;
        r.comps = apply::lightFieldComponents(f);
        r.channel = apply::lightFieldChannel(f);
        QS_INFO("lighting: field %s = %s (limits %g .. %g)", apply::lightFieldKey(f), t.ref.text().c_str(), double(t.minV),
                double(t.maxV));
    }
    g_trip = TripNone;
    g_writeFails = 0;
    if (!weatherAddPostCompose(&onCompose)) {
        featureDisable(kKey, tr("nessuno slot libero dopo la composizione del meteo"));
        g_running = 0;
        return false;
    }
    QS_INFO("lighting: start, %d field(s), ramp %.3f/s, after the game's compose (needs the weather hook)", tg.count(),
            double(cfg.rampPerSec));
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_thread = g_stop ? CreateThread(nullptr, 0, worker, nullptr, 0, nullptr) : nullptr;
    if (!g_thread) {
        weatherRemovePostCompose(&onCompose);
        featureDisable(kKey, tr("impossibile creare il thread della luce"));
        g_running = 0;
        return false;
    }
    return true;
}

void lightingStop() {
    using namespace lightingimpl;
    // the game composes these fields anew every tick (checked by the stale test): stopping is enough
    g_active = false;
    weatherRemovePostCompose(&onCompose);
    if (!g_thread) return;
    SetEvent(g_stop);
    WaitForSingleObject(g_thread, 3000);
    CloseHandle(g_thread);
    CloseHandle(g_stop);
    g_thread = nullptr;
    g_stop = nullptr;
    g_running = 0;
}

bool lightingGetInfo(LightingInfo& out) {
    using namespace lightingimpl;
    std::lock_guard<std::mutex> lk(g_mu);
    out = g_info;
    return g_info.started;
}

}  // namespace qs
