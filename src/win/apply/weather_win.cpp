// QuattroStagioni - seasonal weather in the game (see weather_win.h).
//
// Ported and adapted from Seasons of Pywel (Khione95) weather.cpp with the author's permission:
// hook points, structure offsets and the "write after the compose" approach are his findings.
#include "weather_win.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <string>
#include <vector>

#include "MinHook.h"
#include "../../core/apply/snow.h"
#include "../../core/known_builds.h"
#include "../../core/probe/pattern.h"
#include "../../core/probe/rtti.h"
#include "../log.h"
#include "../probe/hooks.h"
#include "../probe/memory.h"
#include "../probe/module.h"
#include "../probe/probe.h"
#include "../qs_state.h"
#include "../winutil.h"
#include "guarded_memory.h"

namespace qs {
namespace weatherimpl {
namespace {

// ---- game facts (2.04.02 exe 0x6AE46DD0; Seasons of Pywel research notes) ------------------------
// HUD weather query (actor, u32* flags, bool* indoors). Prologue + body up to the first branch; the
// actor offsets +0x68 / +0x1A0 are part of the signature (a changed structure = no match = off).
const char* const kQuerySig =
    "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 60 49 8B F0 48 8B DA 33 ED 48 8B 41 68 "
    "48 8B B8 A0 01 00 00 89 2A 48 8D 54 24 28 48 8B 4F 08 E8 ?? ?? ?? ?? 90 40 38 6C 24 38 75";
// Climate manager update (rcx = manager, xmm1 = dt): composes the layers, then the effects. The
// signature covers the [mgr+0x60] layers and the +0x20 weather layer it reads.
const char* const kUpdateSig =
    "48 8B C4 53 55 56 57 41 54 41 56 41 57 48 81 EC 90 00 00 00 C5 F8 29 70 B8 C5 F8 29 78 A8 C5 78 29 40 98 "
    "C5 78 29 48 88 C5 78 29 54 24 40 C5 78 28 D1 48 8B F1 33 ED 40 38 69 31 74 ?? 48 8B 41 60 48 8D 48 D8 48 85 "
    "C0 48 0F 44 CD E8 ?? ?? ?? ?? 48 8B CE E8 ?? ?? ?? ?? 48 8B 46 60 48 8D 48 20";
constexpr uintptr_t kLayers = 0x60, kLayerWeather = 0x20, kLayerAtmosphere = 0x28;
constexpr uintptr_t kPrecip = 0x12C;   // precipitation, cloudiness, humidity (3 floats)
constexpr uintptr_t kSnow = 0x16C;     // snow, rain (2 floats)
constexpr uintptr_t kCover = 0xAC;     // atmosphere: _cloudBaseDensity (cloud cover the sky shows)
constexpr uintptr_t kClock = 0x100, kClockHourSlot = 0x100;  // [mgr+0x100] vcall +0x100 -> hour
constexpr uintptr_t kCameraX = 0x1001F8, kCameraZ = 0x100200;
constexpr uintptr_t kMapGuess = 0xD8;  // the climate manager's CPU climate map (checked, else searched)
constexpr uint32_t kMapTexels = 512u * 512u;

typedef void (*QueryFn)(uintptr_t, uint32_t*, uint8_t*);
typedef void (*UpdateFn)(uintptr_t, float);
typedef float (*HourFn)(uintptr_t);

// ---- state --------------------------------------------------------------------------------------
std::mutex g_mu;  // director, info, the compose inputs
apply::WeatherDirector g_dir;
WeatherInfo g_info;
probe::ModuleInfo g_mod;
probe::GuardedMinHook g_hookQuery, g_hookUpdate;
QueryFn g_query = nullptr;
UpdateFn g_update = nullptr;
HANDLE g_thread = nullptr, g_stop = nullptr;
volatile LONG g_running = 0;

std::atomic<bool> g_active{false};       // hook may write
std::atomic<uintptr_t> g_mgr{0};
std::atomic<int64_t> g_flags{-1};
std::atomic<float> g_hour{-1.0f};
float g_bypass = 1.0f, g_winterK = 0.0f;  // under g_mu
float g_desertFade = 1.0f;                // worker only: desert (or unknown) fade for the after-compose features
uint64_t g_lastHourTick = 0;              // game thread only
uintptr_t g_goodHourFn = 0;               // game thread only
std::atomic<uint32_t> g_writeFails{0};
apply::GuardedMemory* g_mem = nullptr;
std::atomic<PostComposeFn> g_post[8];
std::atomic<bool> g_hookOn{false};
std::atomic<float> g_bypassA{1.0f};
std::atomic<int> g_desertA{-1};  // -1 unknown, 0 no, 1 yes

void runPostCompose(uintptr_t mgr, float dt) {
    if (!g_mem) return;
    ComposeContext c;
    c.manager = mgr;
    uintptr_t layers = 0;
    if (probe::mem::readT(mgr + kLayers, layers) && layers) {
        probe::mem::readT(layers + kLayerWeather, c.weather);
        probe::mem::readT(layers + kLayerAtmosphere, c.atmosphere);
    }
    c.hour = g_hour.load(std::memory_order_relaxed);
    c.dtSec = dt;
    c.bypass = g_bypassA.load(std::memory_order_relaxed);
    const int d = g_desertA.load(std::memory_order_relaxed);
    c.desertKnown = d >= 0;
    c.desert = d == 1;
    c.mem = g_mem;
    for (std::atomic<PostComposeFn>& f : g_post) {
        PostComposeFn fn = f.load(std::memory_order_acquire);
        if (fn) fn(c);
    }
}

void disable(const std::string& why) {
    g_active = false;
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_info.state == WeatherModState::Disabled) return;
    g_info.state = WeatherModState::Disabled;
    g_info.reason = why;
    QS_ERR("weather: DISABLED: %s", why.c_str());
}

bool inCode(uintptr_t fn) {
    probe::TargetCheck c = probe::checkHookTarget(g_mod, fn);
    return c == probe::TargetCheck::Ok || c == probe::TargetCheck::OkJmpStub;
}

// The game's hour through the clock's own virtual (only called when the function is game code).
void readHour(uintptr_t mgr) {
    uintptr_t clock = 0, vt = 0, fn = 0;
    if (!probe::mem::readT(mgr + kClock, clock) || !clock || !probe::mem::readT(clock, vt) ||
        !probe::mem::readT(vt + kClockHourSlot, fn))
        return;
    if (fn != g_goodHourFn) {
        if (!inCode(fn)) return;
        g_goodHourFn = fn;
    }
    float h = reinterpret_cast<HourFn>(fn)(clock);
    if (std::isfinite(h) && h >= 0.0f && h <= 24.0f) g_hour = h;
}

bool readF(uintptr_t a, float* v, size_t n) { return probe::mem::read(a, v, n * sizeof(float)); }

// After the game composed its weather: the season's values.
void afterCompose(uintptr_t mgr) {
    g_mgr = mgr;
    const uint64_t now = GetTickCount64();
    if (now - g_lastHourTick >= 250) {
        g_lastHourTick = now;
        readHour(mgr);
    }
    if (!g_active.load(std::memory_order_relaxed)) return;
    uintptr_t layers = 0, w = 0, atm = 0;
    if (!probe::mem::readT(mgr + kLayers, layers) || !layers || !probe::mem::readT(layers + kLayerWeather, w) || !w) return;
    if (!probe::mem::readT(layers + kLayerAtmosphere, atm)) atm = 0;
    float pch[3], sr[2], cover = 0;
    if (!readF(w + kPrecip, pch, 3) || !readF(w + kSnow, sr, 2)) return;
    apply::WeatherLayer g;
    g.precipitation = pch[0];
    g.cloudiness = pch[1];
    g.humidity = pch[2];
    g.snow = sr[0];
    g.rain = sr[1];
    g.hasCover = atm && readF(atm + kCover, &cover, 1);
    g.cover = cover;
    apply::WeatherLayer o;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        o = g_dir.compose(g, g_winterK, g_bypass);
        g_info.game = g;
        g_info.out = o;
    }
    std::string why;
    bool ok = true;
    int wrote = 0;
    if (o.precipitation != g.precipitation || o.cloudiness != g.cloudiness || o.humidity != g.humidity) {
        float v[3] = {o.precipitation, o.cloudiness, o.humidity};
        ok &= g_mem->write(w + kPrecip, v, sizeof v, &why);
        ++wrote;
    }
    if (o.snow != g.snow || o.rain != g.rain) {
        float v[2] = {o.snow, o.rain};
        ok &= g_mem->write(w + kSnow, v, sizeof v, &why);
        ++wrote;
    }
    if (g.hasCover && o.cover != g.cover) {
        ok &= g_mem->write(atm + kCover, &o.cover, sizeof o.cover, &why);
        ++wrote;
    }
    if (!ok) {
        if (g_writeFails.fetch_add(1) + 1 >= 50) {
            g_active = false;  // the worker logs it
        }
    } else if (wrote) {
        g_writeFails = 0;
        std::lock_guard<std::mutex> lk(g_mu);
        ++g_info.writes;
    }
}

void hookUpdate(uintptr_t mgr, float dt) {
    g_update(mgr, dt);
    afterCompose(mgr);
    runPostCompose(mgr, dt);
}

void hookQuery(uintptr_t actor, uint32_t* flags, uint8_t* indoors) {
    g_query(actor, flags, indoors);
    if (flags) g_flags = int64_t(*flags);  // the game's own out parameter, just written
}

// ---- climate map (desert) -----------------------------------------------------------------------
bool isClimateMap(uintptr_t o, const probe::RttiIndex& rtti) {
    uintptr_t vt = 0, texels = 0;
    uint32_t count = 0, w = 0, h = 0;
    if (!o || !probe::mem::readT(o, vt) || !probe::mem::readT(o + 0x18, texels) || !probe::mem::readT(o + 0x20, count) ||
        !probe::mem::readT(o + 0x38, w) || !probe::mem::readT(o + 0x3C, h))
        return false;
    if (count != kMapTexels || w != 512 || h != 512 || texels < 0x10000) return false;
    uint64_t vrva = g_mod.view.toRva(vt);
    const probe::ColInfo* c = vrva == UINT64_MAX ? nullptr : rtti.colForVtable(uint32_t(vrva));
    return c && probe::demangleClassName(rtti.typeName(c->tdRva)).find("ClimateTexture") != std::string::npos;
}

uintptr_t findClimateMap(uintptr_t mgr, const probe::RttiIndex& rtti) {
    uintptr_t o = 0;
    if (probe::mem::readT(mgr + kMapGuess, o) && isClimateMap(o, rtti)) return o;
    for (uintptr_t off = 0; off < 0x400; off += 8)
        if (off != kMapGuess && probe::mem::readT(mgr + off, o) && isClimateMap(o, rtti)) return o;
    return 0;
}

// Desert around the camera; false when it cannot be told (camera or map unreadable).
bool desertNow(uintptr_t mgr, uintptr_t map, int radius, float thresholdC, bool& desert, float& hottest) {
    float x = 0, z = 0;
    uintptr_t texels = 0;
    if (!probe::mem::readT(mgr + kCameraX, x) || !probe::mem::readT(mgr + kCameraZ, z) || !probe::mem::readT(map + 0x18, texels))
        return false;
    int row, col;
    if (!apply::climateCell(x, z, row, col)) return false;
    std::vector<uint32_t> block;
    std::vector<uint32_t> line(size_t(2 * radius + 1));
    for (int r = row - radius; r <= row + radius; ++r) {
        if (r < 0 || r >= apply::kClimateMapSize) continue;
        int c0 = std::max(0, col - radius), c1 = std::min(apply::kClimateMapSize - 1, col + radius);
        size_t n = size_t(c1 - c0 + 1);
        if (!probe::mem::read(texels + (size_t(r) * apply::kClimateMapSize + size_t(c0)) * 4, line.data(), n * 4)) return false;
        block.insert(block.end(), line.begin(), line.begin() + long(n));
    }
    desert = apply::desertBlock(block, thresholdC, &hottest);
    return true;
}

// ---- worker -------------------------------------------------------------------------------------
bool findUnique(const char* name, const char* sig, uintptr_t& out, std::string& why) {
    probe::Pattern p;
    if (!probe::parsePattern(sig, p, &why)) return false;
    std::vector<uint64_t> hits = probe::scanModule(g_mod, p, false, 4);
    if (hits.size() != 1) {
        why = std::string(name) + (hits.empty() ? ": signature not found (game update?)" : ": signature not unique");
        return false;
    }
    out = g_mod.base + uintptr_t(hits[0]);
    QS_INFO("weather: %s found: %s", name, g_mod.view.describe(hits[0]).c_str());
    return true;
}

bool hook(probe::GuardedMinHook& h, uintptr_t target, void* detour, void** orig, const char* name, std::string& why) {
    if (!h.install(g_mod, target, detour, orig, &why)) {
        why = std::string(name) + ": hook refused (" + why + ")";
        return false;
    }
    MH_STATUS st = MH_EnableHook(reinterpret_cast<void*>(target));
    if (st != MH_OK) {
        h.remove();
        why = std::string(name) + ": MH_EnableHook " + MH_StatusToString(st);
        return false;
    }
    return true;
}

void flushLog() {
    for (const std::string& l : g_dir.takeLog()) QS_INFO("%s", l.c_str());
}

DWORD WINAPI worker(LPVOID) {
    const apply::WeatherConfig cfg = g_dir.config();
    if (WaitForSingleObject(g_stop, cfg.startDelayMs) == WAIT_OBJECT_0) return 0;
    std::string err;
    if (!probe::describeModule(nullptr, g_mod, &err)) return disable("cannot describe the game module (" + err + ")"), 0;
    bool known = false;
    for (const builds::KnownExe& k : builds::kKnownExes) known |= k.timestamp == g_mod.view.timeDateStamp;
    if (!known) {
        char b[96];
        snprintf(b, sizeof b, "exe timestamp 0x%08X is not a known build", g_mod.view.timeDateStamp);
        return disable(b), 0;
    }
    uintptr_t update = 0, query = 0;
    if (!findUnique("climate update", kUpdateSig, update, err)) return disable(err), 0;
    probe::RttiIndex rtti;
    rtti.build(g_mod.view);
    apply::GuardedMemory mem(g_mod);
    g_mem = &mem;
    if (!hook(g_hookUpdate, update, reinterpret_cast<void*>(&hookUpdate), reinterpret_cast<void**>(&g_update), "climate update", err))
        return disable(err), 0;
    bool hud = findUnique("HUD weather query", kQuerySig, query, err) &&
               hook(g_hookQuery, query, reinterpret_cast<void*>(&hookQuery), reinterpret_cast<void**>(&g_query), "HUD weather query", err);
    if (!hud) QS_ERR("weather: HUD weather not watched (%s); the season's weather still runs", err.c_str());
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_info.hudHooked = hud;
    }
    g_hookOn = true;
    QS_INFO("weather: hooks on; after-compose consumers (lightning, lighting, daylength, particles) can run");

    apply::GameClock clock;
    uintptr_t map = 0, mapMgr = 0;
    uint64_t prev = GetTickCount64(), started = prev, lastDesert = 0, lastLog = 0, lastHud = 0, hourSeen = 0;
    bool desert = false, desertKnown = false, loggedFallback = false, loggedNoMap = false, prevOn = false, prevDesert = false;
    int64_t loggedFlags = -2;
    float hottest = 0;
    while (WaitForSingleObject(g_stop, 250) != WAIT_OBJECT_0) {
        const uint64_t now = GetTickCount64();
        double dt = double(now - prev) / 1000.0;
        prev = now;
        if (dt > 1.0) dt = 1.0;
        if (g_writeFails.load() >= 50) {
            disable("writes to the weather layer keep failing");
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
        const bool on = cfg.enabled && sc.weatherControl && s.regionActive[r];

        // game hours: the game's clock, or (until it is read) real time at the season's day length
        double hours = 0;
        bool jump = false;
        const float h = g_hour.load();
        if (h >= 0) {
            apply::GameClock::Step st = clock.feed(h);
            hours = st.hours;
            jump = st.jump;
            hourSeen = now;
        } else if (now - started > 30000) {
            if (!loggedFallback) {
                QS_ERR("weather: the game's hour is not readable: spells follow real time (%.0f real minutes a day)", sc.realMinutesPerDay);
                loggedFallback = true;
            }
            hours = dt * 24.0 / (std::max(1.0f, sc.realMinutesPerDay) * 60.0);
        }
        if (jump) QS_INFO("weather: the game's clock jumped %.1f game hours (to %.2f)", hours, h);

        // desert, once a second
        const uintptr_t mgr = g_mgr.load();
        if (mgr && now - lastDesert >= 1000) {
            lastDesert = now;
            if (!map || mgr != mapMgr || !isClimateMap(map, rtti)) {
                map = findClimateMap(mgr, rtti);
                mapMgr = mgr;
                if (map) QS_INFO("weather: climate map found (%p, manager %p)", reinterpret_cast<void*>(map), reinterpret_cast<void*>(mgr));
            }
            desertKnown = map && desertNow(mgr, map, cfg.desertRadius, cfg.desertTopC, desert, hottest);
            if (!map && now - started > 90000 && !loggedNoMap) {
                loggedNoMap = true;
                disable("climate map not found: the desert cannot be told, weather left to the game");
                break;
            }
        }
        const bool bypassAll = !on || !desertKnown || desert;
        if (desertKnown && desert != prevDesert) {
            QS_INFO("weather: %s the desert (hottest day top %.1f C): %s", desert ? "entering" : "leaving", hottest,
                    desert ? "the game's own weather" : "the season's weather");
            prevDesert = desert;
        }
        if (on != prevOn) {
            QS_INFO("weather: season's weather %s", on ? "on" : "off (F5 Meteo stagionale, [Weather] Enabled or region off)");
            prevOn = on;
        }

        const int64_t flags = g_flags.load();
        std::lock_guard<std::mutex> lk(g_mu);
        apply::WeatherSeason ws;
        ws.phase = s.regionPhase[r];
        ws.progress = s.regionPhaseProgress[r];
        ws.transitionFraction = sc.transitionFraction;
        ws.winterK = apply::winterIntensity(s, sc);
        if (on) g_dir.advance(hours, ws);
        g_winterK = ws.winterK;
        const float step = cfg.desertFadeSec > 0 ? float(dt) / cfg.desertFadeSec : 1.0f;
        g_bypass = bypassAll ? std::min(1.0f, g_bypass + step) : std::max(0.0f, g_bypass - step);
        g_desertFade = (!desertKnown || desert) ? std::min(1.0f, g_desertFade + step) : std::max(0.0f, g_desertFade - step);
        g_bypassA = g_desertFade;
        g_desertA = desertKnown ? (desert ? 1 : 0) : -1;
        g_active = g_info.state != WeatherModState::Disabled;
        flushLog();

        if (flags != loggedFlags && now - lastHud >= 2000) {
            lastHud = now;
            loggedFlags = flags;
            const int idx = flags < 0 ? -1 : apply::hudWeatherIndex(uint32_t(flags));
            QS_INFO("weather: HUD %s (flags %llX)%s", apply::hudWeatherName(idx), (unsigned long long)(flags < 0 ? 0 : flags),
                    flags >= 0 && (uint32_t(flags) & apply::kHudStormBit) ? " - Storm bit set" : "");
        }
        if (cfg.logIntervalSec > 0 && now - lastLog >= uint64_t(cfg.logIntervalSec * 1000.0f)) {
            lastLog = now;
            const apply::WeatherLayer& gg = g_info.game;
            const apply::WeatherLayer& oo = g_info.out;
            QS_INFO("weather: %s %.0f%% (%.1f h left, season %s, winter %.0f%%), desert %s, bypass %.2f | game rain %.2f snow %.2f "
                    "cover %.2f cloud %.2f -> rain %.2f snow %.2f cover %.2f cloud %.2f precip %.2f humid %.2f, %llu writes",
                    apply::toString(g_dir.spell().kind), g_dir.spell().intensity * 100.0f, g_dir.hoursLeft(),
                    season::toString(ws.phase), ws.winterK * 100.0f, !desertKnown ? "unknown" : (desert ? "yes" : "no"), g_bypass,
                    gg.rain, gg.snow, gg.cover, gg.cloudiness, oo.rain, oo.snow, oo.cover, oo.cloudiness, oo.precipitation,
                    oo.humidity, (unsigned long long)g_info.writes);
        }
        if (g_info.state != WeatherModState::Disabled) g_info.state = on ? WeatherModState::Active : WeatherModState::Off;
        g_info.hudFlags = flags;
        g_info.hasSpell = g_dir.hasSpell();
        g_info.spell = g_dir.spell();
        g_info.hoursLeft = g_dir.hoursLeft();
        g_info.snowing = apply::snowShare(ws.winterK) >= 0.5f;
        g_info.desert = desertKnown && desert;
        g_info.mapFound = map != 0;
        g_info.hottestC = hottest;
        g_info.bypass = g_bypass;
        g_info.hour = h;
        g_info.clockFromGame = h >= 0 && now - hourSeen < 5000;
        if (g_info.state == WeatherModState::Disabled) break;
    }
    // Stop writing first, then unhook: the game recomposes its own weather on the next tick.
    g_active = false;
    g_hookOn = false;
    g_hookQuery.remove();
    g_hookUpdate.remove();
    Sleep(50);
    g_mem = nullptr;
    QS_INFO("weather: stop");
    return 0;
}

}  // namespace
}  // namespace weatherimpl

const char* toString(WeatherModState s) {
    switch (s) {
        case WeatherModState::Waiting: return "waiting";
        case WeatherModState::Active: return "active";
        case WeatherModState::Off: return "off";
        case WeatherModState::Disabled: return "disabled";
    }
    return "?";
}

bool weatherStart(const std::wstring& scriptsDir, bool versionOk) {
    using namespace weatherimpl;
    if (InterlockedCompareExchange(&g_running, 1, 0) != 0) return false;
    apply::WeatherConfig cfg;
    std::vector<uint8_t> raw;
    if (readWholeFile(joinPath(scriptsDir, L"QuattroStagioni.ini"), raw)) {
        Ini ini;
        ini.parse(std::string(raw.begin(), raw.end()));
        std::vector<std::string> warn;
        cfg.load(ini, warn);
        for (const std::string& w : warn) QS_ERR("weather: %s", w.c_str());
    }
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_dir.configure(cfg);
        g_dir.seed(uint32_t(GetTickCount64() * 2654435761ull) ^ GetCurrentProcessId());
        g_info.started = true;
    }
    if (!cfg.enabled)  // the hook still runs (desert check, clock, after-compose features); nothing of the weather is written
        QS_INFO("weather: [Weather] Enabled=0: seasonal weather off, the hook stays for the other features");
    if (!versionOk) {
        disable("version guard failed: the game is never written");
        g_running = 0;
        return false;
    }
    probeSetProvider("climate_manager", &weatherClimateManager);  // probes.ini Provider=climate_manager
    QS_INFO("weather: start in %u ms (ramp %.2f/h, clear cover %.2f, desert >= %.1f C within %d texels)", cfg.startDelayMs,
            cfg.rampPerHour, cfg.clearCover, cfg.desertTopC, cfg.desertRadius);
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_thread = g_stop ? CreateThread(nullptr, 0, worker, nullptr, 0, nullptr) : nullptr;
    if (!g_thread) {
        disable("cannot create the weather thread");
        g_running = 0;
        return false;
    }
    return true;
}

void weatherStop() {
    using namespace weatherimpl;
    if (!g_thread) return;
    SetEvent(g_stop);
    WaitForSingleObject(g_thread, 3000);
    CloseHandle(g_thread);
    g_thread = nullptr;
    g_running = 0;
}

uintptr_t weatherClimateManager() { return weatherimpl::g_mgr.load(); }

bool weatherAddPostCompose(PostComposeFn fn) {
    using namespace weatherimpl;
    if (!fn) return false;
    for (std::atomic<PostComposeFn>& f : g_post)
        if (f.load() == fn) return true;
    for (std::atomic<PostComposeFn>& f : g_post) {
        PostComposeFn expected = nullptr;
        if (f.compare_exchange_strong(expected, fn)) return true;
    }
    return false;
}

void weatherRemovePostCompose(PostComposeFn fn) {
    using namespace weatherimpl;
    for (std::atomic<PostComposeFn>& f : g_post) {
        PostComposeFn expected = fn;
        f.compare_exchange_strong(expected, nullptr);
    }
}

bool weatherHookInstalled() { return weatherimpl::g_hookOn.load(); }

bool weatherGetInfo(WeatherInfo& out) {
    using namespace weatherimpl;
    std::lock_guard<std::mutex> lk(g_mu);
    out = g_info;
    return g_info.started;
}

}  // namespace qs
