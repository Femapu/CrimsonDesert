// QuattroStagioni - ChaCha20 entry crypto. See chacha20.h.
#include "chacha20.h"

#include <cstring>

#include "../lookup3.h"

namespace qs::crypto {
namespace {

inline uint32_t rotl(uint32_t x, int k) { return (x << k) | (x >> (32 - k)); }
inline uint32_t le32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
inline void put32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); p[2] = uint8_t(v >> 16); p[3] = uint8_t(v >> 24);
}

inline void quarter(uint32_t& a, uint32_t& b, uint32_t& c, uint32_t& d) {
    a += b; d ^= a; d = rotl(d, 16);
    c += d; b ^= c; b = rotl(b, 12);
    a += b; d ^= a; d = rotl(d, 8);
    c += d; b ^= c; b = rotl(b, 7);
}

void block(const uint32_t in[16], uint8_t out[64]) {
    uint32_t x[16];
    std::memcpy(x, in, sizeof(x));
    for (int i = 0; i < 10; ++i) {
        quarter(x[0], x[4], x[8], x[12]); quarter(x[1], x[5], x[9], x[13]);
        quarter(x[2], x[6], x[10], x[14]); quarter(x[3], x[7], x[11], x[15]);
        quarter(x[0], x[5], x[10], x[15]); quarter(x[1], x[6], x[11], x[12]);
        quarter(x[2], x[7], x[8], x[13]); quarter(x[3], x[4], x[9], x[14]);
    }
    for (int i = 0; i < 16; ++i) put32(out + 4 * i, x[i] + in[i]);
}

const uint32_t kDeltas[8] = {0, 0x0A0A0A0Au, 0x0C0C0C0Cu, 0x06060606u, 0x0E0E0E0Eu, 0x0A0A0A0Au, 0x06060606u, 0x02020202u};

}  // namespace

uint8_t keyMaterialFromTag(uint32_t tag) { return uint8_t((tag >> 8) ^ (tag >> 16) ^ (tag >> 24)); }

ChaChaKeyIv deriveKeyIv(const std::string& pathOrBasename, uint8_t material) {
    size_t cut = pathOrBasename.find_last_of("/\\");
    std::string base = cut == std::string::npos ? pathOrBasename : pathOrBasename.substr(cut + 1);
    for (char& c : base)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    const uint32_t seed = cdHash(base.data(), base.size());
    ChaChaKeyIv k;
    for (int i = 0; i < 4; ++i) put32(k.iv + 4 * i, seed);
    // The default material (0x6D) is folded into the lazorr constant; other material changes every byte.
    const uint32_t adjust = uint32_t(uint8_t(material ^ kDefaultKeyMaterial)) * 0x01010101u;
    const uint32_t kb = seed ^ 0x60616263u;
    for (int i = 0; i < 8; ++i) put32(k.key + 4 * i, (kb ^ kDeltas[i]) ^ adjust);
    return k;
}

void chacha20Xor(const uint8_t key[32], const uint8_t iv[16], uint8_t* data, size_t n) {
    uint32_t st[16];
    st[0] = 0x61707865u; st[1] = 0x3320646eu; st[2] = 0x79622d32u; st[3] = 0x6b206574u;
    for (int i = 0; i < 8; ++i) st[4 + i] = le32(key + 4 * i);
    for (int i = 0; i < 4; ++i) st[12 + i] = le32(iv + 4 * i);
    uint8_t ks[64];
    while (n > 0) {
        block(st, ks);
        const size_t take = n < 64 ? n : 64;
        for (size_t i = 0; i < take; ++i) data[i] ^= ks[i];
        data += take;
        n -= take;
        if (++st[12] == 0) ++st[13];  // OpenSSL-style carry (see header)
    }
}

void cryptEntry(const std::string& pathOrBasename, uint8_t* data, size_t n, uint8_t material) {
    ChaChaKeyIv k = deriveKeyIv(pathOrBasename, material);
    chacha20Xor(k.key, k.iv, data, n);
}

}  // namespace qs::crypto
