// QuattroStagioni - season applier, Win32 side (see apply.h for the integration contract).
//
// One background thread: finds the [object.*] sections of targets.ini with the probe helpers and
// feeds the portable controller (core/apply/applier.h) at IntervalMs. Game threads are only ever
// touched through a capture stub (Instance=capture, needs [General] AllowHooks=1), which records
// pointers lock-free; everything else reads and writes from this thread through GuardedMemory.
#include "apply.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "../../core/apply/targets.h"
#include "../../core/probe/rtti.h"
#include "../log.h"
#include "../probe/hooks.h"
#include "../probe/memory.h"
#include "../probe/module.h"
#include "../qs_state.h"
#include "../winutil.h"
#include "guarded_memory.h"

namespace qs {
namespace applyimpl {
namespace {

using apply::InstanceKind;
using apply::ObjectSpec;

// Vtable values are kept XOR-ed in our memory so our own heap scans never find them.
constexpr uintptr_t kVtXor = 0x3C5A3C5A3C5A3C5Aull;

struct ObjectRt {
    uintptr_t classVtX = 0;       // primary vtable of Class ^ kVtXor (0 = no class)
    bool classResolved = false;   // RTTI lookup done (found or not)
    bool anchorResolved = false;
    uint64_t anchorRva = UINT64_MAX;
    std::unique_ptr<probe::CaptureSink> sink;
    std::unique_ptr<probe::CaptureHook> hook;
    bool hookTried = false;
    uint64_t nextResolve = 0;
    uint32_t attempts = 0;
    bool loggedMissing = false;
};

apply::TargetsConfig g_cfg;
apply::Applier g_applier;
probe::ModuleInfo g_mod;
probe::RttiIndex g_rtti;
std::vector<ObjectRt> g_obj;
std::mutex g_mu;  // g_applier, g_status
apply::ApplyStatus g_status;
HANDLE g_thread = nullptr, g_stop = nullptr, g_done = nullptr;
volatile LONG g_running = 0;
bool g_started = false;

uint64_t nowMs() { return GetTickCount64(); }

void flushLog() {
    std::vector<std::string> lines = g_applier.takeLog();
    for (const std::string& l : lines) QS_INFO("apply: %s", l.c_str());
}

bool passesChecks(const ObjectSpec& o, uintptr_t obj) {
    for (const apply::ObjectCheck& c : o.checks) {
        uint8_t b[8] = {};
        if (!probe::mem::read(obj + c.offset, b, apply::fieldBytes(c.kind))) return false;
        double v = apply::decodeField(c.kind, b);
        if (!(v >= c.minV && v <= c.maxV)) return false;
    }
    return true;
}

// Class name of the object at `obj` (via its vtable), empty if unknown.
std::string classOf(uintptr_t obj) {
    uintptr_t vt = 0;
    if (!probe::mem::readT(obj, vt) || !g_rtti.built()) return "";
    uint64_t rva = g_mod.view.toRva(vt);
    const probe::ColInfo* c = rva == UINT64_MAX ? nullptr : g_rtti.colForVtable(uint32_t(rva));
    return c ? probe::demangleClassName(g_rtti.typeName(c->tdRva)) : "";
}

// Heap scan for objects whose first qword is the vtable (same approach as the probes' Instance=scan;
// our own thread stack is skipped, transient copies are dropped by the re-read in the caller).
std::vector<uintptr_t> heapScan(uintptr_t vtX, size_t maxHits, uint32_t maxMB) {
    std::vector<uintptr_t> hits;
    const size_t kChunk = 1 << 20;
    uint8_t* buf = static_cast<uint8_t*>(VirtualAlloc(nullptr, kChunk, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!buf) return hits;
    ULONG_PTR stLo = 0, stHi = 0;
    GetCurrentThreadStackLimits(&stLo, &stHi);
    uint64_t budget = uint64_t(maxMB) << 20;
    probe::mem::forEachHeapRegion([&](uintptr_t base, size_t size) {
        if (base == reinterpret_cast<uintptr_t>(buf) || (base <= stLo && stLo < base + size)) return true;
        for (size_t off = 0; off < size && budget; off += kChunk) {
            size_t n = std::min(kChunk, size - off);
            if (n > budget) n = size_t(budget);
            budget -= n;
            if (!probe::mem::read(base + off, buf, n)) continue;
            const uintptr_t needle = vtX ^ kVtXor;
            for (size_t i = 0; i + 8 <= n; i += 8) {
                uintptr_t q;
                std::memcpy(&q, buf + i, 8);
                if (q == needle) {
                    hits.push_back(base + off + i);
                    if (hits.size() >= maxHits) return false;
                }
            }
        }
        return budget > 0;
    });
    VirtualFree(buf, 0, MEM_RELEASE);
    return hits;
}

bool resolveClass(size_t oi) {
    const ObjectSpec& o = g_cfg.objects[oi];
    ObjectRt& rt = g_obj[oi];
    if (o.className.empty()) return true;
    if (rt.classResolved) return rt.classVtX != 0;
    rt.classResolved = true;
    probe::RttiClass cls;
    if (!g_rtti.find(o.className, cls) || !cls.primaryVtable()) {
        QS_ERR("apply: object %s: class %s not found in RTTI (wrong name or new game build)", o.name.c_str(), o.className.c_str());
        return false;
    }
    rt.classVtX = (g_mod.base + cls.primaryVtable()) ^ kVtXor;
    QS_INFO("apply: object %s: class %s vtable rva 0x%X", o.name.c_str(), cls.pretty.c_str(), cls.primaryVtable());
    return true;
}

bool resolveAnchor(size_t oi) {
    const ObjectSpec& o = g_cfg.objects[oi];
    ObjectRt& rt = g_obj[oi];
    if (rt.anchorResolved) return rt.anchorRva != UINT64_MAX;
    rt.anchorResolved = true;
    if (!o.signatureText.empty()) {
        std::vector<uint64_t> hits = probe::scanModule(g_mod, o.signature, o.sigInData, 16);
        std::vector<uint64_t> targets;
        for (uint64_t h : hits) {
            uint64_t r = h + uint64_t(o.sigOffset);
            if (o.resolve.enabled) {
                int32_t disp;
                if (!g_mod.view.read(r + o.resolve.dispOffset, disp)) continue;
                r = probe::ripTarget(r, o.resolve.insnLen, disp);
            }
            targets.push_back(r);
        }
        bool same = !targets.empty() && std::all_of(targets.begin(), targets.end(), [&](uint64_t t) { return t == targets[0]; });
        if (!same) {
            QS_ERR("apply: object %s: signature -> %zu match(es)%s: object disabled", o.name.c_str(), hits.size(),
                   targets.size() > 1 ? " with different targets (ambiguous)" : "");
            return false;
        }
        rt.anchorRva = targets[0];
    } else if (o.rva) {
        if (!g_cfg.expectTimestamp || g_cfg.expectTimestamp != g_mod.view.timeDateStamp) {
            QS_ERR("apply: object %s: Rva= ignored, [General] ExpectTimestamp 0x%08X does not match this exe 0x%08X", o.name.c_str(),
                   g_cfg.expectTimestamp, g_mod.view.timeDateStamp);
            return false;
        }
        rt.anchorRva = o.rva;
    }
    if (rt.anchorRva != UINT64_MAX)
        QS_INFO("apply: object %s: anchor %s", o.name.c_str(), g_mod.view.describe(rt.anchorRva).c_str());
    return rt.anchorRva != UINT64_MAX;
}

// Filters candidates (re-read vtable, checks) and applies the Multiple policy.
std::vector<uintptr_t> pick(size_t oi, const std::vector<uintptr_t>& cands, const char* how) {
    const ObjectSpec& o = g_cfg.objects[oi];
    const ObjectRt& rt = g_obj[oi];
    std::vector<uintptr_t> ok;
    for (uintptr_t c : cands) {
        if (std::find(ok.begin(), ok.end(), c) != ok.end()) continue;
        uintptr_t vt = 0;
        if (rt.classVtX && (!probe::mem::readT(c, vt) || vt != (rt.classVtX ^ kVtXor))) continue;
        if (!passesChecks(o, c)) continue;
        ok.push_back(c);
    }
    std::sort(ok.begin(), ok.end());
    if (ok.size() > 1) {
        if (o.multiple == apply::MultiplePolicy::Refuse) {
            if (!g_obj[oi].loggedMissing) {
                QS_ERR("apply: object %s: %zu candidates (%s) pass the checks and Multiple=refuse: add Check= or set Multiple=first|all",
                       o.name.c_str(), ok.size(), how);
                for (size_t i = 0; i < ok.size() && i < 8; ++i) QS_INFO("apply:     candidate 0x%llX", (unsigned long long)ok[i]);
            }
            return {};
        }
        if (o.multiple == apply::MultiplePolicy::First) ok.resize(1);
        else if (ok.size() > o.maxInstances) ok.resize(o.maxInstances);
    }
    return ok;
}

bool installCapture(size_t oi) {
    const ObjectSpec& o = g_cfg.objects[oi];
    ObjectRt& rt = g_obj[oi];
    if (!g_cfg.allowHooks) {
        if (!rt.hookTried) QS_ERR("apply: object %s: Instance=capture needs [General] AllowHooks=1", o.name.c_str());
        rt.hookTried = true;
        return false;
    }
    if (!rt.sink) rt.sink = std::make_unique<probe::CaptureSink>();
    for (auto& v : rt.sink->objects) v = 0;
    rt.sink->maxObjects = LONG(std::min<uint32_t>(o.maxInstances * 4, probe::CaptureSink::kMaxObjects));
    rt.sink->useRdx = o.captureRdx;
    rt.hook = std::make_unique<probe::CaptureHook>();
    std::string why;
    bool ok = false;
    if (o.hook == apply::HookKind::Vtable) {
        ok = rt.hook->installVtable(g_mod, rt.classVtX ^ kVtXor, o.slot, rt.sink.get(), &why);
    } else {
        uintptr_t target = 0;
        if (o.slot >= 0 && rt.classVtX) probe::mem::readT((rt.classVtX ^ kVtXor) + uintptr_t(o.slot) * 8, target);
        else if (resolveAnchor(oi)) target = g_mod.base + uintptr_t(rt.anchorRva);
        probe::TargetCheck c = probe::checkHookTarget(g_mod, target);
        if (c == probe::TargetCheck::Ok || c == probe::TargetCheck::OkJmpStub) ok = rt.hook->installFunction(g_mod, target, rt.sink.get(), &why);
        else why = probe::toString(c);
    }
    rt.hookTried = true;
    QS_INFO("apply: object %s: capture hook %s %s", o.name.c_str(), ok ? "installed" : "FAILED:", why.c_str());
    if (!ok) rt.hook.reset();
    return ok;
}

void removeCapture(size_t oi, const char* why) {
    ObjectRt& rt = g_obj[oi];
    if (rt.hook && rt.hook->active()) {
        rt.hook->remove();
        QS_INFO("apply: object %s: capture hook removed (%s)", g_cfg.objects[oi].name.c_str(), why);
    }
    rt.hook.reset();  // the stub itself is never freed (a game thread may still be inside it)
}

// One discovery attempt; binds the object in the controller when found.
void resolveObject(size_t oi) {
    const ObjectSpec& o = g_cfg.objects[oi];
    ObjectRt& rt = g_obj[oi];
    ++rt.attempts;
    if (!resolveClass(oi)) return;
    std::vector<uintptr_t> found;
    const char* how = "";
    switch (o.instance) {
        case InstanceKind::Scan: {
            how = "heap scan";
            uint64_t t0 = nowMs();
            std::vector<uintptr_t> hits = heapScan(rt.classVtX, size_t(o.maxInstances) * 8 + 8, o.scanMaxMB);
            found = pick(oi, hits, how);
            QS_DBG("apply: object %s: heap scan %zu hit(s), %zu valid, %llu ms", o.name.c_str(), hits.size(), found.size(),
                   (unsigned long long)(nowMs() - t0));
            break;
        }
        case InstanceKind::Global: {
            how = "global";
            if (!resolveAnchor(oi)) return;
            uintptr_t a = g_mod.base + uintptr_t(rt.anchorRva);
            for (int64_t off : o.chain) {
                uintptr_t next = 0;
                if (!probe::mem::readT(a + uintptr_t(off), next) || !next) return;  // not valid (yet)
                a = next;
            }
            found = pick(oi, {a}, how);
            break;
        }
        case InstanceKind::Capture: {
            how = "capture";
            if (!rt.hook && !installCapture(oi)) return;
            std::vector<uintptr_t> cands;
            for (int i = 0; i < probe::CaptureSink::kMaxObjects; ++i) {
                uintptr_t c = uintptr_t(rt.sink->objects[i]);
                if (!c) break;
                cands.push_back(c);
            }
            found = pick(oi, cands, how);
            if (!found.empty() && !o.keepHook) removeCapture(oi, "object found");
            break;
        }
    }
    if (found.empty()) {
        if (!rt.loggedMissing && rt.attempts >= 3) {
            QS_INFO("apply: object %s not found yet (%s), retrying every %u ms", o.name.c_str(), how, g_cfg.resolveRetryMs);
            rt.loggedMissing = true;
        }
        return;
    }
    // vtable guard: the class vtable, or the vtable an untyped global object has now (if it is one)
    uintptr_t guard = 0;
    if (o.checkVtable) {
        if (rt.classVtX) {
            guard = rt.classVtX ^ kVtXor;
        } else {
            uintptr_t vt = 0;
            uint64_t vrva = probe::mem::readT(found[0], vt) ? g_mod.view.toRva(vt) : UINT64_MAX;
            if (vrva != UINT64_MAX && g_rtti.built() && g_rtti.colForVtable(uint32_t(vrva))) guard = vt;
        }
    }
    std::string cls = classOf(found[0]);
    QS_INFO("apply: object %s found (%s): %zu instance(s), first 0x%llX%s%s", o.name.c_str(), how, found.size(),
            (unsigned long long)found[0], cls.empty() ? "" : " class ", cls.c_str());
    rt.loggedMissing = false;
    std::vector<uint64_t> addrs(found.begin(), found.end());
    std::lock_guard<std::mutex> lk(g_mu);
    g_applier.bindObject(int(oi), addrs, uint64_t(guard));
}

void setPhase(const char* p) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_status.phase = p;
}

DWORD WINAPI worker(LPVOID) {
    setPhase("waiting start delay");
    if (WaitForSingleObject(g_stop, g_cfg.startDelayMs) == WAIT_OBJECT_0) {
        SetEvent(g_done);
        return 0;
    }
    std::string err;
    if (!probe::describeModule(nullptr, g_mod, &err)) {
        QS_ERR("apply: cannot describe the game module (%s): applier stopped", err.c_str());
        setPhase("error: game module");
        SetEvent(g_done);
        return 0;
    }
    if (!g_cfg.objects.empty()) {
        uint64_t t0 = nowMs();
        g_rtti.build(g_mod.view);
        QS_INFO("apply: RTTI index %zu classes in %llu ms", g_rtti.classCount(), (unsigned long long)(nowMs() - t0));
    }
    apply::GuardedMemory mem(g_mod);
    setPhase("running");
    uint64_t prev = nowMs();
    while (WaitForSingleObject(g_stop, g_cfg.intervalMs) != WAIT_OBJECT_0) {
        const uint64_t now = nowMs();
        // discovery for objects that some active target needs
        for (size_t oi = 0; oi < g_obj.size(); ++oi) {
            bool need, bound;
            {
                std::lock_guard<std::mutex> lk(g_mu);
                need = g_applier.objectNeeded(int(oi));
                bound = g_applier.objectBound(int(oi));
            }
            if (!need || bound || now < g_obj[oi].nextResolve) continue;
            resolveObject(oi);
            // a capture probe answers as soon as the game calls the slot: poll it at every tick
            g_obj[oi].nextResolve = nowMs() + (g_obj[oi].hook ? g_cfg.intervalMs : g_cfg.resolveRetryMs);
        }
        season::SeasonState s;
        season::SeasonConfig sc;
        {
            std::lock_guard<std::mutex> lk(shared().mutex);
            s = shared().season;
            sc = shared().config;
        }
        double dt = double(now - prev) / 1000.0;
        prev = now;
        if (dt > 1.0) dt = 1.0;  // a hitch must not turn into a big jump of rate limited values
        bool clockKnown;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            std::vector<int> lost;
            try {
                lost = g_applier.tick(s, sc, dt, mem);
            } catch (...) {
                QS_ERR("apply: unexpected exception in the apply tick: applier stopped");
                g_status.phase = "error: exception";
                break;
            }
            for (int oi : lost) g_obj[size_t(oi)].nextResolve = now + 250;  // rediscover soon (e.g. after a load)
            g_applier.status(g_status);
            g_status.running = true;
            clockKnown = g_status.clockKnown;
            flushLog();
        }
        if (shared().gameClockHooked.load() != clockKnown) {
            shared().gameClockHooked = clockKnown;
            QS_INFO("apply: game clock %s", clockKnown ? "found: the season calendar can follow it" : "lost: playtime fallback");
        }
    }
    {
        std::lock_guard<std::mutex> lk(g_mu);
        int n = g_applier.restoreAll(mem);
        QS_INFO("apply: stop, %d original value(s) restored", n);
        g_applier.status(g_status);
        flushLog();
        g_status.running = false;
        g_status.phase = "stopped";
    }
    for (size_t oi = 0; oi < g_obj.size(); ++oi) removeCapture(oi, "stop");
    shared().gameClockHooked = false;
    SetEvent(g_done);
    return 0;
}

}  // namespace
}  // namespace applyimpl

bool applyStartWith(const std::wstring& iniPath) {
    using namespace applyimpl;
    if (InterlockedCompareExchange(&g_running, 1, 0) != 0) return false;
    std::vector<uint8_t> raw;
    if (!readWholeFile(iniPath, raw)) {
        QS_INFO("apply: %s not found, season applier off", narrow(iniPath).c_str());
        g_running = 0;
        return false;
    }
    std::vector<std::string> errors;
    apply::TargetsConfig cfg;
    apply::parseTargetsText(std::string(raw.begin(), raw.end()), cfg, errors);
    for (const std::string& e : errors) QS_ERR("apply: targets.ini: %s", e.c_str());
    size_t enabled = 0;
    for (const apply::TargetSpec& t : cfg.targets) enabled += t.enabled;
    if (!cfg.enabled || !enabled) {
        QS_INFO("apply: %s (%zu target(s) enabled): season applier off", cfg.enabled ? "no target enabled" : "disabled in targets.ini",
                enabled);
        std::lock_guard<std::mutex> lk(g_mu);
        g_status.phase = "disabled";
        g_started = true;
        g_running = 0;
        return false;
    }
    g_cfg = cfg;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_applier.configure(g_cfg);
        g_status = apply::ApplyStatus();
        g_applier.status(g_status);
        g_status.phase = "starting";
    }
    g_obj.clear();
    g_obj.resize(g_cfg.objects.size());
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_thread = CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
    if (!g_thread) {
        QS_ERR("apply: CreateThread failed");
        g_running = 0;
        return false;
    }
    g_started = true;
    QS_INFO("apply: started, %zu object(s), %zu of %zu target(s) enabled, interval %u ms, start delay %u ms, hooks %s",
            g_cfg.objects.size(), enabled, g_cfg.targets.size(), g_cfg.intervalMs, g_cfg.startDelayMs,
            g_cfg.allowHooks ? "allowed" : "off");
    return true;
}

bool applyStart() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&applyStart), &self);
    return applyStartWith(joinPath(joinPath(dirName(modulePath(self)), L"QuattroStagioni"), L"targets.ini"));
}

void applyStop() {
    using namespace applyimpl;
    if (!g_running) return;
    SetEvent(g_stop);
    bool exited = WaitForSingleObject(g_done, 3000) == WAIT_OBJECT_0;
    if (!exited) {
        // worker stuck (should not happen): do not touch its state, just log
        QS_ERR("apply: worker did not answer in 3 s; originals may not be restored");
        return;
    }
    WaitForSingleObject(g_thread, 500);
    CloseHandle(g_thread);
    CloseHandle(g_stop);
    CloseHandle(g_done);
    g_thread = g_stop = g_done = nullptr;
    probe::GuardedMinHook::releaseIfOwned();
    g_running = 0;
}

bool applyReadGameClock(double& day, double& hours) {
    using namespace applyimpl;
    if (!g_running) return false;
    std::lock_guard<std::mutex> lk(g_mu);
    return g_applier.clock().take(day, hours);
}

bool applyGameHourOfDay(float& hour) {
    using namespace applyimpl;
    if (!g_running) return false;
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_applier.clock().known()) return false;
    hour = float(g_applier.clock().hourOfDay());
    return true;
}

bool applyGetStatus(apply::ApplyStatus& out) {
    using namespace applyimpl;
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_started) return false;
    out = g_status;
    return true;
}

}  // namespace qs
