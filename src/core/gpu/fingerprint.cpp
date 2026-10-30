// QuattroStagioni - GPU live swap: texture layouts and mip fingerprints.
#include "fingerprint.h"

#include <cstdlib>
#include <cstring>

#include "../dds.h"
#include "../lookup3.h"

namespace qs::gpu {

const char* toString(Family f) {
    switch (f) {
        case Family::BC1: return "BC1";
        case Family::BC2: return "BC2";
        case Family::BC3: return "BC3";
        case Family::BC4: return "BC4";
        case Family::BC5: return "BC5";
        case Family::BC6H: return "BC6H";
        case Family::BC7: return "BC7";
        case Family::R8: return "R8";
        case Family::RG8: return "RG8";
        case Family::RGBA8: return "RGBA8";
        case Family::BGRA8: return "BGRA8";
        default: return "unknown";
    }
}

Family familyFromDxgi(uint32_t f) {
    switch (f) {
        case 70: case 71: case 72: return Family::BC1;
        case 73: case 74: case 75: return Family::BC2;
        case 76: case 77: case 78: return Family::BC3;
        case 79: case 80: case 81: return Family::BC4;
        case 82: case 83: case 84: return Family::BC5;
        case 94: case 95: case 96: return Family::BC6H;
        case 97: case 98: case 99: return Family::BC7;
        case 60: case 61: case 62: case 63: case 64: return Family::R8;
        case 48: case 49: case 50: case 51: case 52: return Family::RG8;
        case 27: case 28: case 29: case 30: case 31: case 32: return Family::RGBA8;
        case 87: case 90: case 91: return Family::BGRA8;
    }
    return Family::Unknown;
}

static uint32_t dxgiFromDds(const std::string& s) {
    if (s.rfind("DXGI_", 0) == 0) return uint32_t(std::strtoul(s.c_str() + 5, nullptr, 10));
    if (s == "DXT1") return 71;
    if (s == "DXT2" || s == "DXT3") return 74;
    if (s == "DXT4" || s == "DXT5") return 77;
    if (s == "ATI1" || s == "BC4U") return 80;
    if (s == "BC4S") return 81;
    if (s == "ATI2" || s == "BC5U") return 83;
    if (s == "BC5S") return 84;
    if (s == "RGB32") return 28;
    if (s == "RGB16") return 49;
    if (s == "RGB8") return 61;
    return 0;
}

Family familyFromDds(const std::string& s) { return familyFromDxgi(dxgiFromDds(s)); }

bool isBlockCompressed(Family f) { return f >= Family::BC1 && f <= Family::BC7; }

uint32_t unitBytes(Family f) {
    switch (f) {
        case Family::BC1: case Family::BC4: return 8;
        case Family::BC2: case Family::BC3: case Family::BC5: case Family::BC6H: case Family::BC7: return 16;
        case Family::R8: return 1;
        case Family::RG8: return 2;
        case Family::RGBA8: case Family::BGRA8: return 4;
        default: return 0;
    }
}

MipLayout mipLayout(Family f, uint32_t width, uint32_t height, uint32_t mip) {
    MipLayout l;
    l.width = (width >> mip) ? (width >> mip) : 1;
    l.height = (height >> mip) ? (height >> mip) : 1;
    const uint32_t u = unitBytes(f);
    if (isBlockCompressed(f)) {
        l.rowBytes = ((l.width + 3) / 4) * u;
        l.rows = (l.height + 3) / 4;
    } else {
        l.rowBytes = l.width * u;
        l.rows = l.height;
    }
    return l;
}

static uint64_t alignUp(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }

uint64_t layoutFootprints(Family f, uint32_t width, uint32_t height, uint32_t firstMip, uint32_t count, uint64_t base,
                          std::vector<Footprint>& out) {
    out.clear();
    uint64_t p = alignUp(base, kPlacementAlign);
    for (uint32_t i = 0; i < count; ++i) {
        Footprint fp;
        fp.layout = mipLayout(f, width, height, firstMip + i);
        fp.rowPitch = uint32_t(alignUp(fp.layout.rowBytes, kPitchAlign));
        fp.offset = p;
        p = alignUp(p + fp.bytes(), kPlacementAlign);
        out.push_back(fp);
    }
    if (out.empty()) return 0;
    return out.back().offset + out.back().bytes() - base;
}

void packedToFootprint(const uint8_t* packed, const MipLayout& l, uint8_t* dst, uint32_t rowPitch) {
    for (uint32_t r = 0; r < l.rows; ++r) std::memcpy(dst + uint64_t(r) * rowPitch, packed + uint64_t(r) * l.rowBytes, l.rowBytes);
}

void footprintToPacked(const uint8_t* src, uint32_t rowPitch, const MipLayout& l, uint8_t* packed) {
    for (uint32_t r = 0; r < l.rows; ++r) std::memcpy(packed + uint64_t(r) * l.rowBytes, src + uint64_t(r) * rowPitch, l.rowBytes);
}

uint64_t mipPrint(Family f, const MipLayout& l, const uint8_t* data, uint32_t rowPitch) {
    const uint32_t shape[4] = {uint32_t(f), l.width, l.height, 0x51505254u};
    const uint32_t seedA = hashlittle(shape, sizeof shape, 0x9E3779B9u);
    const uint32_t seedB = hashlittle(shape, sizeof shape, 0x7F4A7C15u);
    uint8_t buf[kSampleBytes];
    size_t n = 0;
    if (l.size() <= kSampleBytes) {
        for (uint32_t r = 0; r < l.rows; ++r) {
            std::memcpy(buf + n, data + uint64_t(r) * rowPitch, l.rowBytes);
            n += l.rowBytes;
        }
    } else {
        // kSampleSegments segments from evenly spaced rows; a segment wider than a row continues on the
        // following rows (only for very narrow mips, where the sample then covers consecutive rows).
        const uint32_t unit = unitBytes(f);
        for (uint32_t s = 0; s < kSampleSegments; ++s) {
            uint32_t row = l.rows > 1 ? uint32_t(uint64_t(s) * (l.rows - 1) / (kSampleSegments - 1)) : 0;
            uint32_t col = l.rowBytes > kSegmentBytes ? (l.rowBytes - kSegmentBytes) / 2 / unit * unit : 0;
            uint32_t need = kSegmentBytes;
            // keep the whole segment inside the mip
            uint64_t linear = uint64_t(row) * l.rowBytes + col;
            if (linear + need > l.size()) linear = l.size() - need;
            while (need) {
                uint32_t r = uint32_t(linear / l.rowBytes), c = uint32_t(linear % l.rowBytes);
                uint32_t take = l.rowBytes - c < need ? l.rowBytes - c : need;
                std::memcpy(buf + n, data + uint64_t(r) * rowPitch + c, take);
                n += take;
                need -= take;
                linear += take;
            }
        }
    }
    uint64_t h = (uint64_t(hashlittle(buf, n, seedA)) << 32) | hashlittle(buf, n, seedB);
    return h ? h : 1;
}

bool computePrints(const uint8_t* dds, size_t size, TexturePrints& out, std::string* error) {
    out = TexturePrints();
    DdsInfo info;
    if (!parseDdsHeader(dds, size, info, error)) return false;
    out.dxgiFormat = dxgiFromDds(info.format);
    out.family = familyFromDxgi(out.dxgiFormat);
    if (out.family == Family::Unknown) {
        if (error) *error = "unsupported format " + info.format;
        return false;
    }
    out.width = info.width;
    out.height = info.height;
    out.mipCount = info.mipCount;
    out.headerSize = info.headerSize;
    uint64_t p = info.headerSize;
    for (uint32_t m = 0; m < info.mipCount; ++m) {
        MipLayout l = mipLayout(out.family, info.width, info.height, m);
        if (l.size() != info.mipRawSize[m] || p + l.size() > size) {
            if (error) *error = "mip " + std::to_string(m) + " outside the DDS";
            return false;
        }
        out.mipOffset.push_back(p - info.headerSize);
        out.mips.push_back(mipPrintPacked(out.family, l, dds + p));
        p += l.size();
    }
    return true;
}

bool decodeStored(const uint8_t* stored, size_t storedSize, std::vector<uint8_t>& decoded, std::string* error) {
    DdsInfo info;
    if (!parseDdsHeader(stored, storedSize, info, error)) return false;
    size_t raw = info.headerSize;
    for (uint32_t s : info.mipRawSize) raw += s;
    return validatePerMipDds(stored, storedSize, raw, error, &decoded);
}

bool computePrintsStored(const uint8_t* stored, size_t storedSize, TexturePrints& out, std::string* error,
                         std::vector<uint8_t>* decoded) {
    std::vector<uint8_t> tmp;
    std::vector<uint8_t>& d = decoded ? *decoded : tmp;
    if (!decodeStored(stored, storedSize, d, error)) return false;
    return computePrints(d.data(), d.size(), out, error);
}

}  // namespace qs::gpu
