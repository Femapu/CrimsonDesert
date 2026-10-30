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

// ------------------------------------------------------------------------------------------------
// Additions (texture + recipes stream). Existing signatures above are unchanged.

// Bytes per 4x4 block (8 or 16) for block formats, 0 for raw formats.
uint32_t blockBytes(Format f);
// Size in bytes of one w x h surface in format f (BC formats round up to whole blocks).
size_t surfaceSize(Format f, uint32_t w, uint32_t h);
// Dimension of mip `level` (never below 1).
inline uint32_t mipDim(uint32_t d, size_t level) { return (d >> level) ? (d >> level) : 1u; }

// Decodes one raw surface (block data or uncompressed pixels) to RGBA8.
// BC1 3-colour blocks decode selector 3 as transparent black (punch-through alpha, alpha 0).
bool decodeSurface(Format f, const uint8_t* data, size_t size, uint32_t w, uint32_t h, Image& out,
                   std::string* error = nullptr);

// Encodes one image. `reference` (optional) is the ORIGINAL encoding of the same surface: every block
// whose pixels decode identically from the reference keeps the reference bytes verbatim (the encoder
// cannot do better, and untouched regions keep the original LZ4 structure). With fitBias > 0 reference
// blocks within a small error are also kept. Deterministic: same input -> same bytes everywhere.
//
// BC1: a block with any alpha < 128 is encoded in 3-colour punch-through mode (alpha test cut-outs
// are preserved exactly: alpha < 128 <-> transparent texel). BC3: alpha via BC4-style block.
// BC4: channel R. BC5: channels R and G.
bool encodeSurface(Format f, const Image& img, const EncodeOptions& opt, std::vector<uint8_t>& out,
                   const uint8_t* reference = nullptr, size_t referenceSize = 0, std::string* error = nullptr);

// Same as encodeMip() with a reference (original bytes of that mip, raw block data).
bool encodeMipRef(const Texture& t, size_t mip, const EncodeOptions& opt, const uint8_t* reference,
                  size_t referenceSize, std::vector<uint8_t>& out, std::string* error = nullptr);

// Mip generation options. Alpha coverage preservation keeps, on every mip, the same fraction of
// texels passing the alpha test (alpha >= alphaRef) as mip 0, so alpha-tested foliage does not thin
// out or vanish in the distance (classic mip problem, worse after winter leaf removal).
struct MipOptions {
    bool srgb = true;      // filter colour in linear light
    int coverage = -1;     // -1 auto (on if mip 0 has both alpha-tested-out and kept texels), 0 off, 1 on
    uint8_t alphaRef = 128;
};
void rebuildMips(Texture& t, const MipOptions& opt);
// Builds the next mip of `src` (2x2 box; colour weighted by alpha so transparent texels, black in BC1
// punch-through, do not bleed into the leaf edges).
Image downsample(const Image& src, bool srgb);
Image downsample(const Image& src, bool srgb, bool alphaWeighted);
// Fraction of texels with alpha >= ref.
double alphaCoverage(const Image& img, uint8_t ref);
// Scales the alpha of `img` (integer search) so that alphaCoverage(img, ref) is as close as possible
// to `target`.
void matchAlphaCoverage(Image& img, double target, uint8_t ref);

// Colour-only fallback ("recolour"): applies a pointwise colour function to the two endpoint colours
// of every BC1/BC3 colour block and keeps the selectors (and the BC3 alpha half) byte identical.
// Because identical input endpoints map to identical output endpoints, the LZ4 structure of the
// original is almost entirely preserved, so the result nearly always fits the original stored size.
// `fn` receives a 1-row image with the unique endpoint colours (alpha 255) and transforms it in place
// (it must be pointwise: no spatial effects, no alpha changes).
using PaletteFn = void (*)(void* user, Image& colours);
bool recolorSurface(Format f, const uint8_t* blocks, size_t size, PaletteFn fn, void* user,
                    std::vector<uint8_t>& out, std::string* error = nullptr);

// One-call helper for the generator. Encodes mip `mip` of the (already transformed) texture so that
// it can be stored in EXACTLY `targetStored` bytes, the original stored size of that mip:
//   * targetStored >= raw size: the mip is stored raw, `stored` = encoded blocks (fitBias = base);
//   * otherwise fitBias is raised (0, then 10, then binary search) until LZ4-HC compresses the blocks
//     to <= targetStored, then lz4CompressExact() pads the block to exactly targetStored bytes;
//   * if even fitBias 10 does not fit and `recolor` is given, the recolour fallback is tried on
//     `original` (fitBias reported as -1).
// `original` = the original raw block data of the mip (decoded from the game file), used as the
// encoder reference and for the fallback. Deterministic.
struct FitResult {
    bool ok = false;
    bool raw = false;               // mip is stored uncompressed
    std::vector<uint8_t> blocks;    // encoded surface, raw size
    std::vector<uint8_t> stored;    // bytes to write: LZ4 block of exactly targetStored bytes, or blocks
    int fitBias = 0;                // level used, -1 = recolour fallback
    size_t hcSize = 0;              // LZ4-HC size of `blocks`
    int trials = 0;                 // encodes performed
    std::string error;
};
struct RecolorFallback {
    PaletteFn fn = nullptr;
    void* user = nullptr;
};
FitResult fitMip(const Texture& t, size_t mip, size_t targetStored, const uint8_t* original, size_t originalSize,
                 const EncodeOptions& base, const RecolorFallback* recolor = nullptr);

// Smallest LZ4 block size reachable (LZ4-HC max level), i.e. lz4CompressExact() succeeds for any
// target >= this value.
size_t lz4BestSize(const uint8_t* data, size_t size);

}  // namespace qs::tex
