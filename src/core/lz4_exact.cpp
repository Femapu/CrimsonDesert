// QuattroStagioni - exact-size LZ4 block encoder. See lz4_exact.h for the method.
#include "lz4_exact.h"

#include <climits>
#include <cstdint>
#include <cstring>

#include "lz4.h"
#include "lz4hc.h"

namespace qs {
namespace {

struct Seq {
    size_t litPos = 0;   // position of the literals in the uncompressed data
    size_t litLen = 0;
    size_t matchLen = 0; // 0 for the final (literals only) sequence
    uint32_t offset = 0;
};

// Number of extra length bytes LZ4 needs for a 4-bit length field value `v`.
inline size_t extraBytes(size_t v) { return v < 15 ? 0 : 1 + (v - 15) / 255; }

inline size_t seqSize(const Seq& s) {
    size_t n = 1 + extraBytes(s.litLen) + s.litLen;
    if (s.matchLen) n += 2 + extraBytes(s.matchLen - 4);
    return n;
}

// Parses a well-formed LZ4 block into sequences. Returns false on malformed input.
bool parseBlock(const uint8_t* b, size_t n, size_t rawSize, std::vector<Seq>& out) {
    out.clear();
    size_t p = 0, pos = 0;
    while (p < n) {
        Seq s;
        uint8_t token = b[p++];
        size_t l = token >> 4;
        if (l == 15) {
            uint8_t c;
            do {
                if (p >= n) return false;
                c = b[p++];
                l += c;
            } while (c == 255);
        }
        s.litPos = pos;
        s.litLen = l;
        if (p + l > n) return false;
        p += l;
        pos += l;
        if (p == n) {  // final sequence: literals only
            out.push_back(s);
            return pos == rawSize;
        }
        if (p + 2 > n) return false;
        s.offset = uint32_t(b[p]) | (uint32_t(b[p + 1]) << 8);
        p += 2;
        size_t m = token & 15;
        if (m == 15) {
            uint8_t c;
            do {
                if (p >= n) return false;
                c = b[p++];
                m += c;
            } while (c == 255);
        }
        s.matchLen = m + 4;
        if (s.offset == 0 || s.offset > pos) return false;
        pos += s.matchLen;
        out.push_back(s);
    }
    return false;  // a valid block always ends with a literals-only sequence
}

void emit(const std::vector<Seq>& seqs, const uint8_t* src, std::vector<uint8_t>& out) {
    out.clear();
    auto putLen = [&](size_t v) {
        if (v < 15) return;
        v -= 15;
        while (v >= 255) { out.push_back(255); v -= 255; }
        out.push_back(uint8_t(v));
    };
    for (const Seq& s : seqs) {
        uint8_t token = uint8_t((s.litLen < 15 ? s.litLen : 15) << 4);
        if (s.matchLen) {
            size_t m = s.matchLen - 4;
            token |= uint8_t(m < 15 ? m : 15);
        }
        out.push_back(token);
        putLen(s.litLen);
        out.insert(out.end(), src + s.litPos, src + s.litPos + s.litLen);
        if (s.matchLen) {
            out.push_back(uint8_t(s.offset & 0xFF));
            out.push_back(uint8_t(s.offset >> 8));
            putLen(s.matchLen - 4);
        }
    }
}

// Size change when the last k bytes of match i become literals of sequence i+1.
inline long long shortenDelta(const Seq& a, const Seq& b, size_t k) {
    return (long long)k + (long long)extraBytes(b.litLen + k) - (long long)extraBytes(b.litLen) +
           (long long)extraBytes(a.matchLen - 4 - k) - (long long)extraBytes(a.matchLen - 4);
}

// Size change when match i is removed entirely and sequences i, i+1 are merged.
inline long long dropDelta(const Seq& a, const Seq& b) {
    size_t merged = a.litLen + a.matchLen + b.litLen;
    return (long long)a.matchLen + (long long)extraBytes(merged) - (long long)extraBytes(a.litLen) -
           (long long)extraBytes(b.litLen) - 3 - (long long)extraBytes(a.matchLen - 4);
}

// Grows `seqs` by exactly `gap` bytes. Returns false if no combination of operations reaches it.
bool growBy(std::vector<Seq>& seqs, size_t gap) {
    while (gap > 0) {
        bool progress = false;
        // Pass 1: shorten matches (cheapest, +1 byte per moved byte in most cases).
        for (size_t i = 0; i + 1 < seqs.size() && gap > 0; ++i) {
            Seq& a = seqs[i];
            Seq& b = seqs[i + 1];
            if (a.matchLen <= 4) continue;
            size_t lo = 1, hi = a.matchLen - 4, best = 0;
            while (lo <= hi) {  // largest k with delta(k) <= gap (delta is non-decreasing in k)
                size_t mid = lo + (hi - lo) / 2;
                long long d = shortenDelta(a, b, mid);
                if (d <= (long long)gap) { best = mid; lo = mid + 1; }
                else { if (mid == 0) break; hi = mid - 1; }
            }
            if (!best) continue;
            long long d = shortenDelta(a, b, best);
            if (d <= 0) continue;
            a.matchLen -= best;
            b.litPos -= best;
            b.litLen += best;
            gap -= size_t(d);
            progress = true;
        }
        if (gap == 0) break;
        // Pass 2: drop (now minimum-length) matches whose delta fits, rebuilding the list in one sweep.
        auto merge = [](const Seq& a, const Seq& b) {
            Seq m;
            m.litPos = a.litPos;
            m.litLen = a.litLen + a.matchLen + b.litLen;
            m.matchLen = b.matchLen;
            m.offset = b.offset;
            return m;
        };
        {
            std::vector<Seq> out;
            out.reserve(seqs.size());
            for (size_t i = 0; i < seqs.size(); ++i) {
                Seq cur = seqs[i];
                while (gap > 0 && cur.matchLen != 0 && i + 1 < seqs.size()) {
                    long long d = dropDelta(cur, seqs[i + 1]);
                    if (d <= 0 || d > (long long)gap) break;
                    cur = merge(cur, seqs[i + 1]);
                    ++i;
                    gap -= size_t(d);
                    progress = true;
                }
                out.push_back(cur);
            }
            seqs.swap(out);
        }
        if (gap == 0) break;
        if (progress) continue;
        // Stuck: every remaining operation is either too big or does not grow the block. Merges with
        // delta <= 0 (long literal runs whose length bytes coalesce) unlock further growth; apply all
        // zero-delta merges, or else the single least-negative one. Each merge removes a match, so
        // this terminates (at worst in the all-literals block, the largest possible encoding).
        std::vector<Seq> out;
        out.reserve(seqs.size());
        bool merged = false;
        for (size_t i = 0; i < seqs.size(); ++i) {
            Seq cur = seqs[i];
            if (cur.matchLen != 0 && i + 1 < seqs.size() && dropDelta(cur, seqs[i + 1]) == 0) {
                cur = merge(cur, seqs[i + 1]);
                ++i;
                merged = true;
            }
            out.push_back(cur);
        }
        seqs.swap(out);
        if (merged) continue;
        long long bestD = LLONG_MIN;
        size_t bestI = SIZE_MAX;
        for (size_t i = 0; i + 1 < seqs.size(); ++i) {
            if (seqs[i].matchLen == 0) continue;
            long long d = dropDelta(seqs[i], seqs[i + 1]);
            if (d < 0 && d > bestD) { bestD = d; bestI = i; }
        }
        if (bestI == SIZE_MAX) return false;
        seqs[bestI] = merge(seqs[bestI], seqs[bestI + 1]);
        seqs.erase(seqs.begin() + long(bestI) + 1);
        gap += size_t(-bestD);
    }
    return true;
}

}  // namespace

bool lz4VerifyStrict(const uint8_t* block, size_t blockSize, const uint8_t* expected, size_t expectedSize,
                     std::string* error) {
    std::vector<uint8_t> out(expectedSize + 64);
    int r = LZ4_decompress_safe(reinterpret_cast<const char*>(block), reinterpret_cast<char*>(out.data()),
                                int(blockSize), int(out.size()));
    if (r < 0) { if (error) *error = "LZ4_decompress_safe failed (" + std::to_string(r) + ")"; return false; }
    if (size_t(r) != expectedSize) {
        if (error) *error = "decoded size " + std::to_string(r) + " != " + std::to_string(expectedSize);
        return false;
    }
    if (expected && std::memcmp(out.data(), expected, expectedSize) != 0) {
        if (error) *error = "decoded bytes differ from expected";
        return false;
    }
    return true;
}

size_t lz4DefaultSize(const uint8_t* src, size_t srcSize) {
    std::vector<char> dst(static_cast<size_t>(LZ4_compressBound(static_cast<int>(srcSize))));
    int n = LZ4_compress_default(reinterpret_cast<const char*>(src), dst.data(), int(srcSize), int(dst.size()));
    return n > 0 ? size_t(n) : 0;
}

Lz4ExactResult lz4CompressExact(const uint8_t* src, size_t srcSize, size_t targetSize) {
    Lz4ExactResult res;
    if (!src || srcSize == 0 || srcSize > 0x7E000000u) { res.error = "bad input size"; return res; }
    const size_t maxSize = 1 + extraBytes(srcSize) + srcSize;  // all-literals block: the largest encoding
    if (targetSize > maxSize) {
        res.error = "target larger than the all-literals encoding (" + std::to_string(maxSize) + ")";
        return res;
    }
    std::vector<char> tmp(static_cast<size_t>(LZ4_compressBound(static_cast<int>(srcSize))));
    std::vector<uint8_t> out;
    std::vector<Seq> seqs;
    res.bestSize = SIZE_MAX;

    // Try the best parse first; lower levels give different sequence mixes if the best one cannot
    // reach the target exactly (very rare).
    for (int level = LZ4HC_CLEVEL_MAX; level >= 1; --level) {
        int n = LZ4_compress_HC(reinterpret_cast<const char*>(src), tmp.data(), int(srcSize), int(tmp.size()), level);
        if (n <= 0) continue;
        if (size_t(n) < res.bestSize) res.bestSize = size_t(n);
        // Lower levels never compress better than the optimal parser: stop if even it is too big.
        if (size_t(n) > targetSize) {
            if (level == LZ4HC_CLEVEL_MAX) break;
            continue;
        }
        if (!parseBlock(reinterpret_cast<const uint8_t*>(tmp.data()), size_t(n), srcSize, seqs)) {
            res.error = "internal: could not parse LZ4-HC output";
            continue;
        }
        size_t cur = 0;
        for (const Seq& s : seqs) cur += seqSize(s);
        if (cur != size_t(n)) { res.error = "internal: size model mismatch"; continue; }
        if (!growBy(seqs, targetSize - cur)) continue;
        emit(seqs, src, out);
        std::string err;
        if (out.size() != targetSize) { res.error = "internal: emitted size mismatch"; continue; }
        if (!lz4VerifyStrict(out.data(), out.size(), src, srcSize, &err)) { res.error = "verify: " + err; continue; }
        res.ok = true;
        res.block = std::move(out);
        res.hcLevel = level;
        res.error.clear();
        return res;
    }
    if (res.error.empty()) {
        res.error = (res.bestSize != SIZE_MAX && res.bestSize > targetSize)
                        ? "data does not compress to the target size (best " + std::to_string(res.bestSize) + ")"
                        : "target size not reachable";
    }
    return res;
}

}  // namespace qs
