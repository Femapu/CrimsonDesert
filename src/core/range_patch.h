// QuattroStagioni - in-memory substitution of one archive byte range inside arbitrary read buffers.
//
// A Substitution describes an entry stored in a .paz at [offset, offset + size). The game may read
// that range in any number of chunks, at any alignment, possibly together with neighbouring entries.
// applyToRead() patches only the bytes of a completed read that fall inside the range, and only if
// those bytes are exactly the expected original bytes (or already our replacement). Anything else
// (different data, short read) is left untouched. Sizes never change.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace qs {

struct Substitution {
    std::string name;                   // entry path, for logs
    uint64_t offset = 0;                // absolute offset inside the paz file
    std::vector<uint8_t> original;      // expected original stored bytes (verified at startup)
    std::vector<uint8_t> replacement;   // same length as original
    uint64_t end() const { return offset + original.size(); }
    bool valid() const { return !original.empty() && original.size() == replacement.size(); }
};

enum class PatchResult {
    NoOverlap,       // read does not touch the range
    Patched,         // overlapping bytes were original and are now the replacement
    AlreadyPatched,  // overlapping bytes already equal the replacement (idempotent re-application)
    Identical,       // overlap lies where original and replacement are identical (e.g. DDS header)
    Mismatch,        // overlapping bytes are neither original nor replacement: left untouched
};

const char* toString(PatchResult r);

struct PatchInfo {
    PatchResult result = PatchResult::NoOverlap;
    uint64_t overlapBegin = 0;  // absolute file offsets of the overlap
    uint64_t overlapEnd = 0;
};

// True if a read of `length` bytes at `fileOffset` intersects the substitution range.
bool overlaps(const Substitution& s, uint64_t fileOffset, uint64_t length);

// Applies the substitution to `buffer`, which holds `length` bytes read from `fileOffset`.
PatchInfo applyToRead(const Substitution& s, uint64_t fileOffset, uint8_t* buffer, size_t length);

}  // namespace qs
