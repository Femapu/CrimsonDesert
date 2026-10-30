// QuattroStagioni - endpoint tint transform (generator smoke test without a texture codec).
//
// Recolours DXT1/DXT5 textures by transforming the two RGB565 colour endpoints of every block and
// keeping the 2-bit indices (the same trick tools/build_replacement.py used for the phase-2 test).
// Each look has a crude colour grade (spring greener, late summer straw, autumn orange, winter pale).
// It exists to exercise reading, re-encoding, fitting, packing and serving on the user's real files
// before the recipe stream lands; it is not meant to look good.
// DXT1 blocks keep their mode: if the endpoint order (c0 > c1 = opaque 4-colour, else 3-colour +
// transparent) would flip, the block is left unchanged. fitBias > 0 quantises the endpoints (clears
// low bits), which makes the data more repetitive and so more LZ4-compressible.
#include <algorithm>
#include <cstring>

#include "../dds.h"
#include "transform.h"

namespace qs::gen {
namespace {

struct Grade {
    float mul[3] = {1, 1, 1};
    float sat = 1, bright = 1;
};

Grade gradeFor(char look) {
    Grade g;
    switch (look) {
        case 'S': g.mul[0] = 0.95f; g.mul[1] = 1.08f; g.mul[2] = 0.92f; g.sat = 1.10f; break;
        case 'L': g.mul[0] = 1.10f; g.mul[1] = 1.00f; g.mul[2] = 0.75f; g.sat = 0.85f; g.bright = 1.03f; break;
        case 'A': g.mul[0] = 1.30f; g.mul[1] = 0.85f; g.mul[2] = 0.55f; g.sat = 1.05f; break;
        case 'W': g.mul[2] = 1.05f; g.sat = 0.45f; g.bright = 1.08f; break;
        default: break;
    }
    return g;
}

Grade mix(const Grade& a, const Grade& b, float t) {
    Grade g;
    for (int i = 0; i < 3; ++i) g.mul[i] = a.mul[i] + (b.mul[i] - a.mul[i]) * t;
    g.sat = a.sat + (b.sat - a.sat) * t;
    g.bright = a.bright + (b.bright - a.bright) * t;
    return g;
}

uint16_t apply565(uint16_t c, const Grade& g, int quant) {
    float rgb[3] = {float((c >> 11) & 31) * 255.0f / 31.0f, float((c >> 5) & 63) * 255.0f / 63.0f,
                    float(c & 31) * 255.0f / 31.0f};
    for (int i = 0; i < 3; ++i) rgb[i] *= g.mul[i];
    float lum = 0.299f * rgb[0] + 0.587f * rgb[1] + 0.114f * rgb[2];
    for (int i = 0; i < 3; ++i) rgb[i] = (lum + (rgb[i] - lum) * g.sat) * g.bright;
    auto q = [](float v, int maxv) {
        int x = int(v * float(maxv) / 255.0f + 0.5f);
        return std::min(std::max(x, 0), maxv);
    };
    int r = q(rgb[0], 31), gg = q(rgb[1], 63), b = q(rgb[2], 31);
    if (quant > 0) {
        int m5 = ~((1 << quant) - 1), m6 = ~((1 << std::min(quant + 1, 4)) - 1);
        r &= m5; gg &= m6; b &= m5;
    }
    return uint16_t((r << 11) | (gg << 5) | b);
}

class TintWork : public TextureWork {
public:
    TintWork(std::vector<uint8_t> dds, DdsInfo info, Grade g) : dds_(std::move(dds)), info_(std::move(info)), g_(g) {
        blockBytes_ = info_.format == "DXT1" ? 8 : 16;
        colourOff_ = info_.format == "DXT1" ? 0 : 8;
        size_t off = info_.headerSize;
        for (uint32_t i = 0; i < info_.mipCount; ++i) {
            mipOff_.push_back(off);
            off += info_.mipRawSize[i];
        }
    }
    size_t mipCount() const override { return info_.mipCount; }
    bool encodeMip(size_t mip, int fitBias, std::vector<uint8_t>& out, std::string* error) override {
        if (mip >= info_.mipCount) { if (error) *error = "mip out of range"; return false; }
        const uint8_t* src = dds_.data() + mipOff_[mip];
        out.assign(src, src + info_.mipRawSize[mip]);
        const int quant = std::min(std::max(fitBias, 0) / 3, 3);
        const bool dxt1 = blockBytes_ == 8;
        for (size_t p = 0; p + blockBytes_ <= out.size(); p += blockBytes_) {
            uint8_t* c = out.data() + p + colourOff_;
            uint16_t c0 = uint16_t(c[0] | (c[1] << 8)), c1 = uint16_t(c[2] | (c[3] << 8));
            uint16_t n0 = apply565(c0, g_, quant), n1 = apply565(c1, g_, quant);
            if (dxt1 && ((c0 > c1) != (n0 > n1))) continue;  // would change the block mode
            c[0] = uint8_t(n0); c[1] = uint8_t(n0 >> 8); c[2] = uint8_t(n1); c[3] = uint8_t(n1 >> 8);
        }
        return true;
    }

private:
    std::vector<uint8_t> dds_;
    DdsInfo info_;
    Grade g_;
    size_t blockBytes_ = 16, colourOff_ = 8;
    std::vector<size_t> mipOff_;
};

class TintTransform : public TextureTransform {
public:
    std::string name() const override { return "tint"; }
    uint32_t version() const override { return 1; }
    bool affects(const Item& item, const EffectiveStage& st) const override {
        return !st.vanilla() && (item.fmt == "DXT1" || item.fmt == "DXT5");
    }
    std::unique_ptr<TextureWork> begin(const Item&, const EffectiveStage& st, float strength, const uint8_t* dds,
                                       size_t n, std::string* error) const override {
        DdsInfo info;
        if (!parseDdsHeader(dds, n, info, error)) return nullptr;
        if (info.format != "DXT1" && info.format != "DXT5") {
            if (error) *error = "tint: unsupported format " + info.format;
            return nullptr;
        }
        Grade g = mix(Grade(), mix(gradeFor(st.from), gradeFor(st.to), st.t), std::min(std::max(strength, 0.0f), 1.0f));
        return std::unique_ptr<TextureWork>(new TintWork(std::vector<uint8_t>(dds, dds + n), info, g));
    }
};

}  // namespace

std::unique_ptr<TextureTransform> makeTintTransform() { return std::unique_ptr<TextureTransform>(new TintTransform()); }

}  // namespace qs::gen
