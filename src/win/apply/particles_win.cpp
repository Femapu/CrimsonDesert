// QuattroStagioni - seasonal particles in the game (see particles_win.h).
#include "particles_win.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include "../../core/apply/particles.h"
#include "../../core/apply/snow.h"
#include "../log.h"
#include "../qs_state.h"
#include "../winutil.h"
#include "feature_status.h"
#include "weather_win.h"

namespace qs {
namespace particlesimpl {
namespace {

const char* const kKey = "particles";
constexpr uintptr_t kPrecip = 0x12C;  // WeatherConstant precipitation (2.04.02, weather_win.cpp)
constexpr uintptr_t kSnow = 0x16C;    // WeatherConstant snow (2.04.02, weather_win.cpp)
const char* const kNotFound = "campo non ancora trovato (probe)";

apply::ParticlesConfig g_cfg;    // set before the thread starts, read only afterwards
apply::ParticleFields g_fields;  // same
HANDLE g_thread = nullptr, g_stop = nullptr;
volatile LONG g_running = 0;

std::atomic<bool> g_active{false};                      // callback may write
std::atomic<bool> g_flakesOn{false}, g_leavesOn{false}, g_pollenOn{false};
std::atomic<float> g_flakeK{0}, g_leaves{0}, g_pollen{0};
std::atomic<float> g_gameSnow{0}, g_outSnow{0};
std::atomic<unsigned long long> g_writes{0};
std::atomic<bool> g_failed{false};
std::mutex g_failMu;
std::string g_failWhy;  // under g_failMu
bool g_started = false;

void fail(const std::string& why) {
    g_active = false;  // no further writes this tick or later
    bool expected = false;
    if (!g_failed.compare_exchange_strong(expected, true)) return;
    std::lock_guard<std::mutex> lk(g_failMu);
    g_failWhy = why;  // the worker logs it (no logging on the game thread)
}

uintptr_t layerBase(const ComposeContext& c, apply::Layer l) {
    switch (l) {
        case apply::Layer::Weather: return c.weather;
        case apply::Layer::Atmosphere: return c.atmosphere;
        case apply::Layer::Manager: return c.manager;
    }
    return 0;
}

// One leaves / pollen field: read the current floats, write the season's.
bool writeField(const ComposeContext& c, const apply::LayerRef& r, float intensity, const char* name) {
    const uintptr_t base = layerBase(c, r.layer);
    if (!base) return true;  // layer not there this tick
    float v[4], o[4];
    const size_t n = size_t(std::clamp(r.count, 1, 4));
    if (!c.mem->read(base + r.offset, v, n * sizeof(float))) return fail(std::string(name) + ": cannot read " + r.text()), false;
    bool diff = false;
    for (size_t i = 0; i < n; ++i) {
        o[i] = apply::particleField(v[i], intensity, c.bypass, g_fields);
        diff |= o[i] != v[i];
    }
    if (!diff) return true;
    std::string why;
    if (!c.mem->write(base + r.offset, o, n * sizeof(float), &why))
        return fail(std::string(name) + ": write to " + r.text() + " refused (" + why + ")"), false;
    g_writes.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// Game thread, after the compose (and after the weather module's own writes).
void onCompose(const ComposeContext& c) {
    if (!g_active.load(std::memory_order_relaxed)) return;
    if (c.desert || !c.desertKnown || !(c.bypass < 1.0f)) return;  // desert or unknown: the game's own
    const float k = g_flakeK.load(std::memory_order_relaxed);
    if (c.weather && k > 0.0f && g_flakesOn.load(std::memory_order_relaxed)) {
        float precip = 0, snow = 0;
        // re-read: the weather module may have just written the season's snow
        if (!c.mem->read(c.weather + kPrecip, &precip, sizeof precip) || !c.mem->read(c.weather + kSnow, &snow, sizeof snow))
            return fail("cannot read the weather layer (snow +0x16C)");
        const float out = apply::flakes(snow, precip, k, c.bypass, g_cfg);
        g_gameSnow.store(snow, std::memory_order_relaxed);
        g_outSnow.store(out, std::memory_order_relaxed);
        if (out != snow) {
            std::string why;
            if (!c.mem->write(c.weather + kSnow, &out, sizeof out, &why)) return fail("write to snow +0x16C refused (" + why + ")");
            g_writes.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (g_leavesOn.load(std::memory_order_relaxed) && !writeField(c, g_fields.leaves, g_leaves.load(), "leaves")) return;
    if (g_pollenOn.load(std::memory_order_relaxed)) writeField(c, g_fields.pollen, g_pollen.load(), "pollen");
}

// Why leaves / pollen do not run (empty = they can).
std::string fieldOffReason(const apply::LayerRef& r) {
    if (!g_cfg.enabled) return "[Particles] Enabled=0";
    if (!g_fields.enabled) return std::string(kNotFound) + ", targets.ini [particles] Enabled=0";
    if (!r.valid) return kNotFound;
    return "";
}

DWORD WINAPI worker(LPVOID) {
    // the callback needs the weather hook
    const uint64_t t0 = GetTickCount64();
    while (!weatherHookInstalled()) {
        if (WaitForSingleObject(g_stop, 250) == WAIT_OBJECT_0) return 0;
        WeatherInfo wi;
        if (weatherGetInfo(wi) && wi.state == WeatherModState::Disabled)
            return featureDisable(kKey, "weather hook not available (" + wi.reason + ")"), 0;
        if (GetTickCount64() - t0 > 300000) return featureDisable(kKey, "weather hook not installed after 5 minutes"), 0;
    }
    if (!weatherAddPostCompose(&onCompose)) return featureDisable(kKey, "no free after-compose slot"), 0;
    const bool leavesCan = fieldOffReason(g_fields.leaves).empty(), pollenCan = fieldOffReason(g_fields.pollen).empty();
    QS_INFO("particles: after-compose callback on; winter flakes %s, leaves %s, pollen %s", g_cfg.winterFlakes ? "on" : "off",
            leavesCan ? g_fields.leaves.text().c_str() : "off", pollenCan ? g_fields.pollen.text().c_str() : "off");
    g_active = true;

    uint64_t prev = GetTickCount64(), lastLog = prev;
    int prevOn = -1;
    while (WaitForSingleObject(g_stop, 250) != WAIT_OBJECT_0) {
        if (g_failed.load()) {
            std::string why;
            {
                std::lock_guard<std::mutex> lk(g_failMu);
                why = g_failWhy;
            }
            featureDisable(kKey, why);
            break;
        }
        const uint64_t now = GetTickCount64();
        float dt = float(now - prev) / 1000.0f;
        prev = now;
        if (dt > 1.0f) dt = 1.0f;
        season::SeasonState s;
        season::SeasonConfig sc;
        {
            std::lock_guard<std::mutex> lk(shared().mutex);
            s = shared().season;
            sc = shared().config;
        }
        const int r = int(season::Region::Temperate);
        const bool on = g_cfg.enabled && sc.particles && s.regionActive[r];
        const float winterK = apply::winterIntensity(s, sc);
        const float leavesT = g_cfg.leavesFromSeason ? s.leaves[r]
                                                     : apply::leavesIntensity(g_cfg, s.stage[r], sc.regions[r].leaves);
        const float pollenT = apply::pollenIntensity(g_cfg, s.stage[r]);
        const float step = g_cfg.rampPerSec > 0 ? g_cfg.rampPerSec * dt : 2.0f;
        const float fk = apply::approach(g_flakeK.load(), on && g_cfg.winterFlakes ? winterK : 0.0f, step);
        const float lv = apply::approach(g_leaves.load(), on ? std::clamp(leavesT, 0.0f, 1.0f) : 0.0f, step);
        const float pl = apply::approach(g_pollen.load(), on ? pollenT : 0.0f, step);
        g_flakeK = fk;
        g_leaves = lv;
        g_pollen = pl;
        g_flakesOn = on && g_cfg.winterFlakes;
        g_leavesOn = on && leavesCan;
        g_pollenOn = on && pollenCan;

        if (int(on) != prevOn) {
            prevOn = int(on);
            QS_INFO("particles: %s", on ? "on" : "off (F5 Particelle, [Particles] Enabled or region off)");
            if (!on) featureSet(kKey, FeatureState::Off, "spento (F5 Particelle o regione spenta)");
            else if (g_cfg.winterFlakes) featureSet(kKey, FeatureState::Active);
            else featureSet(kKey, FeatureState::Off, "WinterFlakes=0; foglie e pollini: " + std::string(kNotFound));
        }
        char d[160];
        snprintf(d, sizeof d, "fiocchi %.0f%%, foglie %.0f%%%s, pollini %.0f%%%s", fk * 100.0f, lv * 100.0f,
                 leavesCan ? "" : " (spente)", pl * 100.0f, pollenCan ? "" : " (spenti)");
        featureDetail(kKey, d);
        if (now - lastLog >= 60000) {
            lastLog = now;
            QS_INFO("particles: winter %.0f%%, flakes %.0f%% (snow %.2f -> %.2f), leaves %.0f%% (%s), pollen %.0f%% (%s), %llu writes",
                    winterK * 100.0f, fk * 100.0f, g_gameSnow.load(), g_outSnow.load(), lv * 100.0f,
                    leavesCan ? "written" : kNotFound, pl * 100.0f, pollenCan ? "written" : kNotFound, g_writes.load());
        }
    }
    g_active = false;
    weatherRemovePostCompose(&onCompose);
    Sleep(50);  // a callback already running on the game thread finishes
    QS_INFO("particles: stop");
    return 0;
}

}  // namespace
}  // namespace particlesimpl

bool particlesStart(const std::wstring& scriptsDir, bool versionOk) {
    using namespace particlesimpl;
    if (InterlockedCompareExchange(&g_running, 1, 0) != 0) return false;
    featureRegister(kKey, "Particelle stagionali");
    std::vector<uint8_t> raw;
    std::vector<std::string> warn;
    if (readWholeFile(joinPath(scriptsDir, L"QuattroStagioni.ini"), raw)) {
        Ini ini;
        ini.parse(std::string(raw.begin(), raw.end()));
        g_cfg.load(ini, warn);
    }
    raw.clear();
    if (readWholeFile(joinPath(joinPath(scriptsDir, L"QuattroStagioni"), L"targets.ini"), raw)) {
        Ini ini;
        ini.parse(std::string(raw.begin(), raw.end()));
        g_fields.load(ini, warn);
    }
    for (const std::string& w : warn) QS_ERR("particles: %s", w.c_str());
    g_started = true;
    const std::string lr = fieldOffReason(g_fields.leaves), pr = fieldOffReason(g_fields.pollen);
    QS_INFO("particles: config Enabled=%d WinterFlakes=%d boost %.2f min %.2f active above %.3f, ramp %.2f/s, leaves %s, pollen %s",
            int(g_cfg.enabled), int(g_cfg.winterFlakes), g_cfg.winterFlakeBoost, g_cfg.winterFlakeMin, g_cfg.snowMinActive,
            g_cfg.rampPerSec, g_cfg.leavesFromSeason ? "from the season engine" : apply::formatPhaseValues(g_cfg.leavesPerPhase).c_str(),
            apply::formatPhaseValues(g_cfg.pollenPerPhase).c_str());
    if (!lr.empty()) QS_INFO("particles: leaves off: %s", lr.c_str());
    if (!pr.empty()) QS_INFO("particles: pollen off: %s", pr.c_str());
    if (!g_cfg.enabled || (!g_cfg.winterFlakes && !lr.empty() && !pr.empty())) {
        QS_INFO("particles: nothing to do (%s)", !g_cfg.enabled ? "[Particles] Enabled=0" : "WinterFlakes=0 and no leaves or pollen field");
        featureSet(kKey, FeatureState::Off, !g_cfg.enabled ? "[Particles] Enabled=0" : "WinterFlakes=0; foglie e pollini: " + std::string(kNotFound));
        g_running = 0;
        return false;
    }
    if (!versionOk) {
        featureDisable(kKey, "version guard failed: the game is never written");
        g_running = 0;
        return false;
    }
    QS_INFO("particles: start, waiting for the weather hook");
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_thread = g_stop ? CreateThread(nullptr, 0, worker, nullptr, 0, nullptr) : nullptr;
    if (!g_thread) {
        featureDisable(kKey, "cannot create the particles thread");
        g_running = 0;
        return false;
    }
    return true;
}

void particlesStop() {
    using namespace particlesimpl;
    if (!g_thread) return;
    SetEvent(g_stop);
    WaitForSingleObject(g_thread, 3000);
    CloseHandle(g_thread);
    g_thread = nullptr;
    g_running = 0;
}

bool particlesGetInfo(ParticlesInfo& out) {
    using namespace particlesimpl;
    out.started = g_started;
    out.flakesOn = g_flakesOn.load();
    out.leavesOn = g_leavesOn.load();
    out.pollenOn = g_pollenOn.load();
    out.flakeK = g_flakeK.load();
    out.leaves = g_leaves.load();
    out.pollen = g_pollen.load();
    out.gameSnow = g_gameSnow.load();
    out.outSnow = g_outSnow.load();
    out.writes = g_writes.load();
    return g_started;
}

}  // namespace qs
