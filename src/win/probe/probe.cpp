// QuattroStagioni - runtime probe framework (see probe.h for the integration contract).
//
// Everything runs on one background thread; game threads only ever enter the capture stub (when a
// capture probe is installed), which records pointers lock-free. All reads of game memory go through
// mem::read (VirtualQuery + ReadProcessMemory), so a wrong offset in probes.ini logs "unreadable"
// instead of crashing the game.
#include "probe.h"

#include <windows.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "../../core/ini.h"
#include "../../core/probe/diff.h"
#include "../../core/probe/probe_config.h"
#include "../../core/probe/rtti.h"
#include "../log.h"
#include "../winutil.h"
#include "hooks.h"
#include "memory.h"
#include "module.h"
#include "probe_log.h"

namespace qs {
namespace probe {
namespace {

// Vtable addresses are kept XOR-ed in our own memory so the heap scan never finds our bookkeeping.
constexpr uintptr_t kVtXor = 0x5A5A5A5A5A5A5A5Aull;

struct Instance {
    uintptr_t addr = 0;
    uintptr_t vtableX = 0;  // expected vtable ^ kVtXor (0 = unchecked)
    bool dead = false;
    bool haveDump = false;
    std::vector<uint8_t> dump;
    bool haveWatch = false;
    std::vector<std::vector<uint8_t>> watchVals;
    FieldStats stats;
    uint64_t unreadable = 0;
};

struct Runtime {
    ProbeSpec spec;
    bool ready = false;     // anchor resolved
    bool finished = false;  // duration over or fatal error
    uint64_t anchorRva = UINT64_MAX;
    bool hasClass = false;
    RttiClass cls;
    uintptr_t vtableX = 0;
    CaptureSink sink;
    CaptureHook hook;
    std::vector<Instance> inst;
    uint64_t nextTick = 0, endTick = 0, nextScan = 0;
    LONG64 lastCalls = 0;
    int callersLogged = 0;
};

ModuleInfo g_mod;
RttiIndex g_rtti;
ProbeConfig g_cfg;
std::vector<std::unique_ptr<Runtime>> g_rt;
HANDLE g_thread = nullptr, g_stop = nullptr, g_done = nullptr;
SRWLOCK g_hookLock = SRWLOCK_INIT;
volatile LONG g_running = 0;

uint64_t nowMs() { return GetTickCount64(); }

std::string describeVa(uintptr_t va) {
    uint64_t rva = g_mod.view.toRva(va);
    char b[48];
    if (rva == UINT64_MAX) {
        std::snprintf(b, sizeof(b), "0x%llX (not in exe)", (unsigned long long)va);
        return b;
    }
    return "rva " + g_mod.view.describe(rva);
}

// Class name of the object at `obj` through its vtable pointer, empty if unknown.
std::string classOfObject(uintptr_t obj, uintptr_t* vtOut = nullptr) {
    uintptr_t vt = 0;
    if (!mem::readT(obj, vt)) return "";
    if (vtOut) *vtOut = vt;
    uint64_t rva = g_mod.view.toRva(vt);
    if (rva == UINT64_MAX || !g_rtti.built()) return "";
    const ColInfo* c = g_rtti.colForVtable(uint32_t(rva));
    return c ? demangleClassName(g_rtti.typeName(c->tdRva)) + (c->offset ? " (secondary vtable)" : "") : "";
}

void logSections() {
    const ImageView& v = g_mod.view;
    plog::line("module %s base 0x%llX size 0x%llX entry 0x%X timestamp 0x%08X (%u)", narrow(g_mod.path).c_str(),
               (unsigned long long)g_mod.base, (unsigned long long)v.size, v.entryRva, v.timeDateStamp, v.timeDateStamp);
    for (const Section& s : v.sections)
        plog::line("  section %-8s rva 0x%08X vsize 0x%08X raw 0x%08X flags 0x%08X %c%c%c -> %-10s%s", s.name.c_str(), s.rva,
                   s.vsize, s.rawSize, s.characteristics, (s.characteristics & kScnMemRead) ? 'R' : '-',
                   s.writable() ? 'W' : '-', s.executable() ? 'X' : '-', toString(s.kind),
                   s.kind == SectionKind::Obfuscated ? " (never read/hooked)" : (s.accessible ? "" : " (not readable)"));
    const Section* c = v.primaryCode();
    if (c) plog::line("  game code section: %s at 0x%X (scan + hook target)", c->name.c_str(), c->rva);
    else plog::line("  WARNING: no primary code section identified");
}

void logVtable(uint32_t vtRva, uint32_t maxSlots) {
    uint32_t n = g_rtti.vtableSlots(vtRva, 4096);
    plog::line("    vtable rva 0x%X: %u slots%s", vtRva, n, n > maxSlots ? " (truncated, raise MaxSlots)" : "");
    for (uint32_t i = 0; i < n && i < maxSlots; ++i) {
        uint64_t q = 0;
        g_mod.view.read(uint64_t(vtRva) + 8ull * i, q);
        uint64_t r = g_mod.view.toRva(q), dest = 0;
        std::string extra;
        if (decodeJump(g_mod.view, r, dest) && dest != UINT64_MAX)
            extra = " -> jmp " + g_mod.view.describe(dest) + (g_mod.view.kindAt(dest) == SectionKind::Obfuscated ? " [DENUVO STUB: hook the slot, never the body]" : "");
        plog::line("      [%3u] +0x%03X %s%s", i, i * 8, g_mod.view.describe(r).c_str(), extra.c_str());
    }
}

bool resolveClass(Runtime& rt) {
    const ProbeSpec& p = rt.spec;
    if (!g_rtti.find(p.className, rt.cls)) {
        plog::line("probe %s: class %s (%s) NOT FOUND in RTTI", p.name.c_str(), p.className.c_str(), mangleClassName(p.className).c_str());
        return false;
    }
    rt.hasClass = true;
    plog::line("probe %s: class %s  TypeDescriptor rva 0x%X, %zu COL(s)", p.name.c_str(), rt.cls.pretty.c_str(), rt.cls.tdRva,
               rt.cls.cols.size());
    for (const ColInfo& c : rt.cls.cols)
        plog::line("    COL rva 0x%X offset 0x%X vtable rva 0x%X (refs %u)", c.rva, c.offset, c.vtableRva, c.vtableRefs);
    uint32_t vt = rt.cls.primaryVtable();
    if (!vt) {
        plog::line("    no vtable references the primary COL");
        return false;
    }
    rt.vtableX = (g_mod.base + vt) ^ kVtXor;
    if (p.logHierarchy) {
        std::vector<BaseClass> b = g_rtti.baseClasses(rt.cls.cols[0]);
        for (size_t i = 0; i < b.size(); ++i)
            plog::line("    %s %s (mdisp 0x%X, contains %u)", i == 0 ? "class" : " base", demangleClassName(b[i].mangled).c_str(),
                       b[i].mdisp, b[i].containedBases);
    }
    if (p.logVtable) logVtable(vt, p.maxSlots);
    return true;
}

bool resolveSignature(Runtime& rt) {
    const ProbeSpec& p = rt.spec;
    std::vector<uint64_t> hits = scanModule(g_mod, p.signature, p.sigInData, 64);
    plog::line("probe %s: signature %s -> %zu match(es)%s", p.name.c_str(), formatPattern(p.signature).c_str(), hits.size(),
               hits.size() >= 64 ? "+" : "");
    if (hits.empty()) return false;
    std::vector<uint64_t> targets;
    for (uint64_t h : hits) {
        uint64_t r = h + uint64_t(p.sigOffset);
        if (p.resolve.enabled) {
            int32_t disp;
            if (!g_mod.view.read(r + p.resolve.dispOffset, disp)) return false;
            r = ripTarget(r, p.resolve.insnLen, disp);
        }
        targets.push_back(r);
        if (targets.size() <= 4) plog::line("    match %s -> %s", g_mod.view.describe(h).c_str(), g_mod.view.describe(r).c_str());
    }
    bool same = std::all_of(targets.begin(), targets.end(), [&](uint64_t t) { return t == targets[0]; });
    if (targets.size() > 1 && !(p.allowMultiple && same)) {
        plog::line("    ambiguous (%s): probe disabled; make the pattern longer or set AllowMultiple=1",
                   same ? "all resolve to the same target" : "different targets");
        return false;
    }
    rt.anchorRva = targets[0];
    return true;
}

bool resolveAnchor(Runtime& rt) {
    const ProbeSpec& p = rt.spec;
    if (!p.listGlob.empty()) {
        std::vector<RttiClass> list = g_rtti.findGlob(p.listGlob, 2000);
        plog::line("probe %s: %zu class(es) match '%s'", p.name.c_str(), list.size(), p.listGlob.c_str());
        for (const RttiClass& c : list) {
            uint32_t vt = c.primaryVtable();
            plog::line("    %-70s vtable 0x%-8X slots %-4u cols %zu", c.pretty.c_str(), vt, vt ? g_rtti.vtableSlots(vt) : 0,
                       c.cols.size());
            if (p.logVtable && vt) logVtable(vt, p.maxSlots);
        }
        return false;  // nothing else to do
    }
    if (!p.className.empty()) return resolveClass(rt);
    if (!p.signatureText.empty()) return resolveSignature(rt);
    if (p.rva) {
        if (!g_cfg.expectTimestamp || g_cfg.expectTimestamp != g_mod.view.timeDateStamp) {
            plog::line("probe %s: Rva= ignored: [General] ExpectTimestamp (0x%08X) does not match this exe (0x%08X)", p.name.c_str(),
                       g_cfg.expectTimestamp, g_mod.view.timeDateStamp);
            return false;
        }
        rt.anchorRva = p.rva;
        plog::line("probe %s: fixed %s", p.name.c_str(), g_mod.view.describe(p.rva).c_str());
        return g_mod.view.sectionOf(p.rva) != nullptr;
    }
    return false;
}

bool installCapture(Runtime& rt) {
    const ProbeSpec& p = rt.spec;
    if (!g_cfg.allowHooks) {
        plog::line("probe %s: capture skipped: [General] AllowHooks=0 (read-only mode)", p.name.c_str());
        return false;
    }
    rt.sink.maxObjects = LONG(p.maxInstances);
    rt.sink.useRdx = p.captureRdx;
    std::string why;
    bool ok = false;
    AcquireSRWLockExclusive(&g_hookLock);
    if (p.hook == HookKind::Vtable) {
        ok = rt.hook.installVtable(g_mod, rt.vtableX ^ kVtXor, p.slot, &rt.sink, &why);
        plog::line("probe %s: vtable slot %d (+0x%X) swap %s %s", p.name.c_str(), p.slot, p.slot * 8, ok ? "installed" : "FAILED:",
                   why.c_str());
    } else {
        uintptr_t target = 0;
        if (rt.hasClass) {
            mem::readT((rt.vtableX ^ kVtXor) + uintptr_t(p.slot) * 8, target);
        } else {
            target = g_mod.base + rt.anchorRva;
        }
        uint64_t dest = 0;
        TargetCheck c = checkHookTarget(g_mod, target, &dest);
        plog::line("probe %s: minhook target %s: %s", p.name.c_str(), describeVa(target).c_str(), toString(c));
        if (c == TargetCheck::Ok || c == TargetCheck::OkJmpStub) {
            ok = rt.hook.installFunction(g_mod, target, &rt.sink, &why);
            plog::line("    detour %s %s", ok ? "installed" : "FAILED:", why.c_str());
        }
    }
    ReleaseSRWLockExclusive(&g_hookLock);
    return ok;
}

std::vector<uintptr_t> heapScan(uintptr_t vtableX, size_t maxHits, uint32_t maxMB) {
    std::vector<uintptr_t> hits;
    const size_t kChunk = 1 << 20;
    uint8_t* buf = static_cast<uint8_t*>(VirtualAlloc(nullptr, kChunk, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!buf) return hits;
    ULONG_PTR stLo = 0, stHi = 0;
    GetCurrentThreadStackLimits(&stLo, &stHi);
    uint64_t budget = uint64_t(maxMB) << 20;
    mem::forEachHeapRegion([&](uintptr_t base, size_t size) {
        if (base == reinterpret_cast<uintptr_t>(buf) || (base <= stLo && stLo < base + size)) return true;
        for (size_t off = 0; off < size && budget; off += kChunk) {
            size_t n = std::min(kChunk, size - off);
            if (n > budget) n = size_t(budget);
            budget -= n;
            if (!mem::read(base + off, buf, n)) continue;
            const uintptr_t needle = vtableX ^ kVtXor;
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

void addInstance(Runtime& rt, uintptr_t addr, uintptr_t expectVt, const char* how) {
    for (const Instance& i : rt.inst)
        if (i.addr == addr) return;
    if (rt.inst.size() >= rt.spec.maxInstances) return;
    Instance in;
    in.addr = addr;
    if (rt.spec.checkVtable && expectVt) in.vtableX = expectVt ^ kVtXor;
    if (rt.spec.stats) in.stats.reset(rt.spec.dumpSize);
    rt.inst.push_back(std::move(in));
    std::string cls = classOfObject(addr);
    plog::line("probe %s: instance #%zu at 0x%llX (%s)%s%s", rt.spec.name.c_str(), rt.inst.size() - 1, (unsigned long long)addr, how,
               cls.empty() ? "" : " class ", cls.c_str());
}

void acquireInstances(Runtime& rt, uint64_t now) {
    const ProbeSpec& p = rt.spec;
    if (p.instance == InstanceMode::Global) {
        uintptr_t a = g_mod.base + rt.anchorRva;
        for (int64_t off : p.chain) {
            uintptr_t next = 0;
            if (!mem::readT(a + uintptr_t(off), next) || !next) return;  // chain not valid (yet)
            a = next;
        }
        if (!rt.inst.empty() && rt.inst[0].addr != a) {
            plog::line("probe %s: global chain now points to 0x%llX (was 0x%llX): snapshot reset", p.name.c_str(),
                       (unsigned long long)a, (unsigned long long)rt.inst[0].addr);
            rt.inst.clear();
        }
        if (rt.inst.empty()) addInstance(rt, a, 0, p.chain.empty() ? "global" : "global chain");
    } else if (p.instance == InstanceMode::Scan) {
        if (now < rt.nextScan || rt.inst.size() >= p.maxInstances) return;
        uint64_t t0 = nowMs();
        std::vector<uintptr_t> hits = heapScan(rt.vtableX, p.maxInstances * 4, p.scanMaxMB);
        plog::line("probe %s: heap scan for vtable found %zu object(s) in %llu ms", p.name.c_str(), hits.size(),
                   (unsigned long long)(nowMs() - t0));
        for (uintptr_t h : hits) {
            uintptr_t vt = 0;  // transient copies (stack spills, freed blocks) do not survive a re-read
            if (mem::readT(h, vt) && vt == (rt.vtableX ^ kVtXor)) addInstance(rt, h, vt, "heap scan");
        }
        rt.nextScan = now + (rt.inst.empty() ? 10000 : (p.rescanSec ? uint64_t(p.rescanSec) * 1000 : UINT64_MAX / 2));
    } else if (p.instance == InstanceMode::Capture) {
        for (int i = 0; i < CaptureSink::kMaxObjects; ++i) {
            uintptr_t o = uintptr_t(rt.sink.objects[i]);
            if (!o) break;
            uintptr_t vt = 0;
            if (rt.hasClass && p.hook == HookKind::Vtable) vt = rt.vtableX ^ kVtXor;
            else mem::readT(o, vt);
            addInstance(rt, o, vt, p.captureRdx ? "captured rdx" : "captured this");
        }
        if (p.logCallers) {
            for (int i = rt.callersLogged; i < CaptureSink::kMaxCallers; ++i) {
                uintptr_t c = uintptr_t(rt.sink.callers[i]);
                if (!c) break;
                plog::line("probe %s: caller #%d return address %s", p.name.c_str(), i, describeVa(c).c_str());
                rt.callersLogged = i + 1;
            }
        }
    }
}

void tickInstance(Runtime& rt, Instance& in, size_t idx) {
    const ProbeSpec& p = rt.spec;
    const char* nm = p.name.c_str();
    if (in.vtableX) {
        uintptr_t vt = 0;
        if (!mem::readT(in.addr, vt) || vt != (in.vtableX ^ kVtXor)) {
            in.dead = true;
            plog::line("probe %s: instance #%zu 0x%llX vtable changed or unreadable: object gone, dropped", nm, idx,
                       (unsigned long long)in.addr);
            return;
        }
    }
    if (p.dumpSize) {
        std::vector<uint8_t> cur(p.dumpSize);
        if (!mem::read(in.addr + p.dumpOffset, cur.data(), cur.size())) {
            if (in.unreadable++ == 0) plog::line("probe %s: #%zu dump range unreadable", nm, idx);
        } else if (!in.haveDump) {
            plog::line("probe %s: #%zu initial dump +0x%X..+0x%X", nm, idx, p.dumpOffset, p.dumpOffset + p.dumpSize);
            for (size_t o = 0; o < cur.size() && o < 4096; o += 32)
                plog::line("    +%05zX  %s", p.dumpOffset + o, hexBytes(cur.data() + o, std::min<size_t>(32, cur.size() - o)).c_str());
            if (cur.size() > 4096) plog::line("    (initial dump truncated at 4096 bytes, changes are still tracked)");
            in.dump = cur;
            in.haveDump = true;
            if (p.stats) in.stats.add(cur.data());
        } else {
            std::vector<ChangedRange> r = alignRanges(diffBytes(in.dump.data(), cur.data(), cur.size()), cur.size());
            int lines = 0;
            for (const ChangedRange& c : r) {
                if (++lines > 24) {
                    plog::line("    ... %zu more changed ranges", r.size() - 24);
                    break;
                }
                if (c.length <= 64)
                    plog::line("probe %s: #%zu changed %s", nm, idx, describeDwords(p.dumpOffset + c.offset, cur.data() + c.offset, c.length).c_str());
                else
                    plog::line("probe %s: #%zu changed +0x%zX..+0x%zX (%zu bytes)", nm, idx, p.dumpOffset + c.offset,
                               p.dumpOffset + c.offset + c.length, c.length);
            }
            in.dump.swap(cur);
            if (p.stats) in.stats.add(in.dump.data());
        }
    }
    if (!p.watches.empty()) {
        if (in.watchVals.size() != p.watches.size()) in.watchVals.assign(p.watches.size(), std::vector<uint8_t>(8, 0));
        std::string changes;
        for (size_t i = 0; i < p.watches.size(); ++i) {
            const WatchField& w = p.watches[i];
            uint8_t v[8] = {};
            if (!mem::read(in.addr + w.offset, v, fieldSize(w.type))) continue;
            if (!in.haveWatch || fieldChanged(w.type, in.watchVals[i].data(), v, p.epsilon)) {
                char b[160];
                std::string label = w.name.empty() ? "" : w.name + "@";
                std::snprintf(b, sizeof(b), "%s%s+0x%X:%s=%s", changes.empty() ? "" : "  ", label.c_str(), w.offset, toString(w.type),
                              formatField(w.type, v).c_str());
                changes += b;
                std::memcpy(in.watchVals[i].data(), v, 8);
            }
        }
        if (!changes.empty()) plog::line("probe %s: #%zu %s%s", nm, idx, in.haveWatch ? "" : "watch start ", changes.c_str());
        in.haveWatch = true;
    }
}

void summary(Runtime& rt, const char* why) {
    const ProbeSpec& p = rt.spec;
    if (!rt.ready) return;
    LONG64 calls = rt.sink.calls;
    if (p.instance == InstanceMode::Capture)
        plog::line("summary(%s) %s: %lld calls (+%lld), %zu instance(s)", why, p.name.c_str(), (long long)calls,
                   (long long)(calls - rt.lastCalls), rt.inst.size());
    rt.lastCalls = calls;
    if (!p.stats) return;
    for (size_t i = 0; i < rt.inst.size(); ++i) {
        const FieldStats& st = rt.inst[i].stats;
        if (st.samples() < 2) continue;
        for (const auto& range : p.statsRanges) {
            std::vector<FieldStats::Candidate> c = st.floatsInRange(range.first, range.second, 1);
            std::string s;
            for (size_t k = 0; k < c.size() && k < 16; ++k) {
                char b[96];
                std::snprintf(b, sizeof(b), " +0x%zX[%g..%g n%u]", p.dumpOffset + c[k].offset, double(c[k].minF), double(c[k].maxF), c[k].changes);
                s += b;
            }
            plog::line("summary(%s) %s #%zu floats in [%g, %g] that changed (%zu samples):%s", why, p.name.c_str(), i,
                       double(range.first), double(range.second), st.samples(), s.empty() ? " none" : s.c_str());
        }
        std::vector<FieldStats::Candidate> a = st.mostActive(12, 1);
        std::string s;
        for (const auto& c : a) {
            char b[96];
            std::snprintf(b, sizeof(b), " +0x%zX[n%u u32 %u..%u]", p.dumpOffset + c.offset, c.changes, c.minU, c.maxU);
            s += b;
        }
        plog::line("summary(%s) %s #%zu most active dwords:%s", why, p.name.c_str(), i, s.empty() ? " none" : s.c_str());
    }
}

void removeHooks() {
    AcquireSRWLockExclusive(&g_hookLock);
    for (auto& rt : g_rt)
        if (rt->hook.active()) {
            rt->hook.remove();
            plog::line("probe %s: hook removed", rt->spec.name.c_str());
        }
    ReleaseSRWLockExclusive(&g_hookLock);
}

DWORD WINAPI worker(LPVOID) {
    if (WaitForSingleObject(g_stop, g_cfg.startDelayMs) == WAIT_OBJECT_0) {
        SetEvent(g_done);
        return 0;
    }
    std::string err;
    if (!describeModule(nullptr, g_mod, &err)) {
        plog::line("cannot describe the game module: %s; probes stopped", err.c_str());
        SetEvent(g_done);
        return 0;
    }
    if (g_cfg.logSections) logSections();
    bool needRtti = false;
    for (auto& rt : g_rt) needRtti = needRtti || !rt->spec.className.empty() || !rt->spec.listGlob.empty() || rt->spec.instance == InstanceMode::Capture;
    if (needRtti) {
        uint64_t t0 = nowMs();
        g_rtti.build(g_mod.view);
        plog::line("RTTI index: %zu classes, %zu COLs in %llu ms", g_rtti.classCount(), g_rtti.colCount(), (unsigned long long)(nowMs() - t0));
    }
    const uint64_t start = nowMs();
    for (auto& rt : g_rt) {
        if (WaitForSingleObject(g_stop, 0) == WAIT_OBJECT_0) break;
        rt->ready = resolveAnchor(*rt);
        if (!rt->ready || rt->spec.instance == InstanceMode::None) {
            rt->finished = true;
            continue;
        }
        if (rt->spec.instance == InstanceMode::Capture && !installCapture(*rt)) {
            rt->finished = true;
            continue;
        }
        if (rt->spec.durationSec) rt->endTick = start + uint64_t(rt->spec.durationSec) * 1000;
    }
    plog::flush();
    uint64_t nextSummary = start + uint64_t(g_cfg.summarySec ? g_cfg.summarySec : 30) * 1000;
    while (true) {
        uint64_t now = nowMs(), wake = now + 1000;
        bool any = false;
        for (auto& rtp : g_rt) {
            Runtime& rt = *rtp;
            if (rt.finished) continue;
            any = true;
            if (rt.endTick && now >= rt.endTick) {
                summary(rt, "end");
                AcquireSRWLockExclusive(&g_hookLock);
                if (rt.hook.active()) rt.hook.remove(), plog::line("probe %s: duration over, hook removed", rt.spec.name.c_str());
                ReleaseSRWLockExclusive(&g_hookLock);
                rt.finished = true;
                continue;
            }
            if (now >= rt.nextTick) {
                acquireInstances(rt, now);
                for (size_t i = 0; i < rt.inst.size(); ++i)
                    if (!rt.inst[i].dead) tickInstance(rt, rt.inst[i], i);
                rt.inst.erase(std::remove_if(rt.inst.begin(), rt.inst.end(), [](const Instance& x) { return x.dead; }), rt.inst.end());
                rt.nextTick = now + (rt.spec.intervalMs ? rt.spec.intervalMs : g_cfg.intervalMs);
            }
            wake = std::min(wake, rt.nextTick);
        }
        if (now >= nextSummary) {
            for (auto& rt : g_rt) summary(*rt, "periodic");
            plog::flush();
            nextSummary = now + uint64_t(g_cfg.summarySec ? g_cfg.summarySec : 30) * 1000;
        }
        if (!any) {
            plog::line("all probes finished");
            break;
        }
        DWORD wait = DWORD(wake > now ? std::min<uint64_t>(wake - now, 1000) : 1);
        if (WaitForSingleObject(g_stop, wait) == WAIT_OBJECT_0) break;
    }
    for (auto& rt : g_rt)
        if (!rt->finished) summary(*rt, "stop");
    removeHooks();
    plog::flush();
    SetEvent(g_done);
    return 0;
}

}  // namespace
}  // namespace probe

bool probeStartWith(const std::wstring& iniPath, const std::wstring& logPath) {
    using namespace probe;
    if (InterlockedCompareExchange(&g_running, 1, 0) != 0) return false;
    std::vector<uint8_t> raw;
    if (!readWholeFile(iniPath, raw)) {
        QS_INFO("probes: %s not found, probes off", narrow(iniPath).c_str());
        g_running = 0;
        return false;
    }
    std::vector<std::string> errors;
    parseProbeConfigText(std::string(raw.begin(), raw.end()), g_cfg, errors);
    if (!g_cfg.enabled) {
        QS_INFO("probes: disabled in %s", narrow(iniPath).c_str());
        g_running = 0;
        return false;
    }
    plog::open(logPath, g_cfg.maxLogMB);
    plog::line("QuattroStagioni probes: %zu probe(s), start delay %u ms, interval %u ms, hooks %s", g_cfg.probes.size(),
               g_cfg.startDelayMs, g_cfg.intervalMs, g_cfg.allowHooks ? "ALLOWED" : "off (read-only)");
    for (const std::string& e : errors) plog::line("config error: %s", e.c_str());
    g_rt.clear();
    for (const ProbeSpec& p : g_cfg.probes) {
        g_rt.push_back(std::make_unique<Runtime>());
        g_rt.back()->spec = p;
    }
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_thread = CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
    if (!g_thread) {
        plog::line("CreateThread failed");
        plog::close();
        g_running = 0;
        return false;
    }
    QS_INFO("probes: started (%zu probes, %zu config errors), log %s", g_cfg.probes.size(), errors.size(), narrow(logPath).c_str());
    return true;
}

bool probeStart() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&probeStart), &self);
    const std::wstring dir = joinPath(dirName(modulePath(self)), L"QuattroStagioni");
    return probeStartWith(joinPath(dir, L"probes.ini"), joinPath(dir, L"probe.log"));
}

void probeStop() {
    using namespace probe;
    if (!g_running) return;
    SetEvent(g_stop);
    bool exited = WaitForSingleObject(g_done, 3000) == WAIT_OBJECT_0;
    removeHooks();  // idempotent; also covers a worker that did not answer in time
    GuardedMinHook::releaseIfOwned();
    plog::line("probes stopped%s", exited ? "" : " (worker did not answer in 3 s)");
    plog::close();
    if (exited) {
        WaitForSingleObject(g_thread, 500);
        CloseHandle(g_thread);
        CloseHandle(g_stop);
        CloseHandle(g_done);
        g_thread = g_stop = g_done = nullptr;
        g_rt.clear();
        g_running = 0;
    }
}

}  // namespace qs
