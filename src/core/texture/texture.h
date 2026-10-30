// QuattroStagioni - texture codec: DDS (plain, already decompressed) <-> RGBA8 mip chain.
//
// Contract (docs/dev/CONTRACTS.md, owner: recipes stream). Formats needed by the inventory:
// DXT1 (BC1), DXT5 (BC3), BC4U, BC5U; the "rgbNbpp" / R8 entries are uncompressed and handled raw.
// The encoder must be deterministic (same input -> same bytes on every PC and compiler) because the
// generator output is cross-checked by hash.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "../dds.h"

namespace qs::tex {

enum class Format { Unknown, BC1, BC3, BC4, BC5, RGBA8, BGRA8, R8 };

struct Image {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> rgba;  // width * height * 4, row major, straight (not premultiplied) alpha
};

struct Texture {
    Format format = Format::Unknown;
    std::vector<uint8_t> header;  // original DDS header bytes, reused verbatim on encode
    std::vector<Image> mips;      // mip 0 first; every mip of the original is present
};

// Encoder knobs. `level` trades quality for speed; `fitBias` > 0 asks the encoder to prefer
// LZ4-friendly blocks (fewer distinct index patterns, quantized endpoints) at some quality cost.
// The generator raises fitBias step by step until the re-encoded mip fits the original stored size.
struct EncodeOptions {
    int level = 2;      // 0 fast .. 3 best
    int fitBias = 0;    // 0 = pure quality, up to 10
};

Format formatFromDds(const DdsInfo& info);
const char* toString(Format f);

// Decodes a whole plain DDS file (header + all mips). Returns false on unsupported data.
bool decodeDds(const uint8_t* dds, size_t size, Texture& out, std::string* error = nullptr);

// Encodes one mip of `t` into the block format of `t.format`. Output size equals the raw mip size.
bool encodeMip(const Texture& t, size_t mip, const EncodeOptions& opt, std::vector<uint8_t>& out,
               std::string* error = nullptr);

// Encodes the whole texture back into a plain DDS file with the original header (byte identical).
bool encodeDds(const Texture& t, const EncodeOptions& opt, std::vector<uint8_t>& out, std::string* error = nullptr);

// Rebuilds mips 1..n from mip 0 (box filter in linear light for colour, plain for alpha/data).
void rebuildMips(Texture& t, bool srgb);

}  // namespace qs::tex
