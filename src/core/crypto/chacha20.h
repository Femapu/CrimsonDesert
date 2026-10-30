// QuattroStagioni - ChaCha20 for archive entries flagged crypto 3 (pamt flags high nibble).
//
// Cipher: plain ChaCha20 (D. J. Bernstein, 20 rounds) with a 16-byte IV laid out like OpenSSL /
// Python "cryptography": IV bytes 0..3 = initial 32-bit block counter (little endian), 4..15 = nonce.
// If the counter wraps it carries into the first nonce word (OpenSSL behaviour); with Crimson Desert
// entry sizes this practically never happens (it needs more than (2^32 - seed) * 64 bytes).
//
// Key/IV derivation (lazorr410 unpacker, MIT; pycrimson, MIT), from the entry's file basename:
//   seed = hashlittle(lower(basename), 0xC5EDE)
//   iv   = seed (LE u32) repeated 4 times
//   key  = 8 LE u32 words: (seed ^ 0x60616263) ^ delta[i] for the default key material.
// pycrimson's general form XORs every key byte with m = info[0]^info[1]^info[2], the three high
// bytes of the pamt format tag (word at offset 8). The default tag 0x610E0232 gives m = 0x6D, which
// reproduces the lazorr formula exactly; other groups' material is honoured through `material`.
// Encryption and decryption are the same XOR operation.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace qs::crypto {

constexpr uint8_t kDefaultKeyMaterial = 0x6D;  // 0x02 ^ 0x0E ^ 0x61 (pamt tag 0x610E0232)

struct ChaChaKeyIv {
    uint8_t key[32];
    uint8_t iv[16];
};

// Key material byte of a pamt format tag (u32 at pamt offset 8).
uint8_t keyMaterialFromTag(uint32_t pamtFormatTag);

// Derives key and IV from an entry path or basename (only the part after the last '/' or '\\' is used).
ChaChaKeyIv deriveKeyIv(const std::string& pathOrBasename, uint8_t material = kDefaultKeyMaterial);

// XORs `n` bytes in place with the ChaCha20 key stream starting at block counter iv[0..3].
void chacha20Xor(const uint8_t key[32], const uint8_t iv[16], uint8_t* data, size_t n);

// Convenience: derive from the path and XOR in place (encrypts or decrypts).
void cryptEntry(const std::string& pathOrBasename, uint8_t* data, size_t n, uint8_t material = kDefaultKeyMaterial);

}  // namespace qs::crypto
