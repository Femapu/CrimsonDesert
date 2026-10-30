// QuattroStagioni - winter snow thresholds in the game (see snow_win.h).
#include "snow_win.h"

#include <windows.h>

#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

#include "../../core/known_builds.h"
#include "../../core/probe/pattern.h"
#include "../../core/probe/rtti.h"
#include "../log.h"
#include "../probe/memory.h"
#include "../probe/module.h"
#include "../qs_state.h"
#include "../winutil.h"
#include "guarded_memory.h"

namespace qs {
namespace snowimpl {
namespace {

// Found with the local probes on the 2.04.00 executables (docs/recon/test_locali.md): the
// registration of each cvar, "mov r8d, <name hash>; lea rcx, [rip + cvar]". Same as probes.ini.
struct CvarSig {
    const char* name;
    const char* pattern;
    int64_t sigOffset;       // to the lea
    size_t dispOffset, len;  // rip:3:7
};
const CvarSig kSigs[2] = {
    {"SnowStart", "41 B8 7B 53 B3 20 48 8D 0D ?? ?? ?? ??", 6, 3, 7},
    {"SnowFull", "41 B8 14 0D C4 5F 48 8D 0D ?? ?? ?? ??", 6, 3, 7},
};
constexpr uint32_t kValueOff = 0x38, kDefaultOff = 0x3C;
constexpr const char* kClass = "GlobalVariableFloat";  // pa::GlobalVariableFloat

std::mutex g_mu;  // g_ctl, g_info
apply::SnowController g_ctl;
SnowInfo g_info;
probe::ModuleInfo g_mod;
HANDLE g_thread = nullptr, g_stop = nullptr;
volatile LONG g_running = 0;

void flushLog() {
    for (const std::string& l : g_ctl.takeLog()) QS_INFO("%s", l.c_str());
}

void publish() {
    g_info.status = g_ctl.status();
}

void disable(const std::string& why) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_ctl.disable(why);
    publish();
    flushLog();
}

// Finds one cvar object: unique signature target inside the exe, RTTI class check.
bool findCvar(const CvarSig& s, const probe::RttiIndex& rtti, uintptr_t& obj, std::string& why) {
    probe::Pattern p;
    if (!probe::parsePattern(s.pattern, p, &why)) return false;
    std::vector<uint64_t> hits = probe::scanModule(g_mod, p, false, 8);
    std::vector<uint64_t> targets;
    for (uint64_t h : hits) {
        uint64_t r = h + uint64_t(s.sigOffset);
        int32_t disp;
        if (!g_mod.view.read(r + s.dispOffset, disp)) continue;
        targets.push_back(probe::ripTarget(r, s.len, disp));
    }
    if (targets.empty()) {
        why = std::string(s.name) + ": signature not found (game update?)";
        return false;
    }
    if (!std::all_of(targets.begin(), targets.end(), [&](uint64_t t) { return t == targets[0]; })) {
        why = std::string(s.name) + ": signature ambiguous (" + std::to_string(hits.size()) + " matches)";
        return false;
    }
    obj = g_mod.base + uintptr_t(targets[0]);
    uintptr_t vt = 0;
    if (!probe::mem::readT(obj, vt)) {
        why = std::string(s.name) + ": object not readable";
        return false;
    }
    uint64_t vrva = g_mod.view.toRva(vt);
    const probe::ColInfo* c = vrva == UINT64_MAX ? nullptr : rtti.colForVtable(uint32_t(vrva));
    std::string cls = c ? probe::demangleClassName(rtti.typeName(c->tdRva)) : "";
    if (cls.find(kClass) == std::string::npos) {
        why = std::string(s.name) + ": class check failed (found '" + (cls.empty() ? "unknown" : cls) + "', expected pa::" + kClass + ")";
        return false;
    }
    QS_INFO("snow: %s found: %s, class %s", s.name, g_mod.view.describe(targets[0]).c_str(), cls.c_str());
    return true;
}

DWORD WINAPI worker(LPVOID) {
    const uint32_t delay = g_ctl.config().startDelayMs;
    if (WaitForSingleObject(g_stop, delay) == WAIT_OBJECT_0) return 0;
    std::string err;
    if (!probe::describeModule(nullptr, g_mod, &err)) {
        disable("cannot describe the game module (" + err + ")");
        return 0;
    }
    bool known = false;
    for (const builds::KnownExe& k : builds::kKnownExes) known |= k.timestamp == g_mod.view.timeDateStamp;
    if (!known) {
        char b[96];
        snprintf(b, sizeof b, "exe timestamp 0x%08X is not a known build", g_mod.view.timeDateStamp);
        disable(b);
        return 0;
    }
    probe::RttiIndex rtti;
    rtti.build(g_mod.view);
    uintptr_t obj[2] = {};
    for (int i = 0; i < 2; ++i) {
        if (!findCvar(kSigs[i], rtti, obj[i], err)) {
            disable(err);
            return 0;
        }
    }
    apply::GuardedMemory mem(g_mod);
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_ctl.attach({obj[0] + kValueOff, obj[0] + kDefaultOff}, {obj[1] + kValueOff, obj[1] + kDefaultOff}, mem);
        publish();
        flushLog();
    }
    uint64_t prev = GetTickCount64();
    while (WaitForSingleObject(g_stop, 250) != WAIT_OBJECT_0) {
        season::SeasonState s;
        season::SeasonConfig sc;
        {
            std::lock_guard<std::mutex> lk(shared().mutex);
            s = shared().season;
            sc = shared().config;
        }
        const uint64_t now = GetTickCount64();
        double dt = double(now - prev) / 1000.0;
        prev = now;
        if (dt > 1.0) dt = 1.0;  // a hitch must not become a jump
        const bool on = g_ctl.config().enabled && sc.snowAccumulation;
        const float k = apply::winterIntensity(s, sc);
        const int r = int(season::Region::Temperate);
        const std::string label = std::string(season::phaseNameIt(s.regionPhase[r])) + " " +
                                  std::to_string(int(s.regionPhaseProgress[r] * 100.0f + 0.5f)) + "%";
        std::lock_guard<std::mutex> lk(g_mu);
        try {
            g_ctl.tick(on, k, dt, mem, label);
        } catch (...) {
            g_ctl.disable("unexpected exception in the snow tick");
        }
        publish();
        flushLog();
        if (g_ctl.status().state == apply::SnowState::Disabled) break;
    }
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_ctl.status().attached) {
        g_ctl.restore(mem);
        QS_INFO("snow: stop");
    }
    publish();
    flushLog();
    return 0;
}

}  // namespace
}  // namespace snowimpl

bool snowStart(const std::wstring& scriptsDir, bool versionOk) {
    using namespace snowimpl;
    if (InterlockedCompareExchange(&g_running, 1, 0) != 0) return false;
    apply::SnowConfig cfg;
    std::vector<uint8_t> raw;
    if (readWholeFile(joinPath(scriptsDir, L"QuattroStagioni.ini"), raw)) {
        Ini ini;
        ini.parse(std::string(raw.begin(), raw.end()));
        std::vector<std::string> warn;
        cfg.load(ini, warn);
        for (const std::string& w : warn) QS_ERR("snow: %s", w.c_str());
    }
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_ctl.configure(cfg);
        g_info.started = true;
        g_info.configEnabled = cfg.enabled;
        g_info.winterStart = cfg.effectiveStart();
        g_info.winterFull = cfg.effectiveFull();
    }
    if (!cfg.enabled) {
        QS_INFO("snow: [Snow] Enabled=0: winter snow off");
        disable("[Snow] Enabled=0");
        g_running = 0;
        return false;
    }
    if (!versionOk) {
        disable("version guard failed: the game is never written");
        g_running = 0;
        return false;
    }
    QS_INFO("snow: winter SnowStart %.1f SnowFull %.1f (MaxSnowStart %.1f, MaxRate %.2f/s), start in %u ms", cfg.effectiveStart(),
            cfg.effectiveFull(), cfg.maxStart, cfg.maxRate, cfg.startDelayMs);
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_thread = g_stop ? CreateThread(nullptr, 0, worker, nullptr, 0, nullptr) : nullptr;
    if (!g_thread) {
        disable("cannot create the snow thread");
        g_running = 0;
        return false;
    }
    return true;
}

void snowStop() {
    using namespace snowimpl;
    if (!g_thread) return;
    SetEvent(g_stop);
    WaitForSingleObject(g_thread, 3000);
    CloseHandle(g_thread);
    g_thread = nullptr;
    g_running = 0;
}

bool snowGetInfo(SnowInfo& out) {
    using namespace snowimpl;
    std::lock_guard<std::mutex> lk(g_mu);
    out = g_info;
    return g_info.started;
}

}  // namespace qs
