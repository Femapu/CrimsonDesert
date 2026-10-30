// QuattroStagioni - unit tests for the texture codec (synthetic images only, no game data).
// Usage: test_texture [data_dir]   (data_dir unused: everything is generated)
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../src/core/lookup3.h"
#include "../src/core/lz4_exact.h"
#include "../src/core/texture/dmath.h"
#include "../src/core/texture/synth.h"
#include "../src/core/texture/texture.h"
#include "lz4.h"

using namespace qs::tex;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond)                                                                         \
    do {                                                                                    \
        if (cond) ++g_pass;                                                                 \
        else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); }       \
    } while (0)

// PSNR over visible texels (alpha >= minAlpha in the reference).
static double psnr(const Image& ref, const Image& b, int minAlpha) {
    double se = 0;
    size_t n = 0;
    for (size_t i = 0; i < ref.rgba.size(); i += 4) {
        if (ref.rgba[i + 3] < minAlpha) continue;
        for (int k = 0; k < 3; ++k) { double d = double(ref.rgba[i + k]) - b.rgba[i + k]; se += d * d; }
        n += 3;
    }
    return n ? 10 * std::log10(255.0 * 255.0 / (se / double(n) + 1e-9)) : 99;
}

static Image roundTrip(Format f, const Image& img, int fitBias = 0, std::vector<uint8_t>* blocksOut = nullptr) {
    EncodeOptions o;
    o.fitBias = fitBias;
    std::vector<uint8_t> blocks;
    encodeSurface(f, img, o, blocks);
    Image d;
    decodeSurface(f, blocks.data(), blocks.size(), img.width, img.height, d);
    if (blocksOut) blocksOut->swap(blocks);
    return d;
}

static uint32_t hashOf(const std::vector<uint8_t>& v) { return qs::hashlittle(v.data(), v.size(), 0); }

// ---------------------------------------------------------------------------------------------
static void testDmath() {
    double maxErr = 0;
    for (double x = 0.001; x < 30; x *= 1.37) {
        maxErr = std::max(maxErr, std::fabs(qs::dm::cbrt(x) - std::cbrt(x)) / std::cbrt(x));
        maxErr = std::max(maxErr, std::fabs(qs::dm::log(x) - std::log(x)) / (1 + std::fabs(std::log(x))));
        maxErr = std::max(maxErr, std::fabs(qs::dm::pow(x, 2.4) - std::pow(x, 2.4)) / std::pow(x, 2.4));
    }
    for (double a = -7; a < 7; a += 0.093) {
        double s, c;
        qs::dm::sincos(a, s, c);
        maxErr = std::max(maxErr, std::fabs(s - std::sin(a)));
        maxErr = std::max(maxErr, std::fabs(c - std::cos(a)));
        maxErr = std::max(maxErr, std::fabs(qs::dm::atan2(std::sin(a) * 3, std::cos(a) * 3) - std::atan2(std::sin(a), std::cos(a))));
    }
    std::printf("dmath max relative error %.3g\n", maxErr);
    CHECK(maxErr < 1e-9);
    CHECK(std::fabs(qs::dm::srgbToLinear(0.5) - 0.21404114) < 1e-7);
    CHECK(std::fabs(qs::dm::linearToSrgb(qs::dm::srgbToLinear(0.3)) - 0.3) < 1e-9);
}

// ---------------------------------------------------------------------------------------------
static void testRoundTrip() {
    using synth::Kind;
    Image terrain = synth::make(Kind::Terrain, 128, 3), leaves = synth::make(Kind::Leaves, 128, 3);
    // BC1 opaque
    Image d = roundTrip(Format::BC1, terrain);
    double p = psnr(terrain, d, 0);
    std::printf("BC1 terrain PSNR %.2f\n", p);
    CHECK(p > 38);
    for (size_t i = 3; i < d.rgba.size(); i += 4) if (d.rgba[i] != 255) { CHECK(!"opaque BC1 decoded with holes"); break; }
    // BC1 punch-through: alpha test preserved exactly
    d = roundTrip(Format::BC1, leaves);
    size_t mism = 0;
    for (size_t i = 3; i < d.rgba.size(); i += 4) mism += (leaves.rgba[i] >= 128) != (d.rgba[i] == 255);
    CHECK(mism == 0);
    p = psnr(leaves, d, 128);
    std::printf("BC1 leaves PSNR %.2f (visible texels)\n", p);
    CHECK(p > 34);
    // BC3
    d = roundTrip(Format::BC3, leaves);
    p = psnr(leaves, d, 1);
    double ae = 0;
    for (size_t i = 3; i < d.rgba.size(); i += 4) ae = std::max(ae, std::fabs(double(d.rgba[i]) - leaves.rgba[i]));
    std::printf("BC3 leaves PSNR %.2f, max alpha error %.0f\n", p, ae);
    CHECK(p > 34);
    CHECK(ae <= 4);
    // BC4 / BC5 channels
    Image g = synth::make(Kind::Climate, 64, 5);
    d = roundTrip(Format::BC4, g);
    double e4 = 0, e5 = 0;
    for (size_t i = 0; i < d.rgba.size(); i += 4) e4 = std::max(e4, std::fabs(double(d.rgba[i]) - g.rgba[i]));
    d = roundTrip(Format::BC5, g);
    for (size_t i = 0; i < d.rgba.size(); i += 4)
        for (int k = 0; k < 2; ++k) e5 = std::max(e5, std::fabs(double(d.rgba[i + k]) - g.rgba[i + k]));
    std::printf("BC4 max error %.0f, BC5 max error %.0f\n", e4, e5);
    CHECK(e4 <= 6);
    CHECK(e5 <= 8);
    // raw formats are lossless
    for (Format f : {Format::RGBA8, Format::BGRA8}) {
        d = roundTrip(f, leaves);
        CHECK(d.rgba == leaves.rgba);
    }
    // odd and tiny sizes
    for (uint32_t s : {1u, 2u, 3u, 5u, 13u}) {
        Image t = synth::make(Kind::Terrain, 64, 1);
        Image small;
        small.width = s; small.height = s + 1;
        small.rgba.assign(size_t(s) * (s + 1) * 4, 0);
        for (uint32_t y = 0; y < s + 1; ++y)
            for (uint32_t x = 0; x < s; ++x) std::memcpy(&small.rgba[(y * s + x) * 4], &t.rgba[(y * 64 + x) * 4], 4);
        std::vector<uint8_t> b;
        Image r = roundTrip(Format::BC3, small, 3, &b);
        CHECK(b.size() == surfaceSize(Format::BC3, s, s + 1));
        CHECK(r.width == s && r.height == s + 1);
        CHECK(psnr(small, r, 0) > 30);
    }
}

// ---------------------------------------------------------------------------------------------
static void testDeterminismAndReference() {
    Image leaves = synth::make(synth::Kind::Leaves, 128, 9);
    for (Format f : {Format::BC1, Format::BC3, Format::BC4, Format::BC5})
        for (int fb : {0, 5, 10}) {
            std::vector<uint8_t> a, b;
            EncodeOptions o;
            o.fitBias = fb;
            encodeSurface(f, leaves, o, a);
            encodeSurface(f, leaves, o, b);
            CHECK(a == b);
            CHECK(a.size() == surfaceSize(f, 128, 128));
        }
    // Golden hashes: the generator output is cross-checked by hash on every PC. If these change, the
    // encoder changed: bump them deliberately (and check the mingw build gives the same, see
    // docs/dev/texture_recipes.md).
    std::vector<uint8_t> h1, h3;
    EncodeOptions o;
    encodeSurface(Format::BC1, leaves, o, h1);
    o.fitBias = 6;
    encodeSurface(Format::BC3, leaves, o, h3);
    std::printf("golden: BC1 fb0 %08x, BC3 fb6 %08x\n", hashOf(h1), hashOf(h3));

    // Reference: unchanged blocks keep the original bytes exactly.
    for (Format f : {Format::BC1, Format::BC3}) {
        std::vector<uint8_t> orig;
        EncodeOptions lo;
        lo.level = 0;
        encodeSurface(f, leaves, lo, orig);
        Image dec;
        decodeSurface(f, orig.data(), orig.size(), 128, 128, dec);
        std::vector<uint8_t> again;
        encodeSurface(f, dec, EncodeOptions(), again, orig.data(), orig.size());
        CHECK(again == orig);
        // modify the top quarter: blocks below must stay identical
        Image mod = dec;
        for (uint32_t y = 0; y < 32; ++y)
            for (uint32_t x = 0; x < 128; ++x) mod.rgba[(y * 128 + x) * 4 + 1] = uint8_t(255 - mod.rgba[(y * 128 + x) * 4 + 1]);
        encodeSurface(f, mod, EncodeOptions(), again, orig.data(), orig.size());
        const size_t bb = blockBytes(f), firstUntouched = 8 * 32 * bb;  // rows of blocks 8..31
        CHECK(std::memcmp(again.data() + firstUntouched, orig.data() + firstUntouched, orig.size() - firstUntouched) == 0);
        CHECK(std::memcmp(again.data(), orig.data(), firstUntouched) != 0);
    }
}

// ---------------------------------------------------------------------------------------------
// Builds a plain DDS (legacy header) for a synthetic texture.
static std::vector<uint8_t> makeDds(Format f, const Image& img, uint32_t mips) {
    std::vector<uint8_t> h(128, 0);
    auto w32 = [&](size_t off, uint32_t v) { for (int i = 0; i < 4; ++i) h[off + i] = uint8_t(v >> (8 * i)); };
    std::memcpy(h.data(), "DDS ", 4);
    w32(4, 124);
    w32(8, 0x000A1007);
    w32(12, img.height);
    w32(16, img.width);
    w32(28, mips);
    w32(0x20, 1234);  // stored mip sizes live here in the game files: must survive verbatim
    w32(76, 32);
    w32(80, 4);
    std::memcpy(h.data() + 84, f == Format::BC1 ? "DXT1" : "DXT5", 4);
    Texture t;
    t.format = f;
    t.header = h;
    t.mips.push_back(img);
    for (uint32_t i = 1; i < mips; ++i) t.mips.push_back(Image());
    rebuildMips(t, true);
    std::vector<uint8_t> out;
    encodeDds(t, EncodeOptions(), out);
    return out;
}

static void testDds() {
    Image leaves = synth::make(synth::Kind::Leaves, 64, 2);
    for (Format f : {Format::BC1, Format::BC3}) {
        std::vector<uint8_t> dds = makeDds(f, leaves, 7);
        Texture t;
        std::string err;
        CHECK(decodeDds(dds.data(), dds.size(), t, &err));
        CHECK(t.format == f);
        CHECK(t.mips.size() == 7);
        CHECK(t.mips.size() == 7 && t.mips[6].width == 1 && t.mips[6].height == 1);
        std::vector<uint8_t> again;
        CHECK(encodeDds(t, EncodeOptions(), again, &err));
        CHECK(again.size() == dds.size());
        CHECK(std::memcmp(again.data(), dds.data(), 128) == 0);
        // decode -> encode of an already encoded texture is stable within a small error
        Texture t2;
        CHECK(decodeDds(again.data(), again.size(), t2, &err));
        CHECK(psnr(t.mips[0], t2.mips[0], f == Format::BC1 ? 128 : 1) > 40);
    }
    Texture t;
    std::string err;
    std::vector<uint8_t> junk(200, 7);
    CHECK(!decodeDds(junk.data(), junk.size(), t, &err));
    CHECK(!err.empty());
}

// ---------------------------------------------------------------------------------------------
static void testMips() {
    // sRGB-correct: a fine black/white checker averages to linear 0.5 = sRGB 188, not 128.
    Image c;
    c.width = c.height = 8;
    c.rgba.resize(8 * 8 * 4);
    for (uint32_t y = 0; y < 8; ++y)
        for (uint32_t x = 0; x < 8; ++x) {
            uint8_t v = ((x + y) & 1) ? 255 : 0;
            uint8_t* p = &c.rgba[(y * 8 + x) * 4];
            p[0] = p[1] = p[2] = v;
            p[3] = 255;
        }
    Image m = downsample(c, true);
    CHECK(m.width == 4 && m.height == 4);
    CHECK(m.rgba[0] >= 187 && m.rgba[0] <= 189);
    m = downsample(c, false);
    CHECK(m.rgba[0] >= 127 && m.rgba[0] <= 128);
    // Alpha weighting: transparent black texels do not darken the visible green.
    Image e;
    e.width = e.height = 2;
    e.rgba = {60, 140, 40, 255, 0, 0, 0, 0, 0, 0, 0, 0, 60, 140, 40, 255};
    m = downsample(e, true);
    CHECK(m.rgba[0] == 60 && m.rgba[1] == 140 && m.rgba[2] == 40);
    CHECK(m.rgba[3] == 128);

    // Alpha coverage preserved through the chain (foliage alpha test).
    Image leaves = synth::make(synth::Kind::Leaves, 256, 4);
    Texture t;
    t.format = Format::BC1;
    t.mips.push_back(leaves);
    for (int i = 1; i < 9; ++i) t.mips.push_back(Image());
    Texture plain = t;
    rebuildMips(t, true);
    MipOptions off;
    off.coverage = 0;
    rebuildMips(plain, off);
    const double c0 = alphaCoverage(leaves, 128);
    double worstOn = 0, worstOff = 0;
    for (size_t i = 1; i < 6; ++i) {  // down to 8x8
        CHECK(t.mips[i].width == (256u >> i));
        worstOn = std::max(worstOn, std::fabs(alphaCoverage(t.mips[i], 128) - c0));
        worstOff = std::max(worstOff, std::fabs(alphaCoverage(plain.mips[i], 128) - c0));
    }
    std::printf("alpha coverage mip0 %.3f, worst deviation with preservation %.3f, without %.3f\n", c0, worstOn, worstOff);
    CHECK(worstOn < 0.03);
    CHECK(worstOn <= worstOff);
    CHECK(t.mips[8].width == 1);
    // matchAlphaCoverage on a gradient hits the target.
    Image g;
    g.width = 256; g.height = 1;
    g.rgba.resize(256 * 4);
    for (int i = 0; i < 256; ++i) { g.rgba[i * 4 + 3] = uint8_t(i / 2); }
    matchAlphaCoverage(g, 0.25, 128);
    CHECK(std::fabs(alphaCoverage(g, 128) - 0.25) < 0.01);
}

// ---------------------------------------------------------------------------------------------
static void testFitBias() {
    for (synth::Kind k : {synth::Kind::Leaves, synth::Kind::Grass, synth::Kind::Terrain}) {
        Image img = synth::make(k, 256, 11);
        for (Format f : {Format::BC1, Format::BC3}) {
            size_t s0 = 0, s10 = 0;
            double p0 = 0, p10 = 0;
            for (int fb : {0, 10}) {
                std::vector<uint8_t> b;
                Image d = roundTrip(f, img, fb, &b);
                size_t s = lz4BestSize(b.data(), b.size());
                double p = psnr(img, d, f == Format::BC1 ? 128 : 1);
                (fb ? s10 : s0) = s;
                (fb ? p10 : p0) = p;
            }
            std::printf("fitBias %s %s: LZ4-HC %zu -> %zu (%.0f%%), PSNR %.1f -> %.1f\n", synth::toString(k), toString(f), s0,
                        s10, 100.0 * double(s10) / double(s0), p0, p10);
            CHECK(s10 < s0 * 9 / 10);
            CHECK(p10 > 30);
        }
    }
}

static void testFitMip() {
    Image leaves = synth::make(synth::Kind::Leaves, 256, 21);
    Texture t;
    t.format = Format::BC1;
    t.mips.push_back(leaves);
    std::vector<uint8_t> orig;
    EncodeOptions o0;
    o0.level = 0;
    encodeSurface(Format::BC1, leaves, o0, orig);
    const size_t def = qs::lz4DefaultSize(orig.data(), orig.size());
    // same image, target = LZ4 default size of the original encoding: fits at fitBias 0
    FitResult r = fitMip(t, 0, def, orig.data(), orig.size(), EncodeOptions());
    CHECK(r.ok);
    CHECK(r.fitBias == 0);
    CHECK(r.stored.size() == def);
    std::string err;
    CHECK(qs::lz4VerifyStrict(r.stored.data(), r.stored.size(), r.blocks.data(), r.blocks.size(), &err));
    // a modified image under a tighter target needs fitBias > 0
    Image mod = leaves;
    for (size_t i = 0; i < mod.rgba.size(); i += 4) std::swap(mod.rgba[i], mod.rgba[i + 1]);
    t.mips[0] = mod;
    const size_t tight = lz4BestSize(orig.data(), orig.size()) * 92 / 100;
    r = fitMip(t, 0, tight, orig.data(), orig.size(), EncodeOptions());
    std::printf("fitMip tight target %zu: ok=%d fitBias=%d trials=%d hc=%zu\n", tight, r.ok, r.fitBias, r.trials, r.hcSize);
    CHECK(r.ok);
    CHECK(r.fitBias > 0);
    CHECK(r.stored.size() == tight);
    CHECK(qs::lz4VerifyStrict(r.stored.data(), r.stored.size(), r.blocks.data(), r.blocks.size(), &err));
    // impossible target fails cleanly, raw target returns raw blocks
    r = fitMip(t, 0, 300, orig.data(), orig.size(), EncodeOptions());
    CHECK(!r.ok);
    CHECK(!r.error.empty());
    r = fitMip(t, 0, orig.size(), orig.data(), orig.size(), EncodeOptions());
    CHECK(r.ok && r.raw && r.stored.size() == orig.size());
    // the recolour fallback keeps the LZ4 structure: fits where the full re-encode cannot
    struct Swap { static void fn(void*, Image& img) { for (size_t i = 0; i < img.rgba.size(); i += 4) std::swap(img.rgba[i], img.rgba[i + 1]); } };
    RecolorFallback rf{&Swap::fn, nullptr};
    const size_t veryTight = lz4BestSize(orig.data(), orig.size()) * 102 / 100;
    for (size_t i = 0; i < t.mips[0].rgba.size(); ++i)  // white noise: no re-encode can fit
        t.mips[0].rgba[i] = (i & 3) == 3 ? 255 : uint8_t(qs::dm::hash32(uint32_t(i)));
    r = fitMip(t, 0, veryTight, orig.data(), orig.size(), EncodeOptions(), &rf);
    std::printf("fitMip recolour fallback: ok=%d fitBias=%d\n", r.ok, r.fitBias);
    CHECK(r.ok && r.fitBias == -1);
}

static void testRecolor() {
    Image leaves = synth::make(synth::Kind::Leaves, 128, 5);
    for (Format f : {Format::BC1, Format::BC3}) {
        std::vector<uint8_t> blocks, out;
        encodeSurface(f, leaves, EncodeOptions(), blocks);
        CHECK(recolorSurface(f, blocks.data(), blocks.size(), nullptr, nullptr, out));
        CHECK(out == blocks);  // identity keeps every byte
        struct Inv { static void fn(void*, Image& img) { for (size_t i = 0; i < img.rgba.size(); i += 4) for (int k = 0; k < 3; ++k) img.rgba[i + k] = uint8_t(255 - img.rgba[i + k]); } };
        CHECK(recolorSurface(f, blocks.data(), blocks.size(), &Inv::fn, nullptr, out));
        Image a, b;
        decodeSurface(f, blocks.data(), blocks.size(), 128, 128, a);
        decodeSurface(f, out.data(), out.size(), 128, 128, b);
        size_t bad = 0;
        for (size_t i = 0; i < a.rgba.size(); i += 4) {
            if ((a.rgba[i + 3] == 0) != (b.rgba[i + 3] == 0)) ++bad;  // transparency must not change
            if (a.rgba[i + 3] == 0) continue;
            for (int k = 0; k < 3; ++k) if (std::abs(int(b.rgba[i + k]) - (255 - int(a.rgba[i + k]))) > 10) { ++bad; break; }
        }
        CHECK(bad == 0);
    }
}

int main(int argc, char** argv) {
    testDmath();
    testRoundTrip();
    testDeterminismAndReference();
    testDds();
    testMips();
    testFitBias();
    testFitMip();
    testRecolor();
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
