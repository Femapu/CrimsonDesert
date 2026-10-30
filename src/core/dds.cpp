// QuattroStagioni - DDS per-mip validation.
#include "dds.h"

#include <cstring>

#include "lz4.h"

namespace qs {
namespace {

inline uint32_t rd32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

// Bytes per 4x4 block for block-compressed DXGI formats, 0 otherwise.
uint32_t dxgiBlockBytes(uint32_t f) {
    switch (f) {
        case 70: case 71: case 72: case 79: case 80: case 81: return 8;               // BC1, BC4
        case 73: case 74: case 75: case 76: case 77: case 78:                           // BC2, BC3
        case 82: case 83: case 84: case 94: case 95: case 96: case 97: case 98: case 99:  // BC5, BC6H, BC7
            return 16;
    }
    return 0;
}

// Bits per pixel for common uncompressed DXGI formats, 0 if unknown.
uint32_t dxgiBpp(uint32_t f) {
    switch (f) {
        case 2: return 128;
        case 10: case 11: case 13: return 64;
        case 24: case 26: case 27: case 28: case 29: case 87: case 88: case 91: case 41: case 42: case 43: return 32;
        case 49: case 54: case 56: case 57: return 16;
        case 61: case 62: case 65: return 8;
    }
    return 0;
}

}  // namespace

bool parseDdsHeader(const uint8_t* d, size_t size, DdsInfo& out, std::string* error) {
    out = DdsInfo();
    if (size < 128 || std::memcmp(d, "DDS ", 4) != 0 || rd32(d + 4) != 124) {
        if (error) *error = "not a DDS header";
        return false;
    }
    out.height = rd32(d + 12);
    out.width = rd32(d + 16);
    out.mipCount = rd32(d + 28) ? rd32(d + 28) : 1;
    for (int i = 0; i < 4; ++i) out.storedMip[i] = rd32(d + 0x20 + 4 * i);
    const uint32_t pfFlags = rd32(d + 80);
    const uint8_t* fourcc = d + 84;
    uint32_t blockBytes = 0, bpp = 0;
    out.headerSize = 128;
    if (pfFlags & 4) {  // DDPF_FOURCC
        out.format.assign(reinterpret_cast<const char*>(fourcc), 4);
        if (out.format == "DX10") {
            if (size < 148) { if (error) *error = "truncated DX10 header"; return false; }
            out.headerSize = 148;
            uint32_t f = rd32(d + 128);
            out.format = "DXGI_" + std::to_string(f);
            blockBytes = dxgiBlockBytes(f);
            bpp = dxgiBpp(f);
        } else if (out.format == "DXT1" || out.format == "ATI1" || out.format == "BC4U" || out.format == "BC4S") {
            blockBytes = 8;
        } else if (out.format == "DXT3" || out.format == "DXT5" || out.format == "ATI2" || out.format == "BC5U" ||
                   out.format == "BC5S" || out.format == "DXT2" || out.format == "DXT4") {
            blockBytes = 16;
        }
    } else {
        bpp = rd32(d + 88);
        out.format = "RGB" + std::to_string(bpp);
    }
    if (!blockBytes && !bpp) { if (error) *error = "unsupported pixel format " + out.format; return false; }
    if (out.width == 0 || out.height == 0 || out.mipCount > 16) { if (error) *error = "bad dimensions"; return false; }
    for (uint32_t i = 0; i < out.mipCount; ++i) {
        uint32_t w = out.width >> i ? out.width >> i : 1, h = out.height >> i ? out.height >> i : 1;
        uint64_t s = blockBytes ? uint64_t((w + 3) / 4) * ((h + 3) / 4) * blockBytes : uint64_t(w) * h * bpp / 8;
        out.mipRawSize.push_back(uint32_t(s));
    }
    return true;
}

bool validatePerMipDds(const uint8_t* blob, size_t storedSize, size_t rawSize, std::string* error,
                       std::vector<uint8_t>* decoded) {
    DdsInfo info;
    if (!parseDdsHeader(blob, storedSize, info, error)) return false;
    size_t p = info.headerSize, outSize = info.headerSize;
    if (decoded) decoded->assign(blob, blob + info.headerSize);
    for (uint32_t i = 0; i < info.mipCount; ++i) {
        const uint32_t raw = info.mipRawSize[i];
        const uint32_t st = i < 4 ? info.storedMip[i] : raw;
        if (st == 0 || p + st > storedSize) { if (error) *error = "mip " + std::to_string(i) + " exceeds stored size"; return false; }
        if (st < raw) {
            std::vector<uint8_t> tmp(raw);
            int r = LZ4_decompress_safe(reinterpret_cast<const char*>(blob + p), reinterpret_cast<char*>(tmp.data()),
                                        int(st), int(raw));
            if (r != int(raw)) {
                if (error) *error = "mip " + std::to_string(i) + " LZ4 decode returned " + std::to_string(r) + ", expected " + std::to_string(raw);
                return false;
            }
            if (decoded) decoded->insert(decoded->end(), tmp.begin(), tmp.end());
        } else if (st == raw) {
            if (decoded) decoded->insert(decoded->end(), blob + p, blob + p + st);
        } else {
            if (error) *error = "mip " + std::to_string(i) + " stored size larger than raw size";
            return false;
        }
        p += st;
        outSize += raw;
    }
    if (p != storedSize) { if (error) *error = "layout uses " + std::to_string(p) + " of " + std::to_string(storedSize) + " stored bytes"; return false; }
    if (outSize != rawSize) { if (error) *error = "decoded size " + std::to_string(outSize) + " != entry size " + std::to_string(rawSize); return false; }
    return true;
}

}  // namespace qs
