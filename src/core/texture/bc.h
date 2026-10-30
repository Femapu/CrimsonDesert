// QuattroStagioni - internal block codec (BC1/BC3/BC4/BC5) used by texture.cpp and fit.cpp.
//
// A block is split in 8-byte "units": a colour unit (BC1 style endpoints + 2-bit selectors) or a
// channel unit (BC4 style endpoints + 3-bit selectors). BC1 = [colour], BC3 = [alpha, colour],
// BC4 = [R], BC5 = [R, G]. The rate/distortion pass (fitBias) works on units in stream order, which
// is also the order LZ4 sees them.
#pragma once
#include <cstdint>
#include <vector>

#include "texture.h"

namespace qs::tex::bc {

// 16 texels RGBA plus which of them lie inside the surface (edge blocks of small mips).
struct BlockPx {
    uint8_t px[64];
    uint16_t valid = 0xFFFF;
};

enum class UnitKind : uint8_t { ColorBC1, ColorBC3, Channel };
struct UnitSpec {
    UnitKind kind;
    uint8_t channel;  // for Channel units
};

// Units of a block format (count 1 or 2).
int unitsOf(Format f, UnitSpec out[2]);

void fetchBlock(const Image& img, uint32_t bx, uint32_t by, BlockPx& out);

// Decoders (8-byte unit -> pixels). Colour: writes RGBA of 16 texels (alpha 255 or 0 for BC1
// punch-through). Channel: writes one channel with the given stride.
void decodeColor(const uint8_t* unit, bool bc1Mode, uint8_t* rgba64);
void decodeChannel(const uint8_t* unit, uint8_t* out, int stride);

// Weighted squared error of a unit against the block (huge if a BC1 alpha-test decision changes).
uint64_t unitError(const UnitSpec& u, const uint8_t* unit, const BlockPx& b);

// Best-quality encoding of one unit. `level` 0..3. `transparentUnit` (8 bytes, may be null) is used
// for fully transparent BC1 blocks.
void encodeUnit(const UnitSpec& u, const BlockPx& b, int level, const uint8_t* transparentUnit, uint8_t out[8]);

// Encodes a whole surface with the RDO pass driven by opt.fitBias. `ref` (optional) = original bytes.
void encodeBlocks(Format f, const Image& img, const EncodeOptions& opt, const uint8_t* ref, size_t refSize,
                  std::vector<uint8_t>& out);

// Mode helpers.
inline uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
inline void unpack565(uint16_t c, int& r, int& g, int& b) {
    r = (c >> 11) & 31; g = (c >> 5) & 63; b = c & 31;
    r = (r << 3) | (r >> 2); g = (g << 2) | (g >> 4); b = (b << 3) | (b >> 2);
}

}  // namespace qs::tex::bc
