// QuattroStagioni - synthetic test textures (procedural, deterministic, our own data).
#include "synth.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "dmath.h"

namespace qs::tex::synth {
namespace {

struct Rgb { double r, g, b; };

struct Canvas {
    Image img;
    uint32_t s;
    explicit Canvas(uint32_t size, bool opaque) : s(size) {
        img.width = img.height = size;
        img.rgba.assign(size_t(size) * size * 4, 0);
        if (opaque) for (size_t i = 0; i < img.rgba.size(); i += 4) img.rgba[i + 3] = 255;
    }
    void put(int x, int y, Rgb c, uint8_t a = 255) {
        if (x < 0 || y < 0 || x >= int(s) || y >= int(s)) return;
        uint8_t* p = &img.rgba[(size_t(y) * s + x) * 4];
        p[0] = uint8_t(dm::clamp(c.r, 0, 255) + 0.5);
        p[1] = uint8_t(dm::clamp(c.g, 0, 255) + 0.5);
        p[2] = uint8_t(dm::clamp(c.b, 0, 255) + 0.5);
        p[3] = a;
    }
};

class Rng {
public:
    explicit Rng(uint32_t seed) : s_(dm::hash32(seed * 0x9E3779B9u + 1)) {}
    double next() { s_ = dm::hash32(s_ + 0x6D2B79F5u); return dm::hashUnit(s_); }
    double range(double a, double b) { return a + (b - a) * next(); }
private:
    uint32_t s_;
};

double vnoise(double x, double y, uint32_t seed) {
    double fx = std::floor(x), fy = std::floor(y);
    int xi = int(fx), yi = int(fy);
    double tx = x - fx, ty = y - fy;
    tx = tx * tx * (3 - 2 * tx);
    ty = ty * ty * (3 - 2 * ty);
    auto h = [&](int a, int b) { return dm::hashUnit(dm::hash3(uint32_t(a), uint32_t(b), seed)); };
    return dm::lerp(dm::lerp(h(xi, yi), h(xi + 1, yi), tx), dm::lerp(h(xi, yi + 1), h(xi + 1, yi + 1), tx), ty);
}

double fbm(double x, double y, uint32_t seed, int oct) {
    double sum = 0, amp = 0.5, norm = 0;
    for (int i = 0; i < oct; ++i) {
        sum += amp * vnoise(x, y, seed + uint32_t(i) * 101u);
        norm += amp;
        x *= 2.03; y *= 2.03; amp *= 0.5;
    }
    return sum / norm;
}

Rgb mix(Rgb a, Rgb b, double t) { return {dm::lerp(a.r, b.r, t), dm::lerp(a.g, b.g, t), dm::lerp(a.b, b.b, t)}; }
Rgb mul(Rgb a, double k) { return {a.r * k, a.g * k, a.b * k}; }

// Stamps a disc of radius r.
void disc(Canvas& c, double cx, double cy, double r, Rgb col, uint32_t seed) {
    int x0 = int(std::floor(cx - r)), x1 = int(std::ceil(cx + r)), y0 = int(std::floor(cy - r)), y1 = int(std::ceil(cy + r));
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            double dx = x + 0.5 - cx, dy = y + 0.5 - cy, d2 = dx * dx + dy * dy;
            if (d2 > r * r) continue;
            double edge = 1.0 - 0.35 * (d2 / (r * r));
            double n = 0.85 + 0.3 * vnoise(x * 0.9, y * 0.15, seed);
            c.put(x, y, mul(col, edge * n));
        }
}

// Draws a curved stroke (quadratic Bezier) with tapering radius.
void stroke(Canvas& c, double x0, double y0, double xm, double ym, double x1, double y1, double r0, double r1, Rgb col,
            uint32_t seed) {
    double len = std::sqrt((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0));
    int steps = std::max(8, int(len * 1.5));
    for (int i = 0; i <= steps; ++i) {
        double t = double(i) / steps, u = 1 - t;
        double x = u * u * x0 + 2 * u * t * xm + t * t * x1, y = u * u * y0 + 2 * u * t * ym + t * t * y1;
        disc(c, x, y, dm::lerp(r0, r1, t), col, seed);
    }
}

// Pointed leaf from base (bx,by) along angle `ang`, length L.
void leaf(Canvas& c, double bx, double by, double ang, double L, double wr, Rgb base, uint32_t seed) {
    double sa, ca;
    dm::sincos(ang, sa, ca);
    int x0 = int(std::floor(std::min(bx, bx + ca * L) - L * wr)), x1 = int(std::ceil(std::max(bx, bx + ca * L) + L * wr));
    int y0 = int(std::floor(std::min(by, by + sa * L) - L * wr)), y1 = int(std::ceil(std::max(by, by + sa * L) + L * wr));
    Rgb rib = {base.r + 30, base.g + 28, base.b + 8};
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            double dx = x + 0.5 - bx, dy = y + 0.5 - by;
            double u = (dx * ca + dy * sa) / L, v = (-dx * sa + dy * ca) / L;
            if (u <= 0 || u >= 1) continue;
            double sn, cs;
            dm::sincos(dm::kPi * dm::pow(u, 0.75), sn, cs);
            double hw = wr * dm::pow(sn, 0.8);
            double av = dm::absd(v);
            if (av >= hw) continue;
            double light = 0.72 + 0.38 * u;              // tip catches more light
            if (v > 0) light *= 0.86;                    // folded half in shade
            if (av > 0.82 * hw) light *= 0.84;           // darker rim
            Rgb col = base;
            if (av < 0.012 + 0.006 * (1 - u)) col = rib;  // midrib
            else {
                double vein = (u * 9.0 - av * 14.0);
                vein -= std::floor(vein);
                if (vein < 0.07 && u > 0.08 && u < 0.92) col = mix(col, rib, 0.45);
            }
            double n = 0.92 + 0.16 * vnoise(x * 0.35, y * 0.35, seed);
            c.put(x, y, mul(col, light * n));
        }
}

Image makeLeaves(uint32_t s, uint32_t seed) {
    Canvas c(s, false);
    Rng rng(seed);
    const double S = s;
    Rgb bark = {96, 74, 54};
    struct Twig { double x0, y0, x1, y1; };
    std::vector<Twig> twigs;
    // Main branch and side twigs.
    double mx0 = 0.06 * S, my0 = 0.96 * S, mxm = 0.30 * S, mym = 0.35 * S, mx1 = 0.94 * S, my1 = 0.10 * S;
    stroke(c, mx0, my0, mxm, mym, mx1, my1, 0.022 * S, 0.006 * S, bark, seed);
    for (int k = 0; k < 7; ++k) {
        double t = 0.15 + 0.11 * k, u = 1 - t;
        double px = u * u * mx0 + 2 * u * t * mxm + t * t * mx1, py = u * u * my0 + 2 * u * t * mym + t * t * my1;
        double side = (k & 1) ? 1 : -1;
        double len = rng.range(0.18, 0.28) * S;
        double ang = -0.75 + side * rng.range(0.7, 1.1);
        double sa, ca;
        dm::sincos(ang, sa, ca);
        double ex = px + ca * len, ey = py + sa * len;
        stroke(c, px, py, (px + ex) / 2 + side * 0.03 * S, (py + ey) / 2, ex, ey, 0.007 * S, 0.003 * S, bark, seed + 7);
        twigs.push_back({px, py, ex, ey});
    }
    static const Rgb greens[5] = {{72, 112, 40}, {60, 100, 36}, {88, 126, 46}, {56, 94, 42}, {98, 132, 52}};
    for (const Twig& t : twigs)
        for (int k = 0; k < 6; ++k) {
            double f = 0.25 + 0.15 * k;
            double bx = dm::lerp(t.x0, t.x1, f), by = dm::lerp(t.y0, t.y1, f);
            double tang = dm::atan2(t.y1 - t.y0, t.x1 - t.x0);
            double ang = tang + ((k & 1) ? 1 : -1) * rng.range(0.6, 1.1);
            Rgb g = greens[int(rng.next() * 5) % 5];
            double j = rng.range(-8, 8);
            g = {g.r + j, g.g + j * 1.2, g.b + j * 0.5};
            leaf(c, bx, by, ang, rng.range(0.085, 0.13) * S, 0.36, g, seed + uint32_t(k));
        }
    return c.img;
}

Image makeConifer(uint32_t s, uint32_t seed) {
    Canvas c(s, false);
    Rng rng(seed);
    const double S = s;
    Rgb bark = {84, 64, 48};
    stroke(c, 0.5 * S, 0.98 * S, 0.48 * S, 0.5 * S, 0.5 * S, 0.04 * S, 0.02 * S, 0.006 * S, bark, seed);
    for (int k = 0; k < 9; ++k) {
        double y = (0.12 + 0.095 * k) * S;
        for (int side = -1; side <= 1; side += 2) {
            double len = (0.12 + 0.035 * k) * S;
            double ex = 0.5 * S + side * len, ey = y + 0.08 * S;
            stroke(c, 0.5 * S, y, 0.5 * S + side * len * 0.5, y - 0.01 * S, ex, ey, 0.006 * S, 0.003 * S, bark, seed);
            int needles = int(len / 2.2);
            for (int n = 0; n < needles; ++n) {
                double t = double(n) / needles;
                double px = dm::lerp(0.5 * S, ex, t), py = dm::lerp(y, ey, t * t);
                for (int d = -1; d <= 1; d += 2) {
                    double ang = (side > 0 ? 0.0 : dm::kPi) + d * rng.range(0.9, 1.4) - side * 0.3;
                    double sa, ca;
                    dm::sincos(ang, sa, ca);
                    double nl = rng.range(0.025, 0.045) * S;
                    Rgb g = rng.next() < 0.5 ? Rgb{38, 72, 48} : Rgb{50, 88, 58};
                    for (double q = 0; q < nl; q += 0.7)
                        c.put(int(px + ca * q), int(py + sa * q), mul(g, 0.8 + 0.35 * q / nl));
                }
            }
        }
    }
    return c.img;
}

Image makeGrass(uint32_t s, uint32_t seed) {
    Canvas c(s, false);
    Rng rng(seed);
    const double S = s;
    for (int k = 0; k < 70; ++k) {
        double x0 = rng.range(0.05, 0.95) * S, h = rng.range(0.45, 0.95) * S, w = rng.range(0.012, 0.024) * S;
        double bend = rng.range(-0.18, 0.18) * S;
        bool dry = rng.next() < 0.08;
        Rgb base = dry ? Rgb{110, 100, 60} : Rgb{34, 62, 26};
        Rgb mid = dry ? Rgb{165, 150, 92} : Rgb{rng.range(72, 92), rng.range(112, 130), rng.range(40, 52)};
        Rgb tip = dry ? Rgb{196, 182, 128} : Rgb{rng.range(128, 150), rng.range(150, 170), rng.range(62, 78)};
        for (int y = int(S - h); y < int(S); ++y) {
            double t = (S - y) / h;  // 0 base .. 1 tip
            double cx = x0 + bend * t * t, hw = w * dm::pow(1 - t, 0.85) + 0.4;
            Rgb col = t < 0.5 ? mix(base, mid, t * 2) : mix(mid, tip, (t - 0.5) * 2);
            for (int x = int(std::floor(cx - hw)); x <= int(std::ceil(cx + hw)); ++x) {
                double d = (x + 0.5 - cx) / hw;
                if (d < -1 || d > 1) continue;
                double shade = d < 0 ? 1.0 : 0.82;
                if (dm::absd(d) < 0.18) shade *= 1.08;  // midline
                c.put(x, y, mul(col, shade * (0.94 + 0.12 * vnoise(x * 0.5, y * 0.08, seed))));
            }
        }
    }
    return c.img;
}

Image makeFlowers(uint32_t s, uint32_t seed) {
    Canvas c(s, false);
    Rng rng(seed);
    const double S = s;
    static const Rgb petals[5] = {{232, 118, 168}, {168, 92, 204}, {246, 214, 62}, {242, 240, 232}, {226, 64, 52}};
    for (int k = 0; k < 7; ++k) {
        double hx = rng.range(0.12, 0.88) * S, hy = rng.range(0.12, 0.6) * S;
        // stem and two leaves
        stroke(c, hx + rng.range(-0.05, 0.05) * S, S, hx + rng.range(-0.06, 0.06) * S, (hy + S) / 2, hx, hy, 0.008 * S,
               0.005 * S, {62, 104, 42}, seed);
        double ly = dm::lerp(hy, S, 0.6);
        leaf(c, hx, ly, -0.6, 0.12 * S, 0.3, {66, 108, 44}, seed + 3);
        leaf(c, hx, ly + 0.05 * S, dm::kPi + 0.6, 0.1 * S, 0.3, {58, 98, 40}, seed + 4);
        Rgb pc = petals[int(rng.next() * 5) % 5];
        int np = 5 + int(rng.next() * 4);
        double pr = rng.range(0.06, 0.09) * S, rot = rng.range(0, 6.28);
        for (int p = 0; p < np; ++p) {
            double ang = rot + p * 2 * dm::kPi / np;
            double sa, ca;
            dm::sincos(ang, sa, ca);
            for (int y = int(hy - pr - 2); y <= int(hy + pr + 2); ++y)
                for (int x = int(hx - pr - 2); x <= int(hx + pr + 2); ++x) {
                    double dx = x + 0.5 - hx, dy = y + 0.5 - hy;
                    double u = (dx * ca + dy * sa) / pr, v = (-dx * sa + dy * ca) / pr;
                    if (u < 0 || u > 1) continue;
                    double hw = 0.42 * dm::pow(u * (1 - u) * 4, 0.6);
                    if (dm::absd(v) > hw) continue;
                    double light = 0.78 + 0.3 * u - 0.12 * dm::absd(v) / (hw + 1e-6);
                    c.put(x, y, mul(pc, light * (0.95 + 0.1 * vnoise(x * 0.6, y * 0.6, seed))));
                }
        }
        Rgb centre = rng.next() < 0.6 ? Rgb{236, 172, 40} : Rgb{70, 48, 28};
        disc(c, hx, hy, pr * 0.28, centre, seed + 9);
    }
    return c.img;
}

Image makeTerrain(uint32_t s, uint32_t seed) {
    Canvas c(s, true);
    const Rgb lush = {68, 104, 44}, grass = {92, 116, 54}, olive = {124, 120, 72}, soil = {112, 92, 66}, stone = {128, 126, 118};
    for (uint32_t y = 0; y < s; ++y)
        for (uint32_t x = 0; x < s; ++x) {
            double u = double(x) / s, v = double(y) / s;
            double n1 = fbm(u * 5, v * 5, seed, 5), n2 = fbm(u * 9 + 3, v * 9, seed + 50, 4), n3 = fbm(u * 40, v * 40, seed + 90, 2);
            Rgb col = mix(lush, grass, dm::smoothstep(0.35, 0.6, n1));
            col = mix(col, olive, dm::smoothstep(0.55, 0.75, n2) * 0.8);
            col = mix(col, soil, dm::smoothstep(0.62, 0.7, n1 * 0.6 + n2 * 0.4));
            col = mix(col, stone, dm::smoothstep(0.78, 0.85, n3) * 0.6);
            c.put(int(x), int(y), mul(col, 0.9 + 0.2 * n3));
        }
    return c.img;
}

Image makeBark(uint32_t s, uint32_t seed) {
    Canvas c(s, true);
    const Rgb base = {98, 82, 66}, crack = {52, 42, 34}, moss = {82, 98, 50};
    for (uint32_t y = 0; y < s; ++y)
        for (uint32_t x = 0; x < s; ++x) {
            double u = double(x) / s, v = double(y) / s;
            double r = fbm(u * 18, v * 3, seed, 4);
            double ridge = 1 - dm::absd(r - 0.5) * 2;
            Rgb col = mix(base, crack, dm::smoothstep(0.75, 0.95, ridge));
            col = mix(col, moss, dm::smoothstep(0.62, 0.72, fbm(u * 4, v * 4, seed + 7, 3)) * 0.7);
            c.put(int(x), int(y), mul(col, 0.88 + 0.24 * fbm(u * 60, v * 12, seed + 3, 2)));
        }
    return c.img;
}

Image makeClimate(uint32_t s, uint32_t seed) {
    Canvas c(s, true);
    for (uint32_t y = 0; y < s; ++y)
        for (uint32_t x = 0; x < s; ++x) {
            double u = double(x) / s, v = double(y) / s;
            double t = 60 + 150 * v + 30 * (fbm(u * 4, v * 4, seed, 4) - 0.5);
            c.put(int(x), int(y), {t, 255 * fbm(u * 3, v * 3, seed + 1, 3), 255 * fbm(u * 2, v * 2, seed + 2, 3)});
        }
    return c.img;
}

}  // namespace

bool kindFromString(const std::string& s, Kind& out) {
    static const struct { const char* n; Kind k; } t[] = {{"leaves", Kind::Leaves}, {"grass", Kind::Grass},
        {"flowers", Kind::Flowers}, {"terrain", Kind::Terrain}, {"bark", Kind::Bark}, {"conifer", Kind::Conifer},
        {"climate", Kind::Climate}};
    for (auto& e : t) if (s == e.n) { out = e.k; return true; }
    return false;
}

const char* toString(Kind k) {
    switch (k) {
        case Kind::Leaves: return "leaves";
        case Kind::Grass: return "grass";
        case Kind::Flowers: return "flowers";
        case Kind::Terrain: return "terrain";
        case Kind::Bark: return "bark";
        case Kind::Conifer: return "conifer";
        case Kind::Climate: return "climate";
    }
    return "?";
}

const char* defaultProfile(Kind k) {
    switch (k) {
        case Kind::Leaves: return "deciduous";
        case Kind::Grass: return "grass";
        case Kind::Flowers: return "flower";
        case Kind::Terrain: return "terrain_color";
        case Kind::Bark: return "bark";
        case Kind::Conifer: return "conifer";
        case Kind::Climate: return "climate_temperature";
    }
    return "deciduous";
}

Image make(Kind k, uint32_t size, uint32_t seed) {
    switch (k) {
        case Kind::Leaves: return makeLeaves(size, seed);
        case Kind::Grass: return makeGrass(size, seed);
        case Kind::Flowers: return makeFlowers(size, seed);
        case Kind::Terrain: return makeTerrain(size, seed);
        case Kind::Bark: return makeBark(size, seed);
        case Kind::Conifer: return makeConifer(size, seed);
        case Kind::Climate: return makeClimate(size, seed);
    }
    return Image();
}

}  // namespace qs::tex::synth
