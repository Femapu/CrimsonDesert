// QuattroStagioni - range substitution logic (platform independent, unit tested on Linux).
#include "range_patch.h"

#include <algorithm>
#include <cstring>

namespace qs {

const char* toString(PatchResult r) {
    switch (r) {
        case PatchResult::NoOverlap: return "no-overlap";
        case PatchResult::Patched: return "patched";
        case PatchResult::AlreadyPatched: return "already-patched";
        case PatchResult::Identical: return "identical-bytes";
        case PatchResult::Mismatch: return "MISMATCH(untouched)";
    }
    return "?";
}

bool overlaps(const Substitution& s, uint64_t fileOffset, uint64_t length) {
    if (!s.valid() || length == 0) return false;
    const uint64_t readEnd = fileOffset + length;
    return fileOffset < s.end() && readEnd > s.offset;
}

PatchInfo applyToRead(const Substitution& s, uint64_t fileOffset, uint8_t* buffer, size_t length) {
    PatchInfo info;
    if (!buffer || !overlaps(s, fileOffset, length)) return info;

    const uint64_t lo = std::max<uint64_t>(fileOffset, s.offset);
    const uint64_t hi = std::min<uint64_t>(fileOffset + length, s.end());
    info.overlapBegin = lo;
    info.overlapEnd = hi;

    uint8_t* dst = buffer + (lo - fileOffset);           // position inside the read buffer
    const size_t srcPos = size_t(lo - s.offset);         // position inside the entry
    const size_t n = size_t(hi - lo);

    const bool isOriginal = std::memcmp(dst, s.original.data() + srcPos, n) == 0;
    const bool isReplacement = std::memcmp(dst, s.replacement.data() + srcPos, n) == 0;
    if (isOriginal && isReplacement) {
        info.result = PatchResult::Identical;
    } else if (isOriginal) {
        std::memcpy(dst, s.replacement.data() + srcPos, n);
        info.result = PatchResult::Patched;
    } else if (isReplacement) {
        info.result = PatchResult::AlreadyPatched;
    } else {
        info.result = PatchResult::Mismatch;
    }
    return info;
}

}  // namespace qs
