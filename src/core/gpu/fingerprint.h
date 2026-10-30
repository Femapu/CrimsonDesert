// QuattroStagioni - GPU live swap, part 1: texture layouts and mip fingerprints (portable, unit tested).
//
// Why: the season packs only change texture bytes when the game READS a .paz, so textures that are
// already resident in VRAM keep the old stage until the game streams them out. The GPU swap module
// (src/win/gpu/) watches the game's uploads (ID3D12GraphicsCommandList::CopyTextureRegion from a
// mapped UPLOAD buffer), recognises which texture a destination resource holds by a fingerprint of the
// uploaded mip bytes, and later rewrites resident textures when the stage changes.
//
// A mip fingerprint ("print") is a 64-bit hash of a SAMPLE of the packed mip data (the bytes exactly as
// stored in the decoded DDS: rows of 4x4 blocks, no padding) mixed with the format family and the mip
// dimensions. Sampling instead of hashing a prefix: foliage atlases often start with fully transparent
// rows, so the first bytes of many same-sized textures are identical. The sample is kSampleSegments
// segments of kSegmentBytes taken from evenly spaced block rows, centred in the row; mips smaller than
// the sample are hashed whole. Reading a sample from an upload buffer costs a few uncached 64-byte
// reads, which is what keeps the CopyTextureRegion hook cheap.
//
// The same function computes the print from a decoded DDS (generator, pack loader, tests) and from a
// D3D12 placed footprint (rows separated by a RowPitch >= the packed row size), so both sides agree.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace qs::gpu {

// Format families: UNORM / SRGB / TYPELESS variants of the same block layout compare equal, because the
// game may create a BC1_UNORM_SRGB resource for a "DXT1" DDS.
enum class Family : uint8_t { Unknown = 0, BC1, BC2, BC3, BC4, BC5, BC6H, BC7, R8, RG8, RGBA8, BGRA8 };

const char* toString(Family f);
// DXGI_FORMAT value -> family (Unknown for formats we never swap).
Family familyFromDxgi(uint32_t dxgiFormat);
// DDS header format string as produced by qs::parseDdsHeader ("DXT1", "DXT5", "ATI2", "DXGI_98", "RGB32"...).
Family familyFromDds(const std::string& ddsFormat);
bool isBlockCompressed(Family f);
// Bytes of one 4x4 block (BC) or one pixel (raw).
uint32_t unitBytes(Family f);

// Packed layout of one mip (the decoded DDS layout, also what a placed footprint holds row by row).
struct MipLayout {
    uint32_t width = 0, height = 0;  // texels
    uint32_t rowBytes = 0;           // one row of blocks (BC) or pixels (raw), unpadded
    uint32_t rows = 0;               // block rows (BC) or pixel rows (raw)
    uint64_t size() const { return uint64_t(rowBytes) * rows; }
};
MipLayout mipLayout(Family f, uint32_t width, uint32_t height, uint32_t mip);

// D3D12 copyable footprint rules (D3D12_TEXTURE_DATA_PITCH_ALIGNMENT = 256,
// D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT = 512), used to lay several subresources into one upload buffer.
constexpr uint32_t kPitchAlign = 256;
constexpr uint32_t kPlacementAlign = 512;
struct Footprint {
    uint64_t offset = 0;   // from the start of the upload area, 512 aligned
    uint32_t rowPitch = 0; // 256 aligned
    MipLayout layout;
    uint64_t bytes() const { return uint64_t(rowPitch) * (layout.rows ? layout.rows - 1 : 0) + layout.rowBytes; }
};
// Lays mips [firstMip, firstMip + count) of a width x height texture one after the other starting at
// `base` (rounded up to 512). Returns the total size used from `base` (end of the last mip - base).
uint64_t layoutFootprints(Family f, uint32_t width, uint32_t height, uint32_t firstMip, uint32_t count, uint64_t base,
                          std::vector<Footprint>& out);
// Copies packed mip bytes into a footprint (rowPitch) and back.
void packedToFootprint(const uint8_t* packed, const MipLayout& l, uint8_t* dst, uint32_t rowPitch);
void footprintToPacked(const uint8_t* src, uint32_t rowPitch, const MipLayout& l, uint8_t* packed);

// ---- fingerprints --------------------------------------------------------------------------------
constexpr uint32_t kSampleSegments = 4;
constexpr uint32_t kSegmentBytes = 64;
constexpr uint32_t kSampleBytes = kSampleSegments * kSegmentBytes;

// Print of a mip whose rows start at `data` with `rowPitch` bytes between rows (rowPitch == rowBytes
// for packed data). `l` must describe the mip (mipLayout()). Reads at most kSampleBytes bytes (whole
// mip if smaller). Never returns 0 (0 means "no print").
uint64_t mipPrint(Family f, const MipLayout& l, const uint8_t* data, uint32_t rowPitch);
inline uint64_t mipPrintPacked(Family f, const MipLayout& l, const uint8_t* packed) {
    return mipPrint(f, l, packed, l.rowBytes);
}

// Prints of every mip of one texture (decoded DDS). This is what the generator should store per pack
// record (see docs/dev/gpu_swap.md, "Campi del pacchetto").
struct TexturePrints {
    Family family = Family::Unknown;
    uint32_t dxgiFormat = 0;      // as in the DDS (DX10 header) or the usual mapping for FourCC
    uint32_t width = 0, height = 0, mipCount = 0;
    std::vector<uint64_t> mips;   // one per mip, mip 0 first
    std::vector<uint64_t> mipOffset;  // byte offset of each mip inside the decoded DDS (after the header)
    uint32_t headerSize = 0;
};
// From a whole decoded DDS (header + all mips, plain block data). False for unsupported formats.
bool computePrints(const uint8_t* dds, size_t size, TexturePrints& out, std::string* error = nullptr);
// From a Crimson Desert per-mip stored blob (pamt compression 1: LZ4 mips 0..3), as found in a .paz or
// a .qsp payload. `decoded` (optional) receives the decoded DDS.
bool computePrintsStored(const uint8_t* stored, size_t storedSize, TexturePrints& out, std::string* error = nullptr,
                         std::vector<uint8_t>* decoded = nullptr);
// Decodes a per-mip stored blob without knowing the decoded entry size in advance.
bool decodeStored(const uint8_t* stored, size_t storedSize, std::vector<uint8_t>& decoded, std::string* error = nullptr);

}  // namespace qs::gpu
