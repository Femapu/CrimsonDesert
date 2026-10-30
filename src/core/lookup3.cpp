// QuattroStagioni - lookup3 hashlittle(), byte-wise implementation (endian/alignment safe).
// Algorithm by Bob Jenkins, 2006, placed in the public domain.
#include "lookup3.h"

namespace qs {
namespace {

inline uint32_t rot(uint32_t x, int k) { return (x << k) | (x >> (32 - k)); }

inline void mix(uint32_t& a, uint32_t& b, uint32_t& c) {
    a -= c; a ^= rot(c, 4);  c += b;
    b -= a; b ^= rot(a, 6);  a += c;
    c -= b; c ^= rot(b, 8);  b += a;
    a -= c; a ^= rot(c, 16); c += b;
    b -= a; b ^= rot(a, 19); a += c;
    c -= b; c ^= rot(b, 4);  b += a;
}

inline void finalMix(uint32_t& a, uint32_t& b, uint32_t& c) {
    c ^= b; c -= rot(b, 14);
    a ^= c; a -= rot(c, 11);
    b ^= a; b -= rot(a, 25);
    c ^= b; c -= rot(b, 16);
    a ^= c; a -= rot(c, 4);
    b ^= a; b -= rot(a, 14);
    c ^= b; c -= rot(b, 24);
}

inline uint32_t le32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

}  // namespace

uint32_t hashlittle(const void* data, size_t length, uint32_t initval) {
    const uint8_t* k = static_cast<const uint8_t*>(data);
    uint32_t a, b, c;
    a = b = c = 0xdeadbeefu + static_cast<uint32_t>(length) + initval;

    while (length > 12) {
        a += le32(k);
        b += le32(k + 4);
        c += le32(k + 8);
        mix(a, b, c);
        length -= 12;
        k += 12;
    }
    if (length == 0) return c;  // zero-length tail: no final mix (matches reference)

    uint8_t tail[12] = {0};
    for (size_t i = 0; i < length; ++i) tail[i] = k[i];
    a += le32(tail);
    b += le32(tail + 4);
    c += le32(tail + 8);
    finalMix(a, b, c);
    return c;
}

}  // namespace qs
