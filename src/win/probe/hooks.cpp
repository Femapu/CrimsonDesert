// QuattroStagioni - guarded hook helpers for the runtime probes.
#include "hooks.h"

#include <MinHook.h>

#include <cstring>

#include "../../core/probe/capture_stub.h"
#include "memory.h"

namespace qs::probe {
namespace {

volatile LONG g_mhOwned = 0;    // we called MH_Initialize successfully
volatile LONG g_mhHooks = 0;    // probe hooks currently created through MinHook

void setWhy(std::string* why, const std::string& s) {
    if (why) *why = s;
}

// Called from the capture stub on a game thread: record and return, nothing else.
void captureCallback(void* ctx, void* rcx, void* ret, void* rdx) {
    CaptureSink* s = static_cast<CaptureSink*>(ctx);
    InterlockedIncrement64(&s->calls);
    LONG64 obj = LONG64(reinterpret_cast<uintptr_t>(s->useRdx ? rdx : rcx));
    if (obj) {
        for (LONG i = 0; i < s->maxObjects && i < CaptureSink::kMaxObjects; ++i) {
            LONG64 cur = s->objects[i];
            if (cur == obj) break;
            if (cur == 0) {
                LONG64 prev = InterlockedCompareExchange64(&s->objects[i], obj, 0);
                if (prev == 0 || prev == obj) break;
            }
        }
    }
    LONG64 r = LONG64(reinterpret_cast<uintptr_t>(ret));
    for (int i = 0; i < CaptureSink::kMaxCallers; ++i) {
        LONG64 cur = s->callers[i];
        if (cur == r) break;
        if (cur == 0) {
            LONG64 prev = InterlockedCompareExchange64(&s->callers[i], r, 0);
            if (prev == 0 || prev == r) break;
        }
    }
}

}  // namespace

const char* toString(TargetCheck c) {
    switch (c) {
        case TargetCheck::Ok: return "ok (code section)";
        case TargetCheck::OkJmpStub: return "ok (jmp stub in code section into the obfuscated section)";
        case TargetCheck::Outside: return "REFUSED: outside the main module";
        case TargetCheck::Obfuscated: return "REFUSED: inside an obfuscated (protector) section";
        case TargetCheck::NotCode: return "REFUSED: not in an executable section";
        case TargetCheck::Unreadable: return "REFUSED: unreadable";
    }
    return "?";
}

TargetCheck checkHookTarget(const ModuleInfo& m, uintptr_t target, uint64_t* stubDest) {
    uint64_t rva = m.view.toRva(target);
    if (rva == UINT64_MAX) return TargetCheck::Outside;
    const Section* s = m.view.sectionOf(rva);
    if (!s) return TargetCheck::Outside;
    if (s->kind == SectionKind::Obfuscated) return TargetCheck::Obfuscated;
    if (s->kind != SectionKind::Code) return TargetCheck::NotCode;
    if (!m.view.readable(rva, 16)) return TargetCheck::Unreadable;
    uint64_t dest = 0;
    if (decodeJump(m.view, rva, dest) && dest != UINT64_MAX && m.view.kindAt(dest) == SectionKind::Obfuscated) {
        if (stubDest) *stubDest = dest;
        // only the plain 5-byte E9 form is relocated safely; FF 25 reads a pointer we do not own
        return m.view.bytes[rva] == 0xE9 ? TargetCheck::OkJmpStub : TargetCheck::Obfuscated;
    }
    return TargetCheck::Ok;
}

// ---------------------------------------------------------------------------------------------
bool VtableSlotHook::install(const ModuleInfo& m, uintptr_t vtable, int slot, void* replacement, std::string* why) {
    if (slot_) return setWhy(why, "already installed"), false;
    if (slot < 0 || slot > 4096) return setWhy(why, "bad slot index"), false;
    uintptr_t addr = vtable + uintptr_t(slot) * 8;
    uint64_t rva = m.view.toRva(addr);
    const Section* s = rva == UINT64_MAX ? nullptr : m.view.sectionOf(rva);
    if (!s || s->executable() || !s->contains(rva, 8))
        return setWhy(why, "vtable slot is not in a data section of the main module"), false;
    void* cur = nullptr;
    if (!mem::readT(addr, cur)) return setWhy(why, "vtable slot unreadable"), false;
    uint64_t curRva = m.view.toRva(reinterpret_cast<uintptr_t>(cur));
    const Section* cs = curRva == UINT64_MAX ? nullptr : m.view.sectionOf(curRva);
    if (!cs || !cs->executable()) return setWhy(why, "slot does not point to code (wrong slot index?)"), false;
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(addr), 8, PAGE_READWRITE, &old))
        return setWhy(why, "VirtualProtect failed"), false;
    original_ = cur;
    replacement_ = replacement;
    InterlockedExchangePointer(reinterpret_cast<void* volatile*>(addr), replacement);
    DWORD tmp;
    VirtualProtect(reinterpret_cast<void*>(addr), 8, old, &tmp);
    slot_ = reinterpret_cast<void**>(addr);
    return true;
}

bool VtableSlotHook::restore(std::string* why) {
    if (!slot_) return true;
    void* cur = nullptr;
    bool ok = false;
    if (mem::readT(reinterpret_cast<uintptr_t>(slot_), cur) && cur == replacement_) {
        DWORD old = 0, tmp;
        if (VirtualProtect(slot_, 8, PAGE_READWRITE, &old)) {
            InterlockedExchangePointer(reinterpret_cast<void* volatile*>(slot_), original_);
            VirtualProtect(slot_, 8, old, &tmp);
            ok = true;
        } else {
            setWhy(why, "VirtualProtect failed on restore");
        }
    } else {
        setWhy(why, "slot changed by someone else: left as is");
    }
    slot_ = nullptr;
    return ok;
}

// ---------------------------------------------------------------------------------------------
bool GuardedMinHook::ensureInitialized() {
    MH_STATUS st = MH_Initialize();
    if (st == MH_OK) {
        InterlockedExchange(&g_mhOwned, 1);
        return true;
    }
    return st == MH_ERROR_ALREADY_INITIALIZED;
}

void GuardedMinHook::releaseIfOwned() {
    if (g_mhHooks == 0 && InterlockedExchange(&g_mhOwned, 0) == 1) MH_Uninitialize();
}

bool GuardedMinHook::install(const ModuleInfo& m, uintptr_t target, void* detour, void** original, std::string* why) {
    if (target_) return setWhy(why, "already installed"), false;
    TargetCheck c = checkHookTarget(m, target);
    if (c != TargetCheck::Ok && c != TargetCheck::OkJmpStub) return setWhy(why, toString(c)), false;
    if (!ensureInitialized()) return setWhy(why, "MH_Initialize failed"), false;
    void* tgt = reinterpret_cast<void*>(target);
    MH_STATUS st = MH_CreateHook(tgt, detour, original);
    if (st != MH_OK) return setWhy(why, std::string("MH_CreateHook: ") + MH_StatusToString(st)), false;
    InterlockedIncrement(&g_mhHooks);
    target_ = target;
    return true;
}

bool GuardedMinHook::remove() {
    if (!target_) return true;
    void* tgt = reinterpret_cast<void*>(target_);
    MH_DisableHook(tgt);
    MH_RemoveHook(tgt);
    InterlockedDecrement(&g_mhHooks);
    target_ = 0;
    return true;
}

// ---------------------------------------------------------------------------------------------
// Two pages: code (made executable here) and the original-function pointer (stays read-write).
uint8_t* CaptureHook::makeStub(CaptureSink* sink, uint64_t original, size_t* slotOffset) {
    CaptureStub st = buildCaptureStub(reinterpret_cast<uint64_t>(sink), reinterpret_cast<uint64_t>(&captureCallback), original, 4096);
    uint8_t* mem = static_cast<uint8_t*>(VirtualAlloc(nullptr, 8192, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!mem) return nullptr;
    std::memcpy(mem, st.code.data(), st.code.size());
    std::memcpy(mem + st.originalSlotOffset, &original, 8);
    *slotOffset = st.originalSlotOffset;
    DWORD old;
    if (!VirtualProtect(mem, 4096, PAGE_EXECUTE_READ, &old)) {
        VirtualFree(mem, 0, MEM_RELEASE);
        return nullptr;
    }
    FlushInstructionCache(GetCurrentProcess(), mem, 4096);
    return mem;
}

bool CaptureHook::installVtable(const ModuleInfo& m, uintptr_t vtable, int slot, CaptureSink* sink, std::string* why) {
    void* cur = nullptr;
    if (!mem::readT(vtable + uintptr_t(slot) * 8, cur)) return setWhy(why, "vtable slot unreadable"), false;
    size_t off = 0;
    uint8_t* stub = makeStub(sink, reinterpret_cast<uint64_t>(cur), &off);
    if (!stub) return setWhy(why, "stub allocation failed"), false;
    return vt_.install(m, vtable, slot, stub, why);
}

bool CaptureHook::installFunction(const ModuleInfo& m, uintptr_t target, CaptureSink* sink, std::string* why) {
    size_t off = 0;
    uint8_t* stub = makeStub(sink, 0, &off);
    if (!stub) return setWhy(why, "stub allocation failed"), false;
    void* tramp = nullptr;
    if (!mh_.install(m, target, stub, &tramp, why)) return false;
    InterlockedExchangePointer(reinterpret_cast<void* volatile*>(stub + off), tramp);  // data page, before enabling
    MH_STATUS st = MH_EnableHook(reinterpret_cast<void*>(target));
    if (st != MH_OK) {
        mh_.remove();
        return setWhy(why, std::string("MH_EnableHook: ") + MH_StatusToString(st)), false;
    }
    return true;
}

void CaptureHook::remove() {
    vt_.restore(nullptr);
    mh_.remove();
}

}  // namespace qs::probe
