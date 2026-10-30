// QuattroStagioni - fail-safe memory access for the runtime probes.
//
// mingw has no __try/__except, so reads never dereference unknown pointers directly: the range is
// checked with VirtualQuery (committed, readable, no PAGE_GUARD) and then copied with
// ReadProcessMemory on our own process, which returns FALSE instead of faulting if the page vanishes
// between the check and the copy. Slower than a plain load, fine for periodic probes.
#pragma once
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <functional>

namespace qs::probe::mem {

bool readableProtect(DWORD protect);
// True if every page of [p, p+n) is committed and readable.
bool isReadable(const void* p, size_t n);
bool read(uintptr_t addr, void* out, size_t n);
template <class T>
bool readT(uintptr_t addr, T& out) {
    return read(addr, &out, sizeof(T));
}
// Enumerates committed private read-write regions (heap memory), in address order, until the
// callback returns false.
void forEachHeapRegion(const std::function<bool(uintptr_t base, size_t size)>& cb);
// Same, starting from the region containing `start` (or the next one).
void forEachHeapRegionFrom(uintptr_t start, const std::function<bool(uintptr_t base, size_t size)>& cb);

}  // namespace qs::probe::mem
