// QuattroStagioni - IDA-style byte patterns ("48 8B 0D ?? ?? ?? ?? 48 85 C9") and scanning.
//
// Portable (no Win32): used in-process by the runtime probes (src/win/probe/) over the mapped game
// module, and by the native unit tests. tools/sigscan.py implements the same syntax offline.
// Syntax: hex byte tokens separated by spaces, "?" or "??" = wildcard byte. A compact form without
// spaces ("488B0D????????") is also accepted. Patterns are short and published in the repo (data/),
// never whole functions.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace qs::probe {

struct Pattern {
    std::vector<uint8_t> bytes;  // value to compare (0 for wildcards)
    std::vector<uint8_t> mask;   // 1 = byte must match, 0 = wildcard
    size_t anchorOffset = 0;     // longest run of fixed bytes, searched first
    size_t anchorLength = 0;

    size_t size() const { return bytes.size(); }
    bool empty() const { return bytes.empty(); }
    bool matchesAt(const uint8_t* p) const;  // p must have size() readable bytes
};

// Returns false (and sets *err) on malformed text or a pattern made only of wildcards.
bool parsePattern(const std::string& text, Pattern& out, std::string* err = nullptr);
std::string formatPattern(const Pattern& p);  // canonical "48 8B ?? ..." form

// All match offsets in [data, data+size), in increasing order, stopping after maxHits.
std::vector<size_t> findAll(const uint8_t* data, size_t size, const Pattern& p, size_t maxHits = SIZE_MAX);
// First match offset or SIZE_MAX.
size_t findFirst(const uint8_t* data, size_t size, const Pattern& p);

// RIP-relative helpers. An instruction at address `insn` (any address space: RVA or VA) with a
// signed 32-bit displacement at insn+dispOffset and total length insnLen targets insn+insnLen+disp.
inline uint64_t ripTarget(uint64_t insn, size_t insnLen, int32_t disp) {
    return insn + insnLen + uint64_t(int64_t(disp));
}
int32_t readI32(const uint8_t* p);
uint32_t readU32(const uint8_t* p);
uint64_t readU64(const uint8_t* p);

// "rip:3:7" -> dispOffset 3, insnLen 7. "call" / "jmp" are shorthands for rip:1:5 (E8/E9 rel32).
struct RipSpec {
    bool enabled = false;
    size_t dispOffset = 0;
    size_t insnLen = 0;
};
bool parseRipSpec(const std::string& text, RipSpec& out);

}  // namespace qs::probe
