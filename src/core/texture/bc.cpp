// QuattroStagioni - block codec: best-quality unit encoders (rgbcx + own punch-through BC1) and the
// LZ4-oriented rate/distortion pass behind EncodeOptions::fitBias.
//
// Why a custom RDO pass: the generator must re-encode a modified mip and then store it in EXACTLY the
// original LZ4 size; the exact-size LZ4 encoder can only grow a block. LZ4 has no entropy coder, it
// only wins on repeated byte strings of 4+ bytes. So the pass makes units repeat earlier units: a whole
// 8-byte copy of a recent unit (cheap: one match), a copy that CONTINUES the previous unit's match (a
// longer match: almost free), or half copies (same endpoints with re-chosen selectors, or same
// selectors with least-squares endpoints). Each candidate costs error + lambda * estimated bits;
// lambda and the allowed error grow with fitBias. All decisions use integer errors, so the output is
// deterministic.
#include "bc.h"

#include <algorithm>
#include <cstring>
#include <map>

#include "rgbcx.h"

namespace qs::tex::bc {
namespace {

constexpr int WR = 3, WG = 6, WB = 1;  // colour error weights (sum 10, close to luma)
constexpr uint64_t kBig = uint64_t(1) << 50;

void ensureInit() {
    static const bool done = (rgbcx::init(rgbcx::bc1_approx_mode::cBC1Ideal), true);
    (void)done;
}

inline uint32_t rd32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
inline void wr16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }
inline void wr32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i)); }
inline uint64_t rd48(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 6; ++i) v |= uint64_t(p[i]) << (8 * i);
    return v;
}
inline void wr48(uint8_t* p, uint64_t v) { for (int i = 0; i < 6; ++i) p[i] = uint8_t(v >> (8 * i)); }

struct Pal {
    int c[4][3];
    bool three;
};

void colorPal(uint16_t c0, uint16_t c1, bool bc1Mode, Pal& p) {
    unpack565(c0, p.c[0][0], p.c[0][1], p.c[0][2]);
    unpack565(c1, p.c[1][0], p.c[1][1], p.c[1][2]);
    p.three = bc1Mode && c0 <= c1;
    for (int k = 0; k < 3; ++k) {
        int a = p.c[0][k], b = p.c[1][k];
        if (!p.three) {
            p.c[2][k] = (a * 2 + b) / 3;
            p.c[3][k] = (a + b * 2) / 3;
        } else {
            p.c[2][k] = (a + b) / 2;
            p.c[3][k] = 0;
        }
    }
}

void channelValues(int a0, int a1, int v[8]) {
    v[0] = a0; v[1] = a1;
    if (a0 > a1) {
        for (int i = 1; i <= 6; ++i) v[1 + i] = ((7 - i) * a0 + i * a1) / 7;
    } else {
        for (int i = 1; i <= 4; ++i) v[1 + i] = ((5 - i) * a0 + i * a1) / 5;
        v[6] = 0; v[7] = 255;
    }
}

inline int colorDist(const uint8_t* px, const int* c) {
    int dr = px[0] - c[0], dg = px[1] - c[1], db = px[2] - c[2];
    return WR * dr * dr + WG * dg * dg + WB * db * db;
}

inline uint16_t pack565(int r5, int g6, int b5) { return uint16_t((r5 << 11) | (g6 << 5) | b5); }
inline int q5(double v) { int q = int(v * 31.0 / 255.0 + 0.5); return q < 0 ? 0 : (q > 31 ? 31 : q); }
inline int q6(double v) { int q = int(v * 63.0 / 255.0 + 0.5); return q < 0 ? 0 : (q > 63 ? 63 : q); }

// Colour weight of a texel for BC3 colour units: invisible texels do not matter.
inline bool colorCounts(const UnitSpec& u, const BlockPx& b, int i) {
    if (!((b.valid >> i) & 1)) return false;
    if (u.kind == UnitKind::ColorBC3) return b.px[4 * i + 3] != 0;
    return b.px[4 * i + 3] >= 128;
}

// Best selectors for fixed colour endpoints; returns the error (kBig if the alpha test cannot match).
uint64_t colorSelectors(const UnitSpec& u, uint16_t c0, uint16_t c1, const BlockPx& b, uint32_t& sels) {
    const bool bc1 = u.kind == UnitKind::ColorBC1;
    Pal p;
    colorPal(c0, c1, bc1, p);
    uint64_t err = 0;
    sels = 0;
    for (int i = 0; i < 16; ++i) {
        const uint8_t* px = b.px + 4 * i;
        uint32_t s = 0;
        if (bc1 && px[3] < 128) {
            if (!p.three) return kBig;
            s = 3;
        } else {
            int n = p.three ? 3 : 4, best = 1 << 30;
            for (int k = 0; k < n; ++k) {
                int d = colorDist(px, p.c[k]);
                if (d < best) { best = d; s = uint32_t(k); }
            }
            if (colorCounts(u, b, i)) err += uint64_t(best);
        }
        sels |= s << (2 * i);
    }
    return err;
}

uint64_t channelSelectors(int ch, int a0, int a1, const BlockPx& b, uint64_t& sels) {
    int v[8];
    channelValues(a0, a1, v);
    uint64_t err = 0;
    sels = 0;
    for (int i = 0; i < 16; ++i) {
        int x = b.px[4 * i + ch], best = 1 << 30;
        uint64_t s = 0;
        for (int k = 0; k < 8; ++k) {
            int d = (x - v[k]) * (x - v[k]);
            if (d < best) { best = d; s = uint64_t(k); }
        }
        if ((b.valid >> i) & 1) err += uint64_t(best) * 10;
        sels |= s << (3 * i);
    }
    return err;
}

// Least-squares endpoints for fixed 4-colour selectors (weights of c0: 1, 0, 2/3, 1/3).
bool colorFromSelectors(const UnitSpec& u, uint32_t sels, const BlockPx& b, uint8_t out[8]) {
    static const int w3[4] = {3, 0, 2, 1};
    int64_t A = 0, B = 0, C = 0, R0[3] = {0, 0, 0}, R1[3] = {0, 0, 0};
    for (int i = 0; i < 16; ++i) {
        if (!colorCounts(u, b, i)) continue;
        int w = w3[(sels >> (2 * i)) & 3], v = 3 - w;
        A += w * w; B += w * v; C += v * v;
        for (int k = 0; k < 3; ++k) {
            R0[k] += int64_t(w) * 3 * b.px[4 * i + k];
            R1[k] += int64_t(v) * 3 * b.px[4 * i + k];
        }
    }
    int64_t det = A * C - B * B;
    if (det == 0) return false;
    double e0[3], e1[3];
    for (int k = 0; k < 3; ++k) {
        e0[k] = double(C * R0[k] - B * R1[k]) / double(det);
        e1[k] = double(A * R1[k] - B * R0[k]) / double(det);
    }
    uint16_t c0 = pack565(q5(e0[0]), q6(e0[1]), q5(e0[2])), c1 = pack565(q5(e1[0]), q6(e1[1]), q5(e1[2]));
    if (c0 <= c1) return false;  // the selectors assume 4-colour order
    wr16(out, c0); wr16(out + 2, c1); wr32(out + 4, sels);
    return true;
}

// Least-squares endpoints for fixed 8-value channel selectors.
bool channelFromSelectors(int ch, uint64_t sels, const BlockPx& b, uint8_t out[8]) {
    static const int w7[8] = {7, 0, 6, 5, 4, 3, 2, 1};
    int64_t A = 0, B = 0, C = 0, R0 = 0, R1 = 0;
    for (int i = 0; i < 16; ++i) {
        if (!((b.valid >> i) & 1)) continue;
        int w = w7[(sels >> (3 * i)) & 7], v = 7 - w, x = b.px[4 * i + ch];
        A += w * w; B += w * v; C += v * v;
        R0 += int64_t(w) * 7 * x; R1 += int64_t(v) * 7 * x;
    }
    int64_t det = A * C - B * B;
    if (det == 0) return false;
    double a0 = double(C * R0 - B * R1) / double(det), a1 = double(A * R1 - B * R0) / double(det);
    int i0 = int(a0 + 0.5), i1 = int(a1 + 0.5);
    i0 = std::min(255, std::max(0, i0)); i1 = std::min(255, std::max(0, i1));
    if (i0 <= i1) return false;
    out[0] = uint8_t(i0); out[1] = uint8_t(i1); wr48(out + 2, sels);
    return true;
}

// ---- own BC1 punch-through (3-colour + transparent) encoder ---------------------------------------
struct Q { int c[3]; };  // r5 g6 b5

uint64_t evalPunch(const BlockPx& b, const Q& A, const Q& B, uint8_t choice[16]) {
    int ea[3], eb[3], em[3];
    unpack565(pack565(A.c[0], A.c[1], A.c[2]), ea[0], ea[1], ea[2]);
    unpack565(pack565(B.c[0], B.c[1], B.c[2]), eb[0], eb[1], eb[2]);
    for (int k = 0; k < 3; ++k) em[k] = (ea[k] + eb[k]) / 2;
    uint64_t err = 0;
    for (int i = 0; i < 16; ++i) {
        const uint8_t* px = b.px + 4 * i;
        if (px[3] < 128) { choice[i] = 3; continue; }
        int da = colorDist(px, ea), db = colorDist(px, eb), dm = colorDist(px, em);
        int best = da; choice[i] = 0;
        if (db < best) { best = db; choice[i] = 1; }
        if (dm < best) { best = dm; choice[i] = 2; }
        if ((b.valid >> i) & 1) err += uint64_t(best);
    }
    return err;
}

void writePunch(const Q& A, const Q& B, const uint8_t choice[16], uint8_t out[8]) {
    uint16_t pa = pack565(A.c[0], A.c[1], A.c[2]), pb = pack565(B.c[0], B.c[1], B.c[2]);
    bool aFirst = pa <= pb;
    wr16(out, aFirst ? pa : pb);
    wr16(out + 2, aFirst ? pb : pa);
    uint32_t s = 0;
    for (int i = 0; i < 16; ++i) {
        uint32_t c = choice[i];
        if (c < 2 && !aFirst) c ^= 1;
        s |= c << (2 * i);
    }
    wr32(out + 4, s);
}

void encodePunch(const BlockPx& b, int level, uint8_t out[8]) {
    // Opaque texels only.
    double mean[3] = {0, 0, 0};
    int n = 0;
    for (int i = 0; i < 16; ++i)
        if (b.px[4 * i + 3] >= 128) { for (int k = 0; k < 3; ++k) mean[k] += b.px[4 * i + k]; ++n; }
    for (int k = 0; k < 3; ++k) mean[k] /= n;
    double cov[6] = {0, 0, 0, 0, 0, 0};
    for (int i = 0; i < 16; ++i) {
        if (b.px[4 * i + 3] < 128) continue;
        double d[3] = {b.px[4 * i] - mean[0], b.px[4 * i + 1] - mean[1], b.px[4 * i + 2] - mean[2]};
        cov[0] += d[0] * d[0]; cov[1] += d[0] * d[1]; cov[2] += d[0] * d[2];
        cov[3] += d[1] * d[1]; cov[4] += d[1] * d[2]; cov[5] += d[2] * d[2];
    }
    double ax[3] = {1, 1, 1};
    for (int it = 0; it < 8; ++it) {
        double nx[3] = {cov[0] * ax[0] + cov[1] * ax[1] + cov[2] * ax[2], cov[1] * ax[0] + cov[3] * ax[1] + cov[4] * ax[2],
                        cov[2] * ax[0] + cov[4] * ax[1] + cov[5] * ax[2]};
        double m = std::max(std::max(nx[0] < 0 ? -nx[0] : nx[0], nx[1] < 0 ? -nx[1] : nx[1]), nx[2] < 0 ? -nx[2] : nx[2]);
        if (m < 1e-9) break;
        for (int k = 0; k < 3; ++k) ax[k] = nx[k] / m;
    }
    double pmin = 1e30, pmax = -1e30;
    int imin = -1, imax = -1;
    for (int i = 0; i < 16; ++i) {
        if (b.px[4 * i + 3] < 128) continue;
        double p = (b.px[4 * i] - mean[0]) * ax[0] + (b.px[4 * i + 1] - mean[1]) * ax[1] + (b.px[4 * i + 2] - mean[2]) * ax[2];
        if (p < pmin) { pmin = p; imin = i; }
        if (p > pmax) { pmax = p; imax = i; }
    }
    auto toQ = [](double r, double g, double bb) { Q q; q.c[0] = q5(r); q.c[1] = q6(g); q.c[2] = q5(bb); return q; };
    Q bestA = toQ(b.px[4 * imin], b.px[4 * imin + 1], b.px[4 * imin + 2]);
    Q bestB = toQ(b.px[4 * imax], b.px[4 * imax + 1], b.px[4 * imax + 2]);
    uint8_t bestC[16], tmpC[16];
    uint64_t bestE = evalPunch(b, bestA, bestB, bestC);
    {   // solid candidate
        Q s = toQ(mean[0], mean[1], mean[2]);
        uint64_t e = evalPunch(b, s, s, tmpC);
        if (e < bestE) { bestE = e; bestA = s; bestB = s; std::memcpy(bestC, tmpC, 16); }
    }
    // Least-squares refinement (A weight 1, B weight 0, mid 1/2).
    for (int it = 0; it < 1 + level; ++it) {
        int64_t AA = 0, AB = 0, BBs = 0, R0[3] = {0, 0, 0}, R1[3] = {0, 0, 0};
        for (int i = 0; i < 16; ++i) {
            if (bestC[i] == 3 || !((b.valid >> i) & 1)) continue;
            int w = bestC[i] == 0 ? 2 : (bestC[i] == 1 ? 0 : 1), v = 2 - w;
            AA += w * w; AB += w * v; BBs += v * v;
            for (int k = 0; k < 3; ++k) { R0[k] += int64_t(w) * 2 * b.px[4 * i + k]; R1[k] += int64_t(v) * 2 * b.px[4 * i + k]; }
        }
        int64_t det = AA * BBs - AB * AB;
        if (det == 0) break;
        double ea[3], eb[3];
        for (int k = 0; k < 3; ++k) {
            ea[k] = double(BBs * R0[k] - AB * R1[k]) / double(det);
            eb[k] = double(AA * R1[k] - AB * R0[k]) / double(det);
        }
        Q na = toQ(ea[0], ea[1], ea[2]), nb = toQ(eb[0], eb[1], eb[2]);
        uint64_t e = evalPunch(b, na, nb, tmpC);
        if (e >= bestE) break;
        bestE = e; bestA = na; bestB = nb; std::memcpy(bestC, tmpC, 16);
    }
    // Greedy +-1 search on the six quantized components.
    static const int maxv[3] = {31, 63, 31};
    for (int pass = 0; pass < 1 + level && bestE > 0; ++pass) {
        bool improved = false;
        for (int e = 0; e < 2; ++e)
            for (int k = 0; k < 3; ++k)
                for (int d = -1; d <= 1; d += 2) {
                    Q na = bestA, nb = bestB;
                    Q& t = e ? nb : na;
                    t.c[k] += d;
                    if (t.c[k] < 0 || t.c[k] > maxv[k]) continue;
                    uint64_t er = evalPunch(b, na, nb, tmpC);
                    if (er < bestE) { bestE = er; bestA = na; bestB = nb; std::memcpy(bestC, tmpC, 16); improved = true; }
                }
        if (!improved) break;
    }
    writePunch(bestA, bestB, bestC, out);
}

const uint32_t kRgbcxLevel[4] = {2, 6, 10, 14};
const uint32_t kBc4Radius[4] = {1, 2, 3, 4};

}  // namespace

int unitsOf(Format f, UnitSpec out[2]) {
    switch (f) {
        case Format::BC1: out[0] = {UnitKind::ColorBC1, 0}; return 1;
        case Format::BC3: out[0] = {UnitKind::Channel, 3}; out[1] = {UnitKind::ColorBC3, 0}; return 2;
        case Format::BC4: out[0] = {UnitKind::Channel, 0}; return 1;
        case Format::BC5: out[0] = {UnitKind::Channel, 0}; out[1] = {UnitKind::Channel, 1}; return 2;
        default: return 0;
    }
}

void fetchBlock(const Image& img, uint32_t bx, uint32_t by, BlockPx& out) {
    out.valid = 0;
    for (uint32_t y = 0; y < 4; ++y)
        for (uint32_t x = 0; x < 4; ++x) {
            uint32_t sx = bx * 4 + x, sy = by * 4 + y;
            if (sx < img.width && sy < img.height) out.valid |= uint16_t(1u << (y * 4 + x));
            sx = std::min(sx, img.width - 1);
            sy = std::min(sy, img.height - 1);
            std::memcpy(out.px + 4 * (y * 4 + x), &img.rgba[(size_t(sy) * img.width + sx) * 4], 4);
        }
}

void decodeColor(const uint8_t* unit, bool bc1Mode, uint8_t* rgba) {
    Pal p;
    colorPal(rd16(unit), rd16(unit + 2), bc1Mode, p);
    uint32_t s = rd32(unit + 4);
    for (int i = 0; i < 16; ++i) {
        int k = (s >> (2 * i)) & 3;
        rgba[4 * i] = uint8_t(p.c[k][0]);
        rgba[4 * i + 1] = uint8_t(p.c[k][1]);
        rgba[4 * i + 2] = uint8_t(p.c[k][2]);
        rgba[4 * i + 3] = (p.three && k == 3) ? 0 : 255;
    }
}

void decodeChannel(const uint8_t* unit, uint8_t* out, int stride) {
    int v[8];
    channelValues(unit[0], unit[1], v);
    uint64_t s = rd48(unit + 2);
    for (int i = 0; i < 16; ++i) out[i * stride] = uint8_t(v[(s >> (3 * i)) & 7]);
}

uint64_t unitError(const UnitSpec& u, const uint8_t* unit, const BlockPx& b) {
    uint8_t d[64];
    uint64_t err = 0;
    if (u.kind == UnitKind::Channel) {
        decodeChannel(unit, d, 1);
        for (int i = 0; i < 16; ++i)
            if ((b.valid >> i) & 1) { int e = int(d[i]) - b.px[4 * i + u.channel]; err += uint64_t(e * e) * 10; }
        return err;
    }
    const bool bc1 = u.kind == UnitKind::ColorBC1;
    decodeColor(unit, bc1, d);
    for (int i = 0; i < 16; ++i) {
        const uint8_t* px = b.px + 4 * i;
        if (bc1 && ((px[3] < 128) != (d[4 * i + 3] == 0))) return kBig;  // alpha test must match
        if (!colorCounts(u, b, i)) continue;
        int c[3] = {d[4 * i], d[4 * i + 1], d[4 * i + 2]};
        err += uint64_t(colorDist(px, c));
    }
    return err;
}

void encodeUnit(const UnitSpec& u, const BlockPx& b, int level, const uint8_t* transparentUnit, uint8_t out[8]) {
    ensureInit();
    level = std::min(3, std::max(0, level));
    if (u.kind == UnitKind::Channel) {
        rgbcx::encode_bc4_hq(out, b.px + u.channel, 4, kBc4Radius[level], rgbcx::BC4_USE_ALL_MODES);
        return;
    }
    if (u.kind == UnitKind::ColorBC1) {
        int nT = 0;
        for (int i = 0; i < 16; ++i) nT += b.px[4 * i + 3] < 128;
        if (nT == 16) {
            static const uint8_t kT[8] = {0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF};
            std::memcpy(out, transparentUnit ? transparentUnit : kT, 8);
            return;
        }
        if (nT > 0) { encodePunch(b, level, out); return; }
        uint8_t px[64];
        std::memcpy(px, b.px, 64);
        rgbcx::encode_bc1(kRgbcxLevel[level], out, px, true, false);
        if (unitError(u, out, b) >= kBig) {  // 3-colour block that would punch holes: force 4-colour
            rgbcx::encode_bc1(kRgbcxLevel[level], out, px, false, false);
            if (unitError(u, out, b) >= kBig) {  // degenerate: solid colour, selector 0 is always opaque
                int s[3] = {0, 0, 0};
                for (int i = 0; i < 16; ++i) for (int k = 0; k < 3; ++k) s[k] += px[4 * i + k];
                uint16_t c = pack565(q5(s[0] / 16.0), q6(s[1] / 16.0), q5(s[2] / 16.0));
                wr16(out, c); wr16(out + 2, c); wr32(out + 4, 0);
            }
        }
        return;
    }
    // BC3 colour: ignore fully transparent texels by replacing them with the mean visible colour.
    uint8_t px[64];
    std::memcpy(px, b.px, 64);
    int s[3] = {0, 0, 0}, n = 0;
    for (int i = 0; i < 16; ++i)
        if (px[4 * i + 3]) { for (int k = 0; k < 3; ++k) s[k] += px[4 * i + k]; ++n; }
    if (n > 0 && n < 16)
        for (int i = 0; i < 16; ++i)
            if (!px[4 * i + 3]) for (int k = 0; k < 3; ++k) px[4 * i + k] = uint8_t((s[k] + n / 2) / n);
    rgbcx::encode_bc1(kRgbcxLevel[level], out, px, false, false);
}

// ---------------------------------------------------------------------------------------------------
void encodeBlocks(Format f, const Image& img, const EncodeOptions& opt, const uint8_t* ref, size_t refSize,
                  std::vector<uint8_t>& out) {
    ensureInit();
    UnitSpec us[2];
    const int U = unitsOf(f, us);
    const uint32_t bw = (img.width + 3) / 4, bh = (img.height + 3) / 4;
    const size_t nUnits = size_t(bw) * bh * U;
    out.assign(nUnits * 8, 0);
    if (!U) return;
    const bool useRef = ref && refSize >= out.size();
    const int fb = std::min(10, std::max(0, opt.fitBias));

    // Fully transparent BC1 blocks: reuse the original encoder's favourite transparent block.
    uint8_t transp[8] = {0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF};
    if (useRef && f == Format::BC1) {
        std::map<uint64_t, uint32_t> counts;
        uint64_t bestKey = 0;
        uint32_t bestN = 0;
        for (size_t i = 0; i < nUnits; ++i) {
            const uint8_t* p = ref + i * 8;
            uint8_t d[64];
            decodeColor(p, true, d);
            bool all = true;
            for (int k = 0; k < 16 && all; ++k) all = d[4 * k + 3] == 0;
            if (!all) continue;
            uint64_t key = 0;
            std::memcpy(&key, p, 8);
            uint32_t c = ++counts[key];
            if (c > bestN || (c == bestN && key < bestKey)) { bestN = c; bestKey = key; }
        }
        if (bestN) std::memcpy(transp, &bestKey, 8);
    }

    // RDO parameters (see header comment). Errors are weighted SSE (10 per unit squared level).
    const uint64_t lambda = uint64_t(fb) * fb * 3;
    const uint64_t capPct = 15 * uint64_t(fb);
    const uint64_t capAbs = uint64_t(fb) * fb * 160;
    const int rowWin = fb ? 4 + 6 * fb : 0;   // previous units in stream order
    const int upRad = fb ? 1 + fb / 2 : 0;    // blocks around the same x in the rows above
    const int upRows = fb >= 4 ? 2 : 1;
    const int selCands = fb >= 2 ? 6 : 3;     // least-squares candidates are costlier

    std::vector<int32_t> src(nUnits, -1);  // unit whose bytes this unit repeats (match tracking)
    BlockPx b;
    std::vector<int64_t> cands;
    cands.reserve(256);
    for (uint32_t by = 0; by < bh; ++by)
        for (uint32_t bx = 0; bx < bw; ++bx) {
            fetchBlock(img, bx, by, b);
            const int64_t blk = int64_t(by) * bw + bx;
            for (int ui = 0; ui < U; ++ui) {
                const UnitSpec& u = us[ui];
                const int64_t s = blk * U + ui;
                uint8_t* dst = &out[size_t(s) * 8];
                uint8_t best[8];
                encodeUnit(u, b, opt.level, transp, best);
                uint64_t eBest = unitError(u, best, b);
                if (useRef) {
                    const uint8_t* r = ref + size_t(s) * 8;
                    uint64_t eRef = unitError(u, r, b);
                    uint64_t slack = fb ? uint64_t(fb) * 40 : 0;
                    if (eRef <= eBest + slack) { std::memcpy(best, r, 8); eBest = std::min(eBest, eRef); }
                }
                const int64_t cont = (s > 0 && src[s - 1] >= 0) ? src[s - 1] + 1 : -1;
                if (fb == 0) {
                    std::memcpy(dst, best, 8);
                    if (cont >= 0 && std::memcmp(dst, &out[size_t(cont) * 8], 8) == 0) src[s] = int32_t(cont);
                    continue;
                }
                // Smooth blocks show quality loss first: scale lambda by block activity.
                uint64_t act = 0;
                {
                    int64_t sum[4] = {0, 0, 0, 0}, sq = 0;
                    for (int i = 0; i < 16; ++i)
                        for (int k = 0; k < 4; ++k) sum[k] += b.px[4 * i + k];
                    for (int i = 0; i < 16; ++i) {
                        if (u.kind == UnitKind::Channel) {
                            int64_t d = int64_t(b.px[4 * i + u.channel]) * 16 - sum[u.channel];
                            sq += d * d * 10;
                        } else {
                            int64_t dr = int64_t(b.px[4 * i]) * 16 - sum[0], dg = int64_t(b.px[4 * i + 1]) * 16 - sum[1],
                                    db = int64_t(b.px[4 * i + 2]) * 16 - sum[2];
                            sq += WR * dr * dr + WG * dg * dg + WB * db * db;
                        }
                    }
                    act = uint64_t(sq / (256 * 16));  // per texel weighted variance
                }
                const uint64_t lam = lambda * (act >= 600 ? 4 : 1 + act * 3 / 600) / 4;
                const uint64_t cap = eBest + eBest * capPct / 100 + capAbs * (act >= 600 ? 4 : 1 + act * 3 / 600) / 4;

                // Candidate source units (same unit type), newest first.
                cands.clear();
                if (cont >= 0) cands.push_back(cont);
                for (int k = 1; k <= rowWin; ++k) {
                    int64_t q = s - int64_t(k) * U;
                    if (q < 0) break;
                    if (q != cont) cands.push_back(q);
                }
                for (int r = 1; r <= upRows; ++r)
                    for (int dx = -upRad; dx <= upRad; ++dx) {
                        int64_t j = blk - int64_t(r) * bw + dx;
                        if (j < 0 || j >= blk) continue;
                        int64_t q = j * U + ui;
                        if ((s - q) * 8 > 65000) continue;
                        if (q != cont && s - q > int64_t(rowWin) * U) cands.push_back(q);
                    }

                auto rateOf = [&](const uint8_t* bytes) -> uint64_t {
                    if (cont >= 0 && std::memcmp(bytes, &out[size_t(cont) * 8], 8) == 0) return 2;
                    for (int64_t q : cands)
                        if (std::memcmp(bytes, &out[size_t(q) * 8], 8) == 0) return 24;
                    return 64;
                };
                uint8_t choice[8];
                std::memcpy(choice, best, 8);
                uint64_t bestCost = eBest + lam * rateOf(best);
                int nSel = 0;
                for (int64_t q : cands) {
                    const uint8_t* qb = &out[size_t(q) * 8];
                    // 1) whole copy
                    {
                        uint64_t e = unitError(u, qb, b);
                        uint64_t c = e + lam * (q == cont ? 2 : 24);
                        if (e <= cap && c < bestCost) { bestCost = c; std::memcpy(choice, qb, 8); }
                    }
                    if (u.kind != UnitKind::Channel) {
                        // 2) same endpoints, best selectors
                        uint32_t sels;
                        uint64_t e = colorSelectors(u, rd16(qb), rd16(qb + 2), b, sels);
                        uint64_t c = e + lam * 60;
                        if (e <= cap && c < bestCost) {
                            bestCost = c;
                            std::memcpy(choice, qb, 4);
                            wr32(choice + 4, sels);
                        }
                        // 3) same selectors, least-squares endpoints (4-colour sources only)
                        bool four = u.kind == UnitKind::ColorBC3 || rd16(qb) > rd16(qb + 2);
                        if (four && nSel < selCands) {
                            ++nSel;
                            uint8_t t[8];
                            if (colorFromSelectors(u, rd32(qb + 4), b, t)) {
                                uint64_t e2 = unitError(u, t, b), c2 = e2 + lam * 60;
                                if (e2 <= cap && c2 < bestCost) { bestCost = c2; std::memcpy(choice, t, 8); }
                            }
                        }
                    } else if (qb[0] > qb[1] && nSel < selCands) {
                        // 3) same 6 selector bytes, least-squares endpoints
                        ++nSel;
                        uint8_t t[8];
                        if (channelFromSelectors(u.channel, rd48(qb + 2), b, t)) {
                            uint64_t e2 = unitError(u, t, b), c2 = e2 + lam * 40;
                            if (e2 <= cap && c2 < bestCost) { bestCost = c2; std::memcpy(choice, t, 8); }
                        }
                        // 2) same endpoints, best selectors (2-byte match: only helps as a run start)
                        uint64_t sels;
                        uint64_t e = channelSelectors(u.channel, qb[0], qb[1], b, sels);
                        if (e <= cap && e + lam * 60 < bestCost) {
                            bestCost = e + lam * 60;
                            choice[0] = qb[0]; choice[1] = qb[1];
                            wr48(choice + 2, sels);
                        }
                    }
                }
                std::memcpy(dst, choice, 8);
                if (cont >= 0 && std::memcmp(dst, &out[size_t(cont) * 8], 8) == 0) {
                    src[s] = int32_t(cont);
                } else {
                    for (int64_t q : cands)
                        if (std::memcmp(dst, &out[size_t(q) * 8], 8) == 0) { src[s] = int32_t(q); break; }
                }
            }
        }
}

}  // namespace qs::tex::bc
