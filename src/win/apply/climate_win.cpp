// QuattroStagioni - seasonal temperatures in the game (see climate_win.h).
//
// Ported and adapted from Seasons of Pywel (Khione95) climate.cpp with the author's permission: the
// two CPU climate maps, their layout and the heap search by vtable are his findings.
#include "climate_win.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "../../core/apply/climate.h"
#include "../../core/known_builds.h"
#include "../../core/probe/rtti.h"
#include "../log.h"
#include "../probe/memory.h"
#include "../probe/module.h"
#include "../qs_state.h"
#include "../winutil.h"
#include "guarded_memory.h"
#include "weather_win.h"

namespace qs {
namespace climateimpl {
namespace {

// ---- game facts (Seasons of Pywel research notes and climate.cpp) -------------------------------
const char* const kClassName = ".?AVPAClimateTexture@pa@@";
constexpr uint32_t kSize = 512, kTexels = kSize * kSize;
constexpr float kTexelM = 80.0f;
constexpr uintptr_t kMgrMap = 0xD8;  // the climate manager's map (rescan trigger only)
constexpr uintptr_t kCameraX = 0x1001F8, kCameraZ = 0x100200;
// Cross-check only (logged): the vtable rva Seasons of Pywel uses on each build.
struct ExpectedVtable {
    uint32_t timestamp, rva;
};
constexpr ExpectedVtable kExpected[] = {{0x6AE2C6A6u, 0x5E897C0u}, {0x6AE387F2u, 0x5E8B7C0u}, {0x6AE46DD0u, 0x5E8D890u},
                                     {0x6AE58024u, 0x5E91AD0u}};
constexpr size_t kMaxMaps = 8;
constexpr size_t kChunk = 4u << 20;

#pragma pack(push, 1)
struct MapHeader {
    uintptr_t vtable;
    uint8_t pad0[0x10];
    uintptr_t texels;   // +0x18
    uint32_t count;     // +0x20
    uint8_t pad1[0x0C];
    float mx, mz;       // +0x30, +0x34
    uint32_t w, h;      // +0x38, +0x3C
};
#pragma pack(pop)
static_assert(sizeof(MapHeader) == 0x40, "layout");

struct Map {
    uintptr_t obj = 0, texels = 0;
    std::vector<uint32_t> orig, written, desired;
    bool hasWritten = false;
    float desiredFor = NAN, loggedFor = NAN;
    apply::ClimatePlan plan;
};

// ---- state --------------------------------------------------------------------------------------
std::mutex g_mu;  // g_info
ClimateInfo g_info;
apply::ClimateConfig g_cfg;
probe::ModuleInfo g_mod;
HANDLE g_thread = nullptr, g_stop = nullptr;
volatile LONG g_running = 0;
uintptr_t g_vtable = 0;
std::vector<Map> g_maps;     // worker only
std::vector<uint8_t> g_buf;  // scan buffer (worker only)
apply::GuardedMemory* g_mem = nullptr;

void setState(ClimateModState s, const std::string& why = std::string()) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_info.state == ClimateModState::Disabled) return;
    g_info.state = s;
    if (!why.empty()) g_info.reason = why;
}

enum class Check { Ok, NotMap, BadFormat };

Check checkMap(uintptr_t o, MapHeader* out) {
    MapHeader h;
    if (!probe::mem::readT(o, h) || h.vtable != g_vtable) return Check::NotMap;
    if (h.count != kTexels || h.w != kSize || h.h != kSize || h.texels < 0x10000 || std::fabs(h.mx - kTexelM) > 0.01f ||
        std::fabs(h.mz - kTexelM) > 0.01f || !probe::mem::isReadable(reinterpret_cast<const void*>(h.texels), kTexels * 4))
        return Check::BadFormat;
    if (out) *out = h;
    return Check::Ok;
}

bool readTexels(uintptr_t t, std::vector<uint32_t>& out) {
    out.resize(kTexels);
    return probe::mem::read(t, out.data(), kTexels * 4);
}

// Every map in the heap. badFormat: objects with the vtable but another layout (game update).
std::vector<uintptr_t> scanHeap(size_t& badFormat, double& seconds, size_t& scannedMB) {
    std::vector<uintptr_t> found;
    badFormat = 0;
    scannedMB = 0;
    g_buf.resize(kChunk);
    const uintptr_t b0 = reinterpret_cast<uintptr_t>(g_buf.data()), b1 = b0 + g_buf.size();
    const uint64_t t0 = GetTickCount64();
    size_t chunks = 0;
    bool stop = false;
    probe::mem::forEachHeapRegion([&](uintptr_t base, size_t size) {
        for (size_t off = 0; off < size && !stop; off += kChunk) {
            const size_t n = std::min(kChunk, size - off);
            if (++chunks % 16 == 0) {
                if (WaitForSingleObject(g_stop, 1) == WAIT_OBJECT_0) stop = true;
            }
            if (!probe::mem::read(base + off, g_buf.data(), n)) continue;
            scannedMB += n >> 20;
            const uintptr_t* q = reinterpret_cast<const uintptr_t*>(g_buf.data());
            for (size_t i = 0; i < n / 8; ++i) {
                if (q[i] != g_vtable) continue;
                const uintptr_t a = base + off + i * 8;
                if (a >= b0 && a < b1) continue;  // our own buffer
                Check c = checkMap(a, nullptr);
                if (c == Check::Ok) {
                    if (found.size() < kMaxMaps) found.push_back(a);
                } else if (c == Check::BadFormat) {
                    ++badFormat;
                }
            }
        }
        return !stop;
    });
    seconds = double(GetTickCount64() - t0) / 1000.0;
    return found;
}

bool known(uintptr_t o) {
    for (const Map& m : g_maps)
        if (m.obj == o) return true;
    return false;
}

// Copies the original and builds the plan; false: not plausible (reason in why).
bool attach(uintptr_t o, Map& m, std::string& why) {
    MapHeader h;
    if (checkMap(o, &h) != Check::Ok) return why = "map vanished while attaching", false;
    m = Map();
    m.obj = o;
    m.texels = h.texels;
    if (!readTexels(h.texels, m.orig)) return why = "map texels unreadable", false;
    if (!apply::buildClimatePlan(m.orig, int(kSize), g_cfg, m.plan, &why)) return false;
    QS_INFO("climate: attached map %p (texels %p, %ux%u, %.0f m): desert %zu texels (+%zu fade), game's cold %zu, "
            "day tops %.1f .. %.1f C",
            reinterpret_cast<void*>(o), reinterpret_cast<void*>(h.texels), h.w, h.h, double(h.mx), m.plan.desert, m.plan.fade,
            m.plan.gameCold, double(m.plan.coldestC), double(m.plan.hottestC));
    return true;
}

// Writes the original back where our map still stands.
int restoreAll() {
    int n = 0;
    std::vector<uint32_t> cur;
    for (Map& m : g_maps) {
        if (!m.hasWritten) continue;
        MapHeader h;
        if (checkMap(m.obj, &h) == Check::Ok && h.texels == m.texels && readTexels(m.texels, cur) && cur == m.written) {
            std::string why;
            if (g_mem && g_mem->writeBlock(m.texels, m.orig.data(), kTexels * 4, &why)) ++n;
            else QS_ERR("climate: restore of map %p failed (%s)", reinterpret_cast<void*>(m.obj), why.c_str());
        }
        m.hasWritten = false;
        m.desiredFor = NAN;
    }
    return n;
}

void disable(const std::string& why) {
    const int n = restoreAll();
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_info.state == ClimateModState::Disabled) return;
        g_info.state = ClimateModState::Disabled;
        g_info.reason = why;
        g_info.maps = 0;
        g_info.offsetC = 0;
    }
    QS_ERR("climate: DISABLED: %s%s", why.c_str(), n ? " (original maps restored)" : "");
}

float topC(uint32_t t) { return (float(t >> 24) / 255.0f * 2.0f - 1.0f) * 50.0f; }

DWORD WINAPI worker(LPVOID) {
    if (WaitForSingleObject(g_stop, g_cfg.startDelayMs) == WAIT_OBJECT_0) return 0;
    std::string err;
    if (!probe::describeModule(nullptr, g_mod, &err)) return disable("cannot describe the game module (" + err + ")"), 0;
    const uint32_t ts = g_mod.view.timeDateStamp;
    bool isKnown = false;
    for (const builds::KnownExe& k : builds::kKnownExes) isKnown |= k.timestamp == ts;
    if (!isKnown) {
        char b[96];
        snprintf(b, sizeof b, "exe timestamp 0x%08X is not a known build", ts);
        return disable(b), 0;
    }
    {
        probe::RttiIndex rtti;
        rtti.build(g_mod.view);
        probe::RttiClass cls;
        if (!rtti.find(kClassName, cls) || !cls.primaryVtable()) return disable("RTTI class pa::PAClimateTexture not found"), 0;
        const uint32_t rva = cls.primaryVtable();
        g_vtable = g_mod.base + rva;
        uint32_t expected = 0;
        for (const ExpectedVtable& e : kExpected)
            if (e.timestamp == ts) expected = e.rva;
        QS_INFO("climate: PAClimateTexture vtable rva 0x%X from RTTI (%s)", rva,
                !expected ? "no reference for this build"
                          : (expected == rva ? "matches the known address" : "DIFFERS from the known address: RTTI wins"));
    }
    apply::GuardedMemory mem(g_mod);
    g_mem = &mem;
    setState(ClimateModState::Off);

    uint64_t prev = GetTickCount64(), started = prev, lastScan = 0, lastLog = 0, scanWanted = 1;
    float inEffect = 0, loggedQ = NAN;
    bool prevOn = false, loggedNone = false;
    int writeFails = 0;
    std::vector<uint32_t> cur;
    while (WaitForSingleObject(g_stop, 1000) != WAIT_OBJECT_0) {
        const uint64_t now = GetTickCount64();
        float dt = float(now - prev) / 1000.0f;
        prev = now;
        if (dt > 5.0f) dt = 5.0f;

        season::SeasonState s;
        season::SeasonConfig sc;
        {
            std::lock_guard<std::mutex> lk(shared().mutex);
            s = shared().season;
            sc = shared().config;
        }
        const int r = int(season::Region::Temperate);
        const bool on = g_cfg.enabled && sc.climateControl && s.regionActive[r];
        const float target = on ? apply::climateOffsetC(g_cfg, s.regionPhase[r], s.regionPhaseProgress[r], sc.transitionFraction) : 0.0f;
        if (on != prevOn) {
            if (!on) {
                const int n = restoreAll();
                inEffect = 0;
                QS_INFO("climate: season's temperatures off (F5 Temperature stagionali, [Climate] Enabled or region off): "
                        "%d maps restored to the game's own",
                        n);
            } else {
                QS_INFO("climate: season's temperatures on");
            }
            prevOn = on;
        }
        if (on) inEffect = apply::climateRamp(inEffect, target, g_cfg.maxRatePerSec, dt);
        const float q = std::round(inEffect * 4.0f) / 4.0f;

        // maps: drop the vanished, look again when needed
        const uintptr_t mgr = weatherClimateManager();
        for (size_t i = 0; i < g_maps.size();) {
            MapHeader h;
            Check c = checkMap(g_maps[i].obj, &h);
            if (c != Check::Ok || h.texels != g_maps[i].texels) {
                QS_INFO("climate: map %p gone or rebuilt (world load?): looking again", reinterpret_cast<void*>(g_maps[i].obj));
                g_maps.erase(g_maps.begin() + long(i));
                scanWanted = now + 2000;
                continue;
            }
            ++i;
        }
        if (mgr && now - lastScan > 10000) {
            uintptr_t mm = 0;
            if (probe::mem::readT(mgr + kMgrMap, mm) && mm && !known(mm) && checkMap(mm, nullptr) == Check::Ok) scanWanted = now;
        }
        if (g_maps.empty() && now - lastScan > 15000) scanWanted = std::max<uint64_t>(scanWanted, 1);
        if (now - lastScan > 300000) scanWanted = std::max<uint64_t>(scanWanted, 1);
        if (scanWanted && now >= scanWanted) {
            scanWanted = 0;
            size_t bad = 0, mb = 0;
            double sec = 0;
            std::vector<uintptr_t> found = scanHeap(bad, sec, mb);
            lastScan = GetTickCount64();
            if (WaitForSingleObject(g_stop, 0) == WAIT_OBJECT_0) break;
            if (found.empty() && bad) {
                char b[128];
                snprintf(b, sizeof b, "%zu PAClimateTexture objects with an unknown layout (game update?)", bad);
                disable(b);
                break;
            }
            bool failed = false;
            for (uintptr_t o : found) {
                if (known(o)) continue;
                Map m;
                if (!attach(o, m, err)) {
                    char b[48];
                    snprintf(b, sizeof b, "map %p", reinterpret_cast<void*>(o));
                    disable(std::string(b) + " not usable: " + err);
                    failed = true;
                    break;
                }
                g_maps.push_back(std::move(m));
            }
            if (failed) break;
            QS_INFO("climate: heap scan %.1f s (%zu MB): %zu maps found, %zu attached", sec, mb, found.size(), g_maps.size());
        }
        if (g_maps.empty() && now - started > 180000 && !loggedNone) {
            loggedNone = true;
            disable("no climate map found in 3 minutes (not in a world yet, or the game changed)");
            break;
        }

        // write the season's map
        uint64_t wrote = 0;
        for (Map& m : g_maps) {
            if (!readTexels(m.texels, cur)) continue;
            const bool ours = m.hasWritten && cur == m.written;
            if (!ours && cur != m.orig) {
                // the game rewrote it (or wrote it before us): the new content is the original
                std::vector<uint32_t> keep = m.orig;
                m.orig = cur;
                if (!apply::buildClimatePlan(m.orig, int(kSize), g_cfg, m.plan, &err)) {
                    m.orig = keep;
                    disable("map content changed and is not plausible: " + err);
                    break;
                }
                m.hasWritten = false;
                m.desiredFor = NAN;
                QS_INFO("climate: map %p changed by the game: new original copied", reinterpret_cast<void*>(m.obj));
            }
            if (!on) continue;
            if (!(m.desiredFor == q)) {
                apply::ClimateApplyStats st;
                apply::applyClimate(m.orig, m.plan, g_cfg, q, m.desired, &st);
                m.desiredFor = q;
                if (q == target && !(m.loggedFor == q)) {
                    m.loggedFor = q;
                    QS_INFO("climate: map %p at %+.2f C: %zu texels changed, %zu held by the night floor, %zu below the desert line",
                            reinterpret_cast<void*>(m.obj), double(q), st.changed, st.floored, st.capped);
                }
            }
            if (cur == m.desired) {
                m.written = m.desired;
                m.hasWritten = q != 0.0f;
                continue;
            }
            if (g_mem->writeBlock(m.texels, m.desired.data(), kTexels * 4, &err)) {
                m.written = m.desired;
                m.hasWritten = true;
                ++wrote;
                writeFails = 0;
            } else if (++writeFails >= 5) {
                disable("writes to the climate map keep failing (" + err + ")");
                break;
            }
        }
        {
            std::lock_guard<std::mutex> lk(g_mu);
            if (g_info.state == ClimateModState::Disabled) break;
        }
        const bool moved = std::isnan(loggedQ) || std::fabs(q - loggedQ) >= 1.0f || (q == target && q != loggedQ);
        const bool periodic = g_cfg.logIntervalSec > 0 && now - lastLog >= uint64_t(g_cfg.logIntervalSec * 1000.0f);
        if (on && !g_maps.empty() && (moved || periodic)) {
            QS_INFO("climate: season offset %+.2f C in effect (season %s %.0f%%: target %+.2f C), %zu maps, %llu writes", double(q),
                    season::toString(s.regionPhase[r]), double(s.regionPhaseProgress[r] * 100.0f), double(target), g_maps.size(),
                    (unsigned long long)(g_info.writes + wrote));
            loggedQ = q;
            lastLog = now;
        }

        // menu info: the camera's texel in the first map
        ClimateInfo ci;
        float x = 0, z = 0;
        int row = 0, col = 0;
        if (mgr && !g_maps.empty() && probe::mem::readT(mgr + kCameraX, x) && probe::mem::readT(mgr + kCameraZ, z) &&
            std::isfinite(x) && std::isfinite(z)) {
            const float c = std::floor(x / kTexelM) + float(kSize / 2), rr = std::floor(z / kTexelM) + float(kSize / 2);
            if (c >= 0 && rr >= 0 && c < float(kSize) && rr < float(kSize)) {
                row = int(rr);
                col = int(c);
                const Map& m = g_maps[0];
                const size_t i = size_t(row) * kSize + size_t(col);
                ci.cameraKnown = true;
                ci.cameraKind = int(m.plan.kind(i));
                ci.cameraGameTopC = topC(m.orig[i]);
                ci.cameraTopC = topC(m.hasWritten ? m.written[i] : m.orig[i]);
            }
        }
        std::lock_guard<std::mutex> lk(g_mu);
        g_info.state = on ? ClimateModState::Active : ClimateModState::Off;
        g_info.maps = int(g_maps.size());
        g_info.offsetC = on ? q : 0.0f;
        g_info.targetC = target;
        g_info.cameraKnown = ci.cameraKnown;
        g_info.cameraKind = ci.cameraKind;
        g_info.cameraGameTopC = ci.cameraGameTopC;
        g_info.cameraTopC = ci.cameraTopC;
        g_info.writes += wrote;
    }
    const int n = restoreAll();
    QS_INFO("climate: stop, %d maps restored to the game's own", n);
    g_maps.clear();
    g_mem = nullptr;
    return 0;
}

}  // namespace
}  // namespace climateimpl

bool climateStart(const std::wstring& scriptsDir, bool versionOk) {
    using namespace climateimpl;
    if (InterlockedCompareExchange(&g_running, 1, 0) != 0) return false;
    apply::ClimateConfig cfg;
    std::vector<uint8_t> raw;
    if (readWholeFile(joinPath(scriptsDir, L"QuattroStagioni.ini"), raw)) {
        Ini ini;
        ini.parse(std::string(raw.begin(), raw.end()));
        std::vector<std::string> warn;
        cfg.load(ini, warn);
        for (const std::string& w : warn) QS_ERR("climate: %s", w.c_str());
    }
    g_cfg = cfg;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_info.started = true;
    }
    if (!cfg.enabled) {
        QS_INFO("climate: [Climate] Enabled=0: seasonal temperatures off");
        disable("[Climate] Enabled=0");
        g_running = 0;
        return false;
    }
    if (!versionOk) {
        disable("version guard failed: the game is never written");
        g_running = 0;
        return false;
    }
    QS_INFO("climate: start in %u ms (offsets spring %+.0f, summer %+.0f, late summer %+.0f, autumn %+.0f, winter %+.0f C; "
            "night floor %.0f C; desert >= %.0f C within %d texels, fade %d)",
            cfg.startDelayMs, double(cfg.offsetC[0]), double(cfg.offsetC[1]), double(cfg.offsetC[2]), double(cfg.offsetC[3]),
            double(cfg.offsetC[4]), double(cfg.nightFloorC), double(cfg.desertTopC), cfg.desertRadius, cfg.desertFade);
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_thread = g_stop ? CreateThread(nullptr, 0, worker, nullptr, 0, nullptr) : nullptr;
    if (!g_thread) {
        disable("cannot create the climate thread");
        g_running = 0;
        return false;
    }
    return true;
}

void climateStop() {
    using namespace climateimpl;
    if (!g_thread) return;
    SetEvent(g_stop);
    WaitForSingleObject(g_thread, 5000);
    CloseHandle(g_thread);
    g_thread = nullptr;
    g_running = 0;
}

bool climateGetInfo(ClimateInfo& out) {
    using namespace climateimpl;
    std::lock_guard<std::mutex> lk(g_mu);
    out = g_info;
    return g_info.started;
}

}  // namespace qs
