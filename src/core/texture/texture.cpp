// QuattroStagioni - texture codec: plain DDS <-> RGBA8 mip chain, surface encode/decode, mip rebuild.
//
// Block work lives in bc.cpp (rgbcx + own BC1 punch-through + the fitBias rate/distortion pass), the
// generator-facing fit loop in fit.cpp. Mip generation filters colour in linear light with integer
// lookup tables (deterministic) and weights colour by alpha, because BC1 punch-through texels decode
// as black and would otherwise darken the leaf edges of every smaller mip.
#include "texture.h"

#include <algorithm>
#include <cstring>

#include "bc.h"
#include "dmath.h"

namespace qs::tex {
namespace {

inline uint32_t rd32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
void setErr(std::string* e, const std::string& m) { if (e) *e = m; }

// sRGB <-> linear in 20-bit fixed point. thr[i] = linear value halfway (in sRGB terms) between i and i+1.
struct SrgbLut {
    uint32_t lin[256];
    uint32_t thr[255];
    SrgbLut() {
        for (int i = 0; i < 256; ++i) lin[i] = uint32_t(dm::srgbToLinear(i / 255.0) * 1048576.0 + 0.5);
        for (int i = 0; i < 255; ++i) thr[i] = uint32_t(dm::srgbToLinear((i + 0.5) / 255.0) * 1048576.0 + 0.5);
    }
    uint8_t toSrgb(uint32_t l) const {
        int lo = 0, hi = 255;  // first i with l < thr[i], i.e. upper_bound
        while (lo < hi) {
            int mid = (lo + hi) / 2;
            if (l < thr[mid]) hi = mid; else lo = mid + 1;
        }
        return uint8_t(lo);
    }
};
const SrgbLut& srgbLut() {
    static const SrgbLut lut;
    return lut;
}

}  // namespace

// ---------------------------------------------------------------------------------------------------
Format formatFromDds(const DdsInfo& info) {
    const std::string& f = info.format;
    if (f == "DXT1" || f == "DXGI_71" || f == "DXGI_72") return Format::BC1;
    if (f == "DXT5" || f == "DXT4" || f == "DXGI_77" || f == "DXGI_78") return Format::BC3;
    if (f == "ATI1" || f == "BC4U" || f == "DXGI_80") return Format::BC4;
    if (f == "ATI2" || f == "BC5U" || f == "DXGI_83") return Format::BC5;
    if (f == "DXGI_28" || f == "DXGI_29") return Format::RGBA8;
    if (f == "DXGI_87" || f == "DXGI_91" || f == "RGB32") return Format::BGRA8;  // RGB32: refined by masks
    if (f == "DXGI_61" || f == "RGB8") return Format::R8;
    return Format::Unknown;
}

const char* toString(Format f) {
    switch (f) {
        case Format::BC1: return "BC1";
        case Format::BC3: return "BC3";
        case Format::BC4: return "BC4";
        case Format::BC5: return "BC5";
        case Format::RGBA8: return "RGBA8";
        case Format::BGRA8: return "BGRA8";
        case Format::R8: return "R8";
        default: return "Unknown";
    }
}

uint32_t blockBytes(Format f) {
    switch (f) {
        case Format::BC1: case Format::BC4: return 8;
        case Format::BC3: case Format::BC5: return 16;
        default: return 0;
    }
}

size_t surfaceSize(Format f, uint32_t w, uint32_t h) {
    if (uint32_t bb = blockBytes(f)) return size_t((w + 3) / 4) * ((h + 3) / 4) * bb;
    switch (f) {
        case Format::RGBA8: case Format::BGRA8: return size_t(w) * h * 4;
        case Format::R8: return size_t(w) * h;
        default: return 0;
    }
}

bool decodeSurface(Format f, const uint8_t* data, size_t size, uint32_t w, uint32_t h, Image& out, std::string* error) {
    const size_t need = surfaceSize(f, w, h);
    if (!need) { setErr(error, std::string("unsupported format ") + toString(f)); return false; }
    if (size < need) { setErr(error, "surface data truncated"); return false; }
    out.width = w;
    out.height = h;
    out.rgba.assign(size_t(w) * h * 4, 255);
    if (f == Format::RGBA8 || f == Format::BGRA8 || f == Format::R8) {
        for (size_t i = 0, n = size_t(w) * h; i < n; ++i) {
            uint8_t* o = &out.rgba[i * 4];
            if (f == Format::R8) { o[0] = data[i]; o[1] = o[2] = 0; o[3] = 255; continue; }
            const uint8_t* p = data + i * 4;
            if (f == Format::RGBA8) { std::memcpy(o, p, 4); }
            else { o[0] = p[2]; o[1] = p[1]; o[2] = p[0]; o[3] = p[3]; }
        }
        return true;
    }
    bc::UnitSpec us[2];
    const int U = bc::unitsOf(f, us);
    const uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    uint8_t px[64];
    for (uint32_t by = 0; by < bh; ++by)
        for (uint32_t bx = 0; bx < bw; ++bx) {
            const uint8_t* blk = data + (size_t(by) * bw + bx) * 8 * U;
            std::memset(px, 0, 64);
            for (int i = 0; i < 16; ++i) px[4 * i + 3] = 255;
            for (int u = 0; u < U; ++u) {
                const uint8_t* unit = blk + 8 * u;
                if (us[u].kind == bc::UnitKind::Channel) {
                    bc::decodeChannel(unit, px + us[u].channel, 4);
                } else {
                    uint8_t c[64];
                    bc::decodeColor(unit, us[u].kind == bc::UnitKind::ColorBC1, c);
                    for (int i = 0; i < 16; ++i) {
                        std::memcpy(px + 4 * i, c + 4 * i, 3);
                        if (us[u].kind == bc::UnitKind::ColorBC1) px[4 * i + 3] = c[4 * i + 3];
                    }
                }
            }
            for (uint32_t y = 0; y < 4; ++y)
                for (uint32_t x = 0; x < 4; ++x) {
                    uint32_t sx = bx * 4 + x, sy = by * 4 + y;
                    if (sx < w && sy < h) std::memcpy(&out.rgba[(size_t(sy) * w + sx) * 4], px + 4 * (y * 4 + x), 4);
                }
        }
    return true;
}

bool encodeSurface(Format f, const Image& img, const EncodeOptions& opt, std::vector<uint8_t>& out,
                   const uint8_t* reference, size_t referenceSize, std::string* error) {
    if (img.width == 0 || img.height == 0 || img.rgba.size() < size_t(img.width) * img.height * 4) {
        setErr(error, "bad image");
        return false;
    }
    const size_t n = size_t(img.width) * img.height;
    switch (f) {
        case Format::RGBA8:
            out.assign(img.rgba.begin(), img.rgba.begin() + long(n * 4));
            return true;
        case Format::BGRA8:
            out.resize(n * 4);
            for (size_t i = 0; i < n; ++i) {
                const uint8_t* p = &img.rgba[i * 4];
                out[i * 4] = p[2]; out[i * 4 + 1] = p[1]; out[i * 4 + 2] = p[0]; out[i * 4 + 3] = p[3];
            }
            return true;
        case Format::R8:
            out.resize(n);
            for (size_t i = 0; i < n; ++i) out[i] = img.rgba[i * 4];
            return true;
        case Format::BC1: case Format::BC3: case Format::BC4: case Format::BC5:
            bc::encodeBlocks(f, img, opt, reference, referenceSize, out);
            return true;
        default:
            setErr(error, std::string("cannot encode format ") + toString(f));
            return false;
    }
}

bool encodeMipRef(const Texture& t, size_t mip, const EncodeOptions& opt, const uint8_t* reference, size_t referenceSize,
                  std::vector<uint8_t>& out, std::string* error) {
    if (mip >= t.mips.size()) { setErr(error, "mip out of range"); return false; }
    return encodeSurface(t.format, t.mips[mip], opt, out, reference, referenceSize, error);
}

bool encodeMip(const Texture& t, size_t mip, const EncodeOptions& opt, std::vector<uint8_t>& out, std::string* error) {
    return encodeMipRef(t, mip, opt, nullptr, 0, out, error);
}

bool decodeDds(const uint8_t* dds, size_t size, Texture& out, std::string* error) {
    out = Texture();
    DdsInfo info;
    if (!parseDdsHeader(dds, size, info, error)) return false;
    Format f = formatFromDds(info);
    if (info.format == "RGB32") {
        const uint32_t rmask = rd32(dds + 92);
        f = rmask == 0x000000FFu ? Format::RGBA8 : (rmask == 0x00FF0000u ? Format::BGRA8 : Format::Unknown);
    }
    if (f == Format::Unknown) { setErr(error, "unsupported DDS format " + info.format); return false; }
    out.format = f;
    out.header.assign(dds, dds + info.headerSize);
    size_t p = info.headerSize;
    for (uint32_t i = 0; i < info.mipCount; ++i) {
        const uint32_t w = mipDim(info.width, i), h = mipDim(info.height, i);
        const size_t s = surfaceSize(f, w, h);
        if (p + s > size) { setErr(error, "DDS truncated at mip " + std::to_string(i)); return false; }
        Image img;
        if (!decodeSurface(f, dds + p, s, w, h, img, error)) return false;
        out.mips.push_back(std::move(img));
        p += s;
    }
    return true;
}

bool encodeDds(const Texture& t, const EncodeOptions& opt, std::vector<uint8_t>& out, std::string* error) {
    out = t.header;
    std::vector<uint8_t> m;
    for (size_t i = 0; i < t.mips.size(); ++i) {
        if (!encodeMip(t, i, opt, m, error)) return false;
        out.insert(out.end(), m.begin(), m.end());
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------
Image downsample(const Image& src, bool srgb, bool alphaWeighted) {
    const SrgbLut& L = srgbLut();
    Image dst;
    dst.width = std::max(1u, src.width / 2);
    dst.height = std::max(1u, src.height / 2);
    dst.rgba.resize(size_t(dst.width) * dst.height * 4);
    for (uint32_t y = 0; y < dst.height; ++y)
        for (uint32_t x = 0; x < dst.width; ++x) {
            const uint8_t* s[4];
            for (int k = 0; k < 4; ++k) {
                uint32_t sx = std::min(src.width - 1, x * 2 + (k & 1)), sy = std::min(src.height - 1, y * 2 + (k >> 1));
                s[k] = &src.rgba[(size_t(sy) * src.width + sx) * 4];
            }
            uint64_t wsum = 0, wt[4];
            for (int k = 0; k < 4; ++k) { wt[k] = alphaWeighted ? s[k][3] : 1; wsum += wt[k]; }
            if (!wsum) { for (int k = 0; k < 4; ++k) wt[k] = 1; wsum = 4; }
            uint8_t* o = &dst.rgba[(size_t(y) * dst.width + x) * 4];
            for (int c = 0; c < 3; ++c) {
                uint64_t acc = 0;
                for (int k = 0; k < 4; ++k) acc += wt[k] * (srgb ? L.lin[s[k][c]] : uint64_t(s[k][c]) << 12);
                uint64_t v = (acc + wsum / 2) / wsum;
                o[c] = srgb ? L.toSrgb(uint32_t(v)) : uint8_t(std::min<uint64_t>(255, (v + 2048) >> 12));
            }
            o[3] = uint8_t((uint32_t(s[0][3]) + s[1][3] + s[2][3] + s[3][3] + 2) / 4);
        }
    return dst;
}

Image downsample(const Image& src, bool srgb) { return downsample(src, srgb, true); }

double alphaCoverage(const Image& img, uint8_t ref) {
    const size_t n = size_t(img.width) * img.height;
    if (!n) return 0.0;
    size_t c = 0;
    for (size_t i = 0; i < n; ++i) c += img.rgba[i * 4 + 3] >= ref;
    return double(c) / double(n);
}

void matchAlphaCoverage(Image& img, double target, uint8_t ref) {
    const size_t n = size_t(img.width) * img.height;
    if (!n) return;
    uint64_t hist[256] = {0};
    for (size_t i = 0; i < n; ++i) ++hist[img.rgba[i * 4 + 3]];
    auto scaled = [](uint32_t a, uint32_t s) { return std::min<uint32_t>(255, (a * s + 128) >> 8); };
    auto count = [&](uint32_t s) {
        uint64_t c = 0;
        for (uint32_t a = 0; a < 256; ++a) if (scaled(a, s) >= ref) c += hist[a];
        return c;
    };
    // Integer target in texels; smallest scale reaching it, then the closer of it and the one below.
    const uint64_t want = uint64_t(target * double(n) + 0.5);
    uint32_t lo = 0, hi = 64 * 256;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (count(mid) >= want) hi = mid; else lo = mid + 1;
    }
    uint32_t s = lo;
    if (s > 0) {
        uint64_t c1 = count(s), c0 = count(s - 1);
        uint64_t d1 = c1 > want ? c1 - want : want - c1, d0 = c0 > want ? c0 - want : want - c0;
        if (d0 < d1) s = s - 1;
    }
    if (s == 256) return;
    for (size_t i = 0; i < n; ++i) img.rgba[i * 4 + 3] = uint8_t(scaled(img.rgba[i * 4 + 3], s));
}

void rebuildMips(Texture& t, const MipOptions& opt) {
    if (t.mips.empty()) return;
    const bool data = t.format == Format::BC4 || t.format == Format::BC5;
    const bool srgb = opt.srgb && !data;
    const Image& m0 = t.mips[0];
    bool cov = false;
    if (!data && opt.coverage != 0) {
        bool lo = false, hi = false;
        for (size_t i = 0, n = size_t(m0.width) * m0.height; i < n && !(lo && hi); ++i) {
            uint8_t a = m0.rgba[i * 4 + 3];
            lo |= a < opt.alphaRef;
            hi |= a >= opt.alphaRef;
        }
        cov = opt.coverage == 1 || (lo && hi);
    }
    const double target = cov ? alphaCoverage(m0, opt.alphaRef) : 0.0;
    Image chain = m0;  // unscaled filtered chain; coverage scaling is applied per output mip only
    for (size_t i = 1; i < t.mips.size(); ++i) {
        chain = downsample(chain, srgb, !data);
        Image out = chain;
        if (cov) matchAlphaCoverage(out, target, opt.alphaRef);
        t.mips[i] = std::move(out);
    }
}

void rebuildMips(Texture& t, bool srgb) {
    MipOptions o;
    o.srgb = srgb;
    rebuildMips(t, o);
}

}  // namespace qs::tex
