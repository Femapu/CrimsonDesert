// QuattroStagioni - fail-safe memory access for the runtime probes.
#include "memory.h"

namespace qs::probe::mem {

bool readableProtect(DWORD p) {
    if (p & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    const DWORD base = p & 0xFF;
    return base == PAGE_READONLY || base == PAGE_READWRITE || base == PAGE_WRITECOPY || base == PAGE_EXECUTE_READ ||
           base == PAGE_EXECUTE_READWRITE || base == PAGE_EXECUTE_WRITECOPY;
}

bool isReadable(const void* p, size_t n) {
    uintptr_t a = reinterpret_cast<uintptr_t>(p), end = a + n;
    if (!a || end < a) return false;
    while (a < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(reinterpret_cast<const void*>(a), &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
        if (mbi.State != MEM_COMMIT || !readableProtect(mbi.Protect)) return false;
        uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (regionEnd <= a) return false;
        a = regionEnd;
    }
    return true;
}

bool read(uintptr_t addr, void* out, size_t n) {
    if (!n) return true;
    if (!isReadable(reinterpret_cast<const void*>(addr), n)) return false;
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(addr), out, n, &got) && got == n;
}

void forEachHeapRegion(const std::function<bool(uintptr_t, size_t)>& cb) { forEachHeapRegionFrom(0, cb); }

void forEachHeapRegionFrom(uintptr_t start, const std::function<bool(uintptr_t, size_t)>& cb) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uintptr_t a = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
    if (start > a) a = start;
    const uintptr_t maxA = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
    while (a < maxA) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(reinterpret_cast<const void*>(a), &mbi, sizeof(mbi)) != sizeof(mbi)) break;
        uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && (mbi.Protect & 0xFF) == PAGE_READWRITE &&
            !(mbi.Protect & PAGE_GUARD)) {
            if (!cb(base, mbi.RegionSize)) return;
        }
        uintptr_t next = base + mbi.RegionSize;
        if (next <= a) break;
        a = next;
    }
}

}  // namespace qs::probe::mem
