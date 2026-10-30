// QuattroStagioni - synthetic textures and season packs for the GPU swap tests (tests/test_gpu.cpp) and
// the D3D12 demo (tools/gpu_demo). No game data: block bytes come from a seeded generator.
//
// A synthetic texture is a Crimson Desert "per-mip" stored DDS (pamt compression 1): 128-byte header
// with the stored sizes of mips 0..3 at offset 0x20, then the mips. Mips are stored raw (stored size ==
// raw size), so an original and its replacement always have the same stored size, as the packs require.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "../src/core/pack/qsp_io.h"

namespace qs::gpusynth {

inline void put32(std::vector<uint8_t>& v, size_t at, uint32_t x) {
    v[at] = uint8_t(x);
    v[at + 1] = uint8_t(x >> 8);
    v[at + 2] = uint8_t(x >> 16);
    v[at + 3] = uint8_t(x >> 24);
}

// fourcc: "DXT1", "DXT5", "ATI1", "ATI2"
inline uint32_t blockBytesOf(const char* fourcc) {
    return (std::strcmp(fourcc, "DXT1") == 0 || std::strcmp(fourcc, "ATI1") == 0) ? 8 : 16;
}

inline uint64_t mipBytes(const char* fourcc, uint32_t w, uint32_t h, uint32_t m) {
    uint32_t mw = (w >> m) ? (w >> m) : 1, mh = (h >> m) ? (h >> m) : 1;
    return uint64_t((mw + 3) / 4) * ((mh + 3) / 4) * blockBytesOf(fourcc);
}

// Stored per-mip DDS with deterministic pseudo-random blocks. `seed` selects the content; two calls with
// the same arguments return the same bytes.
inline std::vector<uint8_t> makeStoredDds(const char* fourcc, uint32_t w, uint32_t h, uint32_t mips, uint32_t seed) {
    std::vector<uint8_t> d(128, 0);
    std::memcpy(d.data(), "DDS ", 4);
    put32(d, 4, 124);
    put32(d, 8, 0x000A1007);  // CAPS | HEIGHT | WIDTH | PIXELFORMAT | MIPMAPCOUNT | LINEARSIZE
    put32(d, 12, h);
    put32(d, 16, w);
    put32(d, 20, uint32_t(mipBytes(fourcc, w, h, 0)));
    put32(d, 28, mips);
    for (uint32_t m = 0; m < 4 && m < mips; ++m) put32(d, 0x20 + 4 * m, uint32_t(mipBytes(fourcc, w, h, m)));
    put32(d, 76, 32);
    put32(d, 80, 4);  // DDPF_FOURCC
    std::memcpy(d.data() + 84, fourcc, 4);
    put32(d, 108, 0x401008);  // COMPLEX | TEXTURE | MIPMAP
    uint32_t x = seed * 2654435761u + 0x1234567u;
    for (uint32_t m = 0; m < mips; ++m) {
        uint64_t n = mipBytes(fourcc, w, h, m);
        for (uint64_t i = 0; i < n; ++i) {
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            d.push_back(uint8_t(x >> 7));
        }
    }
    return d;
}

// Replacement for a stage: same header, mips >= keepFrom regenerated with another seed, mips below
// changed. `unchangedMips` lists mips copied from the original (a stage that leaves them alone).
inline std::vector<uint8_t> makeStageVariant(const std::vector<uint8_t>& original, const char* fourcc, uint32_t w,
                                             uint32_t h, uint32_t mips, uint32_t seed, uint32_t unchangedFromMip = 99) {
    std::vector<uint8_t> v = makeStoredDds(fourcc, w, h, mips, seed);
    size_t p = 128;
    for (uint32_t m = 0; m < mips; ++m) {
        size_t n = size_t(mipBytes(fourcc, w, h, m));
        if (m >= unchangedFromMip) std::memcpy(v.data() + p, original.data() + p, n);
        p += n;
    }
    return v;
}

struct SynthTexture {
    const char* fourcc;
    uint32_t w, h, mips;
    uint32_t seed;
    std::string path;     // archive path (informational)
    uint64_t offset = 0;  // inside the synthetic paz
    std::vector<uint8_t> original;
};

// Lays the originals into one synthetic paz (4 KiB aligned entries). Returns the paz bytes.
inline std::vector<uint8_t> buildPaz(std::vector<SynthTexture>& tex) {
    std::vector<uint8_t> paz;
    for (SynthTexture& t : tex) {
        t.original = makeStoredDds(t.fourcc, t.w, t.h, t.mips, t.seed);
        paz.resize((paz.size() + 4095) / 4096 * 4096, 0);
        t.offset = paz.size();
        paz.insert(paz.end(), t.original.begin(), t.original.end());
    }
    paz.resize((paz.size() + 4095) / 4096 * 4096, 0);
    return paz;
}

// Writes a pack for `stage` with the given replacements (same order as `tex`, empty = texture unchanged).
inline bool writePack(const std::string& path, const std::string& stage, const std::string& group, uint32_t pazIndex,
                      uint64_t pazSize, const std::vector<SynthTexture>& tex,
                      const std::vector<std::vector<uint8_t>>& repl, std::string* error) {
    pack::PackWriter w;
    pack::Header hd;
    hd.stageId = stage;
    hd.gameBuild = "synthetic";
    hd.generator = "gpu_synth";
    if (!w.open(path, hd, error)) return false;
    w.setPazSize(group, pazIndex, pazSize);
    for (size_t i = 0; i < tex.size(); ++i) {
        if (i >= repl.size() || repl[i].empty()) continue;
        pack::Record r;
        r.group = group;
        r.path = tex[i].path;
        r.pazIndex = pazIndex;
        r.offset = tex[i].offset;
        r.storedSize = uint32_t(tex[i].original.size());
        r.size = r.storedSize;
        r.flags = 1;
        if (!w.add(r, tex[i].original.data(), repl[i].data(), error)) return false;
    }
    return w.finish(error);
}

}  // namespace qs::gpusynth
