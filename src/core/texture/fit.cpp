// QuattroStagioni - fitting a re-encoded mip into the original stored size, and the recolour fallback.
//
// The game's indexes record the stored (LZ4) size of mips 0..3 and we must never change them, so a
// replacement mip has to compress to <= the original size; lz4CompressExact() then pads it to exactly
// that size. fitMip() raises EncodeOptions::fitBias until LZ4-HC gets below the target. Because the
// originals were most likely written by a fast LZ4 compressor, HC alone usually buys some slack.
#include <algorithm>
#include <cstring>
#include <map>

#include "../lz4_exact.h"
#include "bc.h"
#include "lz4hc.h"
#include "texture.h"

namespace qs::tex {

size_t lz4BestSize(const uint8_t* data, size_t size) {
    if (!size) return 0;
    std::vector<char> tmp(static_cast<size_t>(LZ4_compressBound(int(size))));
    int n = LZ4_compress_HC(reinterpret_cast<const char*>(data), tmp.data(), int(size), int(tmp.size()), LZ4HC_CLEVEL_MAX);
    return n > 0 ? size_t(n) : size + size / 255 + 16;
}

bool recolorSurface(Format f, const uint8_t* blocks, size_t size, PaletteFn fn, void* user, std::vector<uint8_t>& out,
                    std::string* error) {
    if (f != Format::BC1 && f != Format::BC3) {
        if (error) *error = std::string("recolour needs BC1/BC3, got ") + toString(f);
        return false;
    }
    const size_t bb = blockBytes(f), coff = f == Format::BC3 ? 8 : 0;
    if (!size || size % bb) { if (error) *error = "bad block data size"; return false; }
    const size_t n = size / bb;
    // Unique endpoint colours, sorted (deterministic order for the palette image).
    std::vector<uint16_t> uniq;
    uniq.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        uniq.push_back(bc::rd16(blocks + i * bb + coff));
        uniq.push_back(bc::rd16(blocks + i * bb + coff + 2));
    }
    std::sort(uniq.begin(), uniq.end());
    uniq.erase(std::unique(uniq.begin(), uniq.end()), uniq.end());
    Image pal;
    pal.width = uint32_t(uniq.size());
    pal.height = 1;
    pal.rgba.resize(uniq.size() * 4);
    for (size_t i = 0; i < uniq.size(); ++i) {
        int r, g, b;
        bc::unpack565(uniq[i], r, g, b);
        pal.rgba[i * 4] = uint8_t(r); pal.rgba[i * 4 + 1] = uint8_t(g); pal.rgba[i * 4 + 2] = uint8_t(b);
        pal.rgba[i * 4 + 3] = 255;
    }
    if (fn) fn(user, pal);
    if (pal.rgba.size() != uniq.size() * 4) { if (error) *error = "palette function changed the image size"; return false; }
    std::vector<uint16_t> mapped(uniq.size());
    for (size_t i = 0; i < uniq.size(); ++i) {
        const uint8_t* p = &pal.rgba[i * 4];
        int r = (p[0] * 31 + 127) / 255, g = (p[1] * 63 + 127) / 255, b = (p[2] * 31 + 127) / 255;
        mapped[i] = uint16_t((r << 11) | (g << 5) | b);
    }
    auto map = [&](uint16_t c) { return mapped[size_t(std::lower_bound(uniq.begin(), uniq.end(), c) - uniq.begin())]; };
    out.assign(blocks, blocks + size);
    for (size_t i = 0; i < n; ++i) {
        uint8_t* u = out.data() + i * bb + coff;
        const uint16_t c0 = bc::rd16(u), c1 = bc::rd16(u + 2);
        uint16_t n0 = map(c0), n1 = map(c1);
        uint32_t sels = uint32_t(u[4]) | (uint32_t(u[5]) << 8) | (uint32_t(u[6]) << 16) | (uint32_t(u[7]) << 24);
        if (f == Format::BC1) {
            const bool four = c0 > c1;
            if (four && n0 < n1) {
                std::swap(n0, n1);
                sels ^= 0x55555555u;  // 0<->1, 2<->3
            } else if (four && n0 == n1) {
                sels = 0;  // all four colours equal: selector 0 keeps the block opaque
            } else if (!four && n0 > n1) {
                std::swap(n0, n1);
                uint32_t s2 = 0;
                for (int k = 0; k < 16; ++k) {
                    uint32_t s = (sels >> (2 * k)) & 3;
                    if (s < 2) s ^= 1;
                    s2 |= s << (2 * k);
                }
                sels = s2;
            }
        }
        u[0] = uint8_t(n0); u[1] = uint8_t(n0 >> 8); u[2] = uint8_t(n1); u[3] = uint8_t(n1 >> 8);
        for (int k = 0; k < 4; ++k) u[4 + k] = uint8_t(sels >> (8 * k));
    }
    return true;
}

FitResult fitMip(const Texture& t, size_t mip, size_t targetStored, const uint8_t* original, size_t originalSize,
                 const EncodeOptions& base, const RecolorFallback* recolor) {
    FitResult r;
    if (mip >= t.mips.size()) { r.error = "mip out of range"; return r; }
    const Image& img = t.mips[mip];
    const size_t raw = surfaceSize(t.format, img.width, img.height);
    if (!raw) { r.error = std::string("unsupported format ") + toString(t.format); return r; }
    if (original && originalSize < raw) { r.error = "original mip data too small"; return r; }
    if (targetStored > raw) { r.error = "stored size larger than raw size"; return r; }
    if (targetStored == 0) { r.error = "target size 0"; return r; }

    auto encodeAt = [&](int fb, std::vector<uint8_t>& blocks) {
        EncodeOptions o = base;
        o.fitBias = fb;
        ++r.trials;
        return encodeSurface(t.format, img, o, blocks, original, original ? originalSize : 0, &r.error);
    };

    if (targetStored == raw) {
        if (!encodeAt(base.fitBias, r.blocks)) return r;
        r.raw = true;
        r.stored = r.blocks;
        r.fitBias = base.fitBias;
        r.hcSize = raw;
        r.ok = true;
        return r;
    }

    const int lo0 = std::max(0, std::min(10, base.fitBias));
    std::vector<uint8_t> cur;
    bool found = false;
    if (!encodeAt(lo0, cur)) return r;
    size_t hc = lz4BestSize(cur.data(), cur.size());
    if (hc <= targetStored) {
        found = true;
        r.blocks.swap(cur);
        r.fitBias = lo0;
        r.hcSize = hc;
    } else if (lo0 < 10 && blockBytes(t.format)) {
        // First guess from the size gap (small gaps need little bias), then bisect between the
        // largest level known not to fit and the smallest known to fit.
        const double ratio = double(targetStored) / double(hc);
        int guess = ratio > 0.97 ? 2 : (ratio > 0.9 ? 5 : 10);
        guess = std::max(guess, lo0 + 1);
        int lo = lo0 + 1, hi = 11;  // levels in [lo, hi) are untested; hi = smallest fitting level
        for (int probe = guess; lo < hi;) {
            if (!encodeAt(probe, cur)) return r;
            size_t h = lz4BestSize(cur.data(), cur.size());
            if (h <= targetStored) {
                found = true;
                hi = probe;
                r.blocks.swap(cur);
                r.fitBias = probe;
                r.hcSize = h;
            } else {
                lo = probe + 1;
                hc = std::min(hc, h);
            }
            if (lo >= hi) break;
            probe = found ? (lo + hi - 1) / 2 : std::min(10, std::max(lo, hi == 11 ? 10 : (lo + hi) / 2));
            if (!found && probe < lo) break;
        }
    }
    if (!found && recolor && recolor->fn && original) {
        if (recolorSurface(t.format, original, raw, recolor->fn, recolor->user, cur, &r.error)) {
            size_t h = lz4BestSize(cur.data(), cur.size());
            if (h <= targetStored) { found = true; r.blocks.swap(cur); r.fitBias = -1; r.hcSize = h; }
            else hc = std::min(hc, h);
        }
    }
    if (!found) {
        r.error = "mip " + std::to_string(mip) + " does not fit: best LZ4 " + std::to_string(hc) + " > target " +
                  std::to_string(targetStored);
        return r;
    }
    Lz4ExactResult ex = lz4CompressExact(r.blocks.data(), r.blocks.size(), targetStored);
    if (!ex.ok) { r.error = "lz4 exact: " + ex.error; return r; }
    r.stored.swap(ex.block);
    r.ok = true;
    return r;
}

}  // namespace qs::tex
