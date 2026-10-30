// QuattroStagioni - Bob Jenkins lookup3 "hashlittle" (public domain algorithm).
// Crimson Desert uses hashlittle(data, 0xC5EDE) for pamt/papgt checksums, paz checksums,
// pathc keys and the ChaCha20 seed. We use it for fingerprints and range verification.
#pragma once
#include <cstddef>
#include <cstdint>

namespace qs {

constexpr uint32_t kCdHashSeed = 0x000C5EDEu;

// Returns the primary 32-bit hash ("c") of lookup3 hashlittle() for a byte buffer.
uint32_t hashlittle(const void* data, size_t length, uint32_t initval);

inline uint32_t cdHash(const void* data, size_t length) { return hashlittle(data, length, kCdHashSeed); }

}  // namespace qs
