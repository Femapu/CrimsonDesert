// QuattroStagioni - guarded hook helpers for the runtime probes.
#include "hooks.h"

#include <MinHook.h>

#include <cstring>

#include "../../core/probe/capture_stub.h"
#include "../log.h"
#include "memory.h"

namespace qs::probe {
namespace {

volatile LONG g_mhOwned = 0;    // we called MH_Initialize successfully
volatile LONG g_mhHooks = 0;    // probe hooks currently created through MinHook

// Address space kept free for MinHook trampoline blocks (see reserveTrampolineSpace).
constexpr int kMaxReserved = 16;
constexpr uintptr_t kTrampolineReach = 0x40000000;  // MinHook buffer.c MAX_MEMORY_RANGE (+-1 GB)
void* volatile g_reserved[kMaxReserved] = {};
volatile LONG g_reservedLeft = 0;

uintptr_t allocGranularity() {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwAllocationGranularity ? si.dwAllocationGranularity : 0x10000;
}

// Reserves free granularity-aligned regions walking from `from` towards `limit` (down when
// down = true), as MinHook's own FindPrev/NextFreeRegion would. Returns the count added.
int reserveWalk(uintptr_t from, uintptr_t limit, bool down, int want, int& slot, uintptr_t g) {
    int got = 0;
    uintptr_t a = (from / g) * g;
    a = down ? a - g : a + g;
    while (got < want && slot < kMaxReserved && (down ? a >= limit : a <= limit)) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof(mbi)) == 0) break;
        uintptr_t next;
        if (mbi.State == MEM_FREE) {
            if (void* r = VirtualAlloc(reinterpret_cast<void*>(a), g, MEM_RESERVE, PAGE_NOACCESS)) {
                g_reserved[slot++] = r;
                ++got;
            }
            next = down ? a - g : a + g;
        } else if (down) {
            uintptr_t ab = reinterpret_cast<uintptr_t>(mbi.AllocationBase);
            if (ab < g) break;
            next = (ab / g) * g - g;
        } else {
            next = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
            next = (next + g - 1) / g * g;
        }
        if (down ? next >= a : next <= a) break;  // no progress (wrap-around)
        a = next;
    }
    return got;
}

// Gives back one reserved region that MinHook can reach from `target`, so its next search finds
// free space there. False when none is left.
bool releaseReservedNear(uintptr_t target) {
    const uintptr_t g = allocGranularity();
    for (int i = 0; i < kMaxReserved; ++i) {
        uintptr_t r = reinterpret_cast<uintptr_t>(g_reserved[i]);
        if (!r || r + kTrampolineReach < target + g || r + g > target + kTrampolineReach - g) continue;
        if (InterlockedCompareExchangePointer(&g_reserved[i], nullptr, reinterpret_cast<void*>(r)) !=
            reinterpret_cast<void*>(r))
            continue;  // taken by another thread
        VirtualFree(reinterpret_cast<void*>(r), 0, MEM_RELEASE);
        LONG left = InterlockedDecrement(&g_reservedLeft);
        QS_INFO("hooks: no free memory near %p for a MinHook trampoline: reserved region %p released for it (%ld left)",
                reinterpret_cast<void*>(target), reinterpret_cast<void*>(r), long(left));
        return true;
    }
    return false;
}

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

int GuardedMinHook::reserveTrampolineSpace(int count, uintptr_t* lo, uintptr_t* hi) {
    if (count > kMaxReserved) count = kMaxReserved;
    int slot = 0;
    while (slot < kMaxReserved && g_reserved[slot]) ++slot;  // a second call only adds
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(GetModuleHandleW(nullptr));
    if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const uint8_t*>(dos) + dos->e_lfanew);
    const uintptr_t base = reinterpret_cast<uintptr_t>(dos);
    const uintptr_t end = base + nt->OptionalHeader.SizeOfImage;
    const uintptr_t g = allocGranularity();
    if (end - base > kTrampolineReach / 2) return 0;  // no region would reach the whole image
    // Bounds that keep a region (and MinHook's 4 KB block in it) reachable from every image byte.
    const uintptr_t minAddr = end > kTrampolineReach ? end - kTrampolineReach + g : g;
    const uintptr_t maxAddr = base + kTrampolineReach - 2 * g;
    int got = reserveWalk(base, minAddr, true, (count + 1) / 2, slot, g);  // below the image first
    got += reserveWalk(end - 1, maxAddr, false, count - got, slot, g);
    if (got < count) got += reserveWalk(base, minAddr, true, count - got, slot, g);  // top up below
    InterlockedExchangeAdd(&g_reservedLeft, got);
    uintptr_t l = UINTPTR_MAX, h = 0;
    for (int i = 0; i < kMaxReserved; ++i) {
        uintptr_t r = reinterpret_cast<uintptr_t>(g_reserved[i]);
        if (!r) continue;
        if (r < l) l = r;
        if (r > h) h = r;
    }
    if (lo) *lo = got ? l : 0;
    if (hi) *hi = got ? h : 0;
    return got;
}

bool GuardedMinHook::install(const ModuleInfo& m, uintptr_t target, void* detour, void** original, std::string* why) {
    if (target_) return setWhy(why, "already installed"), false;
    TargetCheck c = checkHookTarget(m, target);
    if (c != TargetCheck::Ok && c != TargetCheck::OkJmpStub) return setWhy(why, toString(c)), false;
    if (!ensureInitialized()) return setWhy(why, "MH_Initialize failed"), false;
    void* tgt = reinterpret_cast<void*>(target);
    MH_STATUS st = MH_CreateHook(tgt, detour, original);
    // No room within MinHook's reach: hand it a region reserved at startup and retry.
    while (st == MH_ERROR_MEMORY_ALLOC && releaseReservedNear(target)) st = MH_CreateHook(tgt, detour, original);
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
