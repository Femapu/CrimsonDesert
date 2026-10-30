// QuattroStagioni - guarded game memory access for the applier (see guarded_memory.h).
#include "guarded_memory.h"

#include <windows.h>

#include <cstring>

#include "../probe/memory.h"

namespace qs::apply {

const char* toString(WriteCheck c) {
    switch (c) {
        case WriteCheck::Ok: return "ok";
        case WriteCheck::Unmapped: return "not committed";
        case WriteCheck::NotReadWrite: return "page is not PAGE_READWRITE (code, read-only data or protector section)";
        case WriteCheck::Guard: return "guard page";
        case WriteCheck::ImageNotData: return "inside the exe but not in its writable data section";
        case WriteCheck::OtherModule: return "inside another module";
        case WriteCheck::ForeignMapping: return "mapped file view";
    }
    return "?";
}

WriteCheck checkWritable(const probe::ModuleInfo& m, uintptr_t addr, size_t n) {
    uintptr_t a = addr, end = addr + n;
    if (!a || end < a) return WriteCheck::Unmapped;
    while (a < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(reinterpret_cast<const void*>(a), &mbi, sizeof(mbi)) != sizeof(mbi)) return WriteCheck::Unmapped;
        if (mbi.State != MEM_COMMIT) return WriteCheck::Unmapped;
        if (mbi.Protect & PAGE_GUARD) return WriteCheck::Guard;
        if (mbi.Type == MEM_MAPPED) return WriteCheck::ForeignMapping;
        const DWORD prot = mbi.Protect & 0xFF;
        uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (regionEnd <= a) return WriteCheck::Unmapped;
        if (mbi.Type != MEM_IMAGE && prot != PAGE_READWRITE) return WriteCheck::NotReadWrite;
        if (mbi.Type == MEM_IMAGE) {
            // image pages of .data are PAGE_WRITECOPY until first written: both are plain data
            if (prot != PAGE_READWRITE && prot != PAGE_WRITECOPY) {
                if (m.view.toRva(a) == UINT64_MAX) return WriteCheck::OtherModule;
                return WriteCheck::NotReadWrite;
            }
            // first and last byte of the part inside this region must both be in a Data section
            uintptr_t last = (end < regionEnd ? end : regionEnd) - 1;
            for (uintptr_t p : {a, last}) {
                uint64_t rva = m.view.toRva(p);
                if (rva == UINT64_MAX) return WriteCheck::OtherModule;
                const probe::Section* s = m.view.sectionOf(rva);
                if (!s || s->kind != probe::SectionKind::Data) return WriteCheck::ImageNotData;
            }
        }
        a = regionEnd;
    }
    return WriteCheck::Ok;
}

bool GuardedMemory::read(uint64_t addr, void* out, size_t n) { return probe::mem::read(uintptr_t(addr), out, n); }

bool GuardedMemory::write(uint64_t addr, const void* in, size_t n, std::string* why) {
    if (n == 0 || n > 8) {
        if (why) *why = "bad size";
        return false;
    }
    WriteCheck c = checkWritable(mod_, uintptr_t(addr), n);
    if (c != WriteCheck::Ok) {
        if (why) *why = toString(c);
        return false;
    }
    SIZE_T done = 0;
    if (!WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(uintptr_t(addr)), in, n, &done) || done != n) {
        if (why) *why = "WriteProcessMemory failed";
        return false;
    }
    uint8_t back[8];
    if (!probe::mem::read(uintptr_t(addr), back, n) || std::memcmp(back, in, n) != 0) {
        if (why) *why = "readback differs";
        return false;
    }
    return true;
}

}  // namespace qs::apply
