// QuattroStagioni - lightning in storms, by season (see lightning_win.h).
#include "lightning_win.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "../../core/apply/weather.h"
#include "../log.h"
#include "../qs_state.h"
#include "../winutil.h"
#include "feature_status.h"
#include "weather_win.h"

namespace qs {
namespace lightningimpl {
namespace {

const char* const kKey = "lightning";
const char* const kNoField = "campo dei fulmini non ancora trovato (probe)";

// ---- state --------------------------------------------------------------------------------------
std::mutex g_mu;  // director, info
apply::LightningDirector g_dir;
apply::LightningConfig g_cfg;
apply::LightningTarget g_target;  // read-only after start
LightningInfo g_info;
HANDLE g_thread = nullptr, g_stop = nullptr;
volatile LONG g_running = 0;
bool g_registered = false;

// worker -> game thread
std::atomic<bool> g_writeOn{false};
std::atomic<float> g_level{0.0f};
std::atomic<uint64_t> g_pulseUntil{0};  // GetTickCount64() end of the current flash
std::atomic<uint64_t> g_writes{0};
// game thread -> worker (set once, then g_writeOn stays false)
std::atomic<int> g_failCode{0};
char g_failWhy[200] = {};

// game thread only: the game's own value and what we wrote last tick (to tell a field the game does
// not recompose every tick, so a level write never compounds)
float g_game[4] = {}, g_last[4] = {};
bool g_wrote = false;

void fail(const char* what, uintptr_t addr) {
    g_writeOn.store(false);
    snprintf(g_failWhy, sizeof g_failWhy, "%s del campo %s non riuscita (indirizzo %p)", what, g_target.field.text().c_str(),
             reinterpret_cast<void*>(addr));
    g_failCode.store(1, std::memory_order_release);
}

// GAME thread, after the weather compose: only reads and writes a few floats.
void onCompose(const ComposeContext& ctx) {
    if (!g_writeOn.load(std::memory_order_relaxed) || !ctx.mem) return;
    const apply::LayerRef& f = g_target.field;
    const uintptr_t base = f.layer == apply::Layer::Weather      ? ctx.weather
                           : f.layer == apply::Layer::Atmosphere ? ctx.atmosphere
                                                                 : ctx.manager;
    if (!base) return;  // block not readable this tick: nothing to do
    const uintptr_t addr = base + f.offset;
    const int n = std::clamp(f.count, 1, 4);
    float cur[4] = {};
    if (!ctx.mem->read(addr, cur, sizeof(float) * n)) return fail("lettura", addr);
    for (int i = 0; i < n; ++i) {
        if (g_wrote && std::memcmp(&cur[i], &g_last[i], sizeof(float)) == 0) continue;  // still ours: keep the game's
        g_game[i] = cur[i];
    }
    const float share = ctx.desert ? 0.0f : std::clamp(1.0f - ctx.bypass, 0.0f, 1.0f);  // desert fade, like the weather
    float out[4];
    bool write = false;
    if (g_target.mode == apply::LightningMode::Level) {
        const float k = g_level.load(std::memory_order_relaxed) * share;
        if (k > 1e-4f) {
            for (int i = 0; i < n; ++i) out[i] = g_game[i] + (g_target.value - g_game[i]) * k;
            write = true;
        }
    } else {
        write = share > 0.5f && GetTickCount64() < g_pulseUntil.load(std::memory_order_relaxed);
        for (int i = 0; i < n; ++i) out[i] = g_target.value;
    }
    if (!write) {
        // the game did not recompose the field: hand its own value back once
        bool stale = g_wrote;
        for (int i = 0; i < n && stale; ++i) stale = std::memcmp(&cur[i], &g_last[i], sizeof(float)) == 0;
        g_wrote = false;
        if (!stale) return;
        for (int i = 0; i < n; ++i) out[i] = g_game[i];
    }
    for (int i = 0; i < n; ++i)
        if (!std::isfinite(out[i])) return;
    std::string why;
    if (!ctx.mem->write(addr, out, sizeof(float) * n, &why)) return fail("scrittura", addr);
    if (write) {
        std::memcpy(g_last, out, sizeof(float) * n);
        g_wrote = true;
    }
    g_writes.fetch_add(1, std::memory_order_relaxed);
}

DWORD WINAPI worker(LPVOID) {
    const apply::LightningConfig cfg = g_cfg;
    const bool usable = g_target.usable();
    apply::GameClock clock;
    uint64_t prev = GetTickCount64(), started = prev, lastDetail = 0;
    bool loggedFallback = false, prevAllowed = false, firstAllowed = true;
    std::string prevReason = "-";
    while (WaitForSingleObject(g_stop, 250) != WAIT_OBJECT_0) {
        const uint64_t now = GetTickCount64();
        double dt = double(now - prev) / 1000.0;
        prev = now;
        if (dt > 1.0) dt = 1.0;
        if (g_failCode.load(std::memory_order_acquire)) {
            featureDisable(kKey, g_failWhy);
            break;
        }
        season::SeasonState s;
        season::SeasonConfig sc;
        {
            std::lock_guard<std::mutex> lk(shared().mutex);
            s = shared().season;
            sc = shared().config;
        }
        WeatherInfo wi;
        const bool haveW = weatherGetInfo(wi);
        const int r = int(season::Region::Temperate);
        const bool on = cfg.enabled && sc.lightningControl && s.regionActive[r];
        const bool desertBlock = !haveW || !wi.mapFound || wi.desert;  // unknown counts as desert, like the weather
        const bool ourStorm = haveW && wi.state == WeatherModState::Active && wi.hasSpell && wi.spell.kind == apply::WeatherKind::Storm;
        const bool hudStorm = haveW && wi.hudFlags >= 0 && (uint32_t(wi.hudFlags) & apply::kHudStormBit);

        apply::LightningInput in;
        in.storm = ourStorm || hudStorm;
        in.intensity = ourStorm ? wi.spell.intensity : 1.0f;
        in.allowed = on && !desertBlock;
        in.phase = s.regionPhase[r];
        in.progress = s.regionPhaseProgress[r];
        in.transitionFraction = sc.transitionFraction;

        // game hours: the game's clock (a jump, e.g. sleeping, fires nothing), else real time
        double hours = 0;
        if (haveW && wi.hour >= 0) {
            const apply::GameClock::Step st = clock.feed(wi.hour);
            hours = st.jump ? 0.0 : st.hours;
        } else if (now - started > 30000) {
            if (!loggedFallback) {
                QS_ERR("lightning: the game's hour is not readable: strikes follow real time (%.0f real minutes a day)",
                       sc.realMinutesPerDay);
                loggedFallback = true;
            }
            hours = dt * 24.0 / (std::max(1.0f, sc.realMinutesPerDay) * 60.0);
        }

        std::lock_guard<std::mutex> lk(g_mu);
        const int fired = g_dir.advance(dt, hours, in);
        for (const std::string& l : g_dir.takeLog()) QS_INFO("lightning: %s", l.c_str());
        g_level.store(g_dir.level(), std::memory_order_relaxed);
        if (fired) {
            g_pulseUntil.store(now + uint64_t(cfg.pulseMs), std::memory_order_relaxed);
            if (cfg.logStrikes || !usable)
                QS_INFO("lightning: strike #%llu (%s %.0f%%, %s storm, intensity %.0f%%, rate %.1f/game hour, level %.2f)%s",
                        (unsigned long long)g_dir.strikes(), season::toString(in.phase), in.progress * 100.0f,
                        ourStorm ? "season's" : "game's", in.intensity * 100.0f, g_dir.rate(), g_dir.level(),
                        usable ? "" : " [no field: log only]");
        }
        if (in.allowed != prevAllowed || firstAllowed) {
            const char* why = !cfg.enabled               ? "[Lightning] Enabled=0"
                              : !sc.lightningControl     ? "off in the F5 menu (Fulmini)"
                              : !s.regionActive[r]       ? "region off"
                              : !haveW || !wi.mapFound   ? "desert not known yet (weather module)"
                              : wi.desert                ? "in the desert"
                                                         : "";
            QS_INFO("lightning: %s%s", in.allowed ? "strikes allowed" : "no strikes: ", in.allowed ? "" : why);
            prevAllowed = in.allowed;
            firstAllowed = false;
        }

        // feature state for the F5 Info tab
        if (!featureIsDisabled(kKey)) {
            FeatureState st = FeatureState::Active;
            std::string reason;
            if (!g_target.field.valid) st = FeatureState::Off, reason = kNoField;
            else if (!g_target.enabled) st = FeatureState::Off, reason = "spento in targets.ini ([lightning] Enabled=0)";
            else if (!cfg.enabled) st = FeatureState::Off, reason = "spento in QuattroStagioni.ini ([Lightning] Enabled=0)";
            else if (!sc.lightningControl) st = FeatureState::Off, reason = "spento dal menu F5";
            else if (!weatherHookInstalled()) st = FeatureState::Waiting, reason = "in attesa dell'aggancio del meteo";
            if (reason != prevReason || st == FeatureState::Active) featureSet(kKey, st, reason);
            prevReason = reason;
            g_writeOn.store(usable && cfg.enabled && weatherHookInstalled() && !g_failCode.load(), std::memory_order_relaxed);
        }
        if (now - lastDetail >= 1000) {
            lastDetail = now;
            char b[200];
            const double since = g_dir.sinceLastStrikeSec();
            char last[48] = "nessuno";
            if (g_dir.strikes()) snprintf(last, sizeof last, "%.0f s fa", since);
            snprintf(b, sizeof b, "temporale %s%s, livello %.2f, lampi %llu (ultimo %s)%s", in.storm ? "sì" : "no",
                     g_dir.stormHasLightning() ? " con fulmini" : (g_dir.inStorm() ? " senza fulmini" : ""), g_dir.level(),
                     (unsigned long long)g_dir.strikes(), last, desertBlock ? ", deserto o non ancora noto" : "");
            featureDetail(kKey, b);
        }
        g_info.allowed = in.allowed;
        g_info.storm = g_dir.inStorm();
        g_info.hasLightning = g_dir.stormHasLightning();
        g_info.level = g_dir.level();
        g_info.rate = g_dir.rate();
        g_info.chance = g_dir.chance();
        g_info.strikes = g_dir.strikes();
        g_info.storms = g_dir.storms();
        g_info.sinceStrikeSec = g_dir.sinceLastStrikeSec();
        g_info.writes = g_writes.load(std::memory_order_relaxed);
    }
    g_writeOn = false;
    QS_INFO("lightning: stop (%llu storms, %llu strikes, %llu writes)", (unsigned long long)g_dir.storms(),
            (unsigned long long)g_dir.strikes(), (unsigned long long)g_writes.load());
    return 0;
}

}  // namespace
}  // namespace lightningimpl

bool lightningStart(const std::wstring& scriptsDir, bool versionOk) {
    using namespace lightningimpl;
    if (InterlockedCompareExchange(&g_running, 1, 0) != 0) return false;
    featureRegister(kKey, "Fulmini");
    apply::LightningConfig cfg;
    apply::LightningTarget target;
    std::vector<uint8_t> raw;
    std::vector<std::string> warn;
    if (readWholeFile(joinPath(scriptsDir, L"QuattroStagioni.ini"), raw)) {
        Ini ini;
        ini.parse(std::string(raw.begin(), raw.end()));
        cfg.load(ini, warn);
    }
    raw.clear();
    if (readWholeFile(joinPath(joinPath(scriptsDir, L"QuattroStagioni"), L"targets.ini"), raw)) {
        Ini ini;
        ini.parse(std::string(raw.begin(), raw.end()));
        target.load(ini, warn);
    }
    for (const std::string& w : warn) QS_ERR("lightning: %s", w.c_str());
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_cfg = cfg;
        g_target = target;
        g_dir.configure(cfg);
        g_dir.seed(uint32_t(GetTickCount64() * 2246822519ull) ^ (GetCurrentProcessId() << 7));
        g_info = LightningInfo();
        g_info.started = true;
        g_info.fieldSet = target.usable();
        g_info.field = target.field.valid ? target.field.text() : std::string();
        g_info.mode = target.mode;
    }
    if (!versionOk) {
        featureDisable(kKey, "controllo della versione del gioco non superato: nessuna scrittura");
        g_running = 0;
        return false;
    }
    QS_INFO("lightning: start (chance %s, strikes/game hour %s, gap %.1f s, flash %.0f ms); field %s%s; the thunder stays the game's",
            apply::formatPhaseValues(cfg.chancePerStorm).c_str(), apply::formatPhaseValues(cfg.strikesPerHour).c_str(),
            cfg.minGapSec, cfg.pulseMs,
            target.usable() ? (target.field.text() + " mode " + apply::toString(target.mode)).c_str()
                            : "not set (targets.ini [lightning]): log only",
            cfg.enabled ? "" : ", [Lightning] Enabled=0");
    if (!target.field.valid) featureSet(kKey, FeatureState::Off, kNoField);
    if (target.usable()) g_registered = weatherAddPostCompose(&onCompose);
    if (target.usable() && !g_registered) QS_ERR("lightning: cannot register the after-compose callback: nothing will be written");
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_thread = g_stop ? CreateThread(nullptr, 0, worker, nullptr, 0, nullptr) : nullptr;
    if (!g_thread) {
        if (g_registered) weatherRemovePostCompose(&onCompose);
        g_registered = false;
        featureDisable(kKey, "impossibile creare il thread dei fulmini");
        g_running = 0;
        return false;
    }
    return true;
}

void lightningStop() {
    using namespace lightningimpl;
    g_writeOn = false;
    if (g_registered) {
        weatherRemovePostCompose(&onCompose);
        g_registered = false;
        Sleep(50);  // a callback already running on the game thread finishes
    }
    if (!g_thread) return;
    SetEvent(g_stop);
    WaitForSingleObject(g_thread, 3000);
    CloseHandle(g_thread);
    g_thread = nullptr;
    g_running = 0;
}

bool lightningGetInfo(LightningInfo& out) {
    using namespace lightningimpl;
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_info.started) return false;
    out = g_info;
    return true;
}

}  // namespace qs
