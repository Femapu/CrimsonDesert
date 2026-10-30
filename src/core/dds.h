// QuattroStagioni - DDS helpers for Crimson Desert "per-mip" stored textures (pamt compression 1).
//
// Stored layout: the 128-byte (or 148-byte DX10) DDS header is stored raw; dwReserved1[0..3]
// (header offset 0x20) hold the stored sizes of mips 0..3. A mip whose stored size is smaller than
// its raw size is one LZ4 block, otherwise it is raw. Mips 4+ are raw. meta/0.pathc keeps the same
// header template and the same four stored sizes, so a replacement must keep them identical.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace qs {

struct DdsInfo {
    uint32_t width = 0, height = 0, mipCount = 0;
    uint32_t headerSize = 0;           // 128 or 148
    std::string format;                // "DXT1", "DXT5", "DXGI_98", "RGBA32"...
    std::vector<uint32_t> mipRawSize;  // per mip
    uint32_t storedMip[4] = {0, 0, 0, 0};
};

// Parses a DDS header. Returns false for unknown formats.
bool parseDdsHeader(const uint8_t* data, size_t size, DdsInfo& out, std::string* error = nullptr);

// Validates a type-1 stored blob of `storedSize` bytes that must decode to `rawSize` bytes:
// layout sizes add up and every LZ4 mip decodes strictly (all input consumed, exact output size).
// If `decoded` is non-null it receives the full decoded DDS (header with reserved fields kept).
bool validatePerMipDds(const uint8_t* blob, size_t storedSize, size_t rawSize, std::string* error = nullptr,
                       std::vector<uint8_t>* decoded = nullptr);

}  // namespace qs
