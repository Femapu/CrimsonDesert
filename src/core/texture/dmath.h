// QuattroStagioni - deterministic math for the texture codec and the recipes.
//
// The generator runs on every user's PC and its output is cross-checked by hash, so a pixel must come
// out bit-identical from the Linux build, the mingw build and any future compiler. IEEE-754 + - * / and
// sqrt are correctly rounded everywhere (x86-64 SSE2, no FMA contraction: build.sh passes
// -ffp-contract=off), but libm transcendental functions (cbrt, pow, atan2, sin...) are NOT: glibc and
// the Windows CRT round differently. Everything here is built from basic operations only, with fixed
// iteration counts, so the result depends on nothing but the input.
#pragma once
#include <cstdint>
#include <cstring>

namespace qs::dm {

constexpr double kPi = 3.14159265358979323846;
constexpr double kLn2 = 0.69314718055994530942;

inline double clamp(double x, double lo, double hi) { return x < lo ? lo : (x > hi ? hi : x); }
inline double clamp01(double x) { return clamp(x, 0.0, 1.0); }
inline double lerp(double a, double b, double t) { return a + (b - a) * t; }
inline double smoothstep(double e0, double e1, double x) {
    if (e1 == e0) return x < e0 ? 0.0 : 1.0;
    double t = clamp01((x - e0) / (e1 - e0));
    return t * t * (3.0 - 2.0 * t);
}
inline double absd(double x) { return x < 0 ? -x : x; }

// Splits x > 0 into m * 2^e with m in [1, 2).
inline double frexp2(double x, int& e) {
    uint64_t b;
    std::memcpy(&b, &x, 8);
    e = int((b >> 52) & 0x7FF) - 1023;
    b = (b & ~(uint64_t(0x7FF) << 52)) | (uint64_t(1023) << 52);
    double m;
    std::memcpy(&m, &b, 8);
    return m;
}
// 2^e for |e| < 1000.
inline double pow2i(int e) {
    uint64_t b = uint64_t(e + 1023) << 52;
    double r;
    std::memcpy(&r, &b, 8);
    return r;
}

// Natural log, x > 0 (returns a large negative number for x <= 0).
inline double log(double x) {
    if (!(x > 0)) return -1e300;
    int e;
    double m = frexp2(x, e);
    if (m > 1.41421356237309504880) { m *= 0.5; ++e; }
    double s = (m - 1.0) / (m + 1.0), s2 = s * s, term = s, sum = 0.0;
    for (int k = 1; k <= 27; k += 2) { sum += term / k; term *= s2; }  // |s| < 0.1716: plenty
    return 2.0 * sum + e * kLn2;
}

// e^x, |x| < 700.
inline double exp(double x) {
    if (x < -700) return 0.0;
    if (x > 700) x = 700;
    double kf = x / kLn2;
    int k = int(kf < 0 ? kf - 0.5 : kf + 0.5);
    double r = x - k * kLn2, term = 1.0, sum = 1.0;
    for (int i = 1; i <= 18; ++i) { term *= r / i; sum += term; }  // |r| <= 0.347
    return sum * pow2i(k);
}

inline double pow(double x, double y) { return x <= 0 ? 0.0 : exp(y * log(x)); }

// Cube root (any sign): bit-trick start, then fixed Newton steps.
inline double cbrt(double x) {
    if (x == 0) return 0.0;
    bool neg = x < 0;
    double a = neg ? -x : x;
    int e;
    double m = frexp2(a, e);
    int q = (e >= 0) ? e / 3 : -((-e + 2) / 3);
    int r = e - 3 * q;  // 0..2
    double y = pow2i(q) * (r == 0 ? 1.0 : (r == 1 ? 1.25992104989487316 : 1.58740105196819947)) *
               (0.7 + 0.3 * m);  // cbrt(m) ~ within 6%
    for (int i = 0; i < 6; ++i) y = y - (y * y * y - a) / (3.0 * y * y);
    return neg ? -y : y;
}

// atan for t in [0, 1].
inline double atan01(double t) {
    bool red = t > 0.26794919243112270;  // tan(pi/12)
    if (red) t = (t * 1.73205080756887729 - 1.0) / (1.73205080756887729 + t);
    double t2 = t * t, term = t, sum = 0.0;
    for (int k = 1; k <= 25; k += 2) { sum += ((k & 2) ? -term : term) / k; term *= t2; }
    return red ? sum + kPi / 6.0 : sum;
}

inline double atan2(double y, double x) {
    if (x == 0 && y == 0) return 0.0;
    double ax = absd(x), ay = absd(y);
    double a = ay <= ax ? atan01(ay / ax) : kPi / 2.0 - atan01(ax / ay);
    if (x < 0) a = kPi - a;
    return y < 0 ? -a : a;
}

// sin/cos via reduction to [-pi/4, pi/4] and Taylor series.
inline void sincos(double x, double& s, double& c) {
    double qf = x / (kPi / 2.0);
    long long q = (long long)(qf < 0 ? qf - 0.5 : qf + 0.5);
    double r = x - double(q) * (kPi / 2.0), r2 = r * r;
    double ts = r, tc = 1.0, ss = 0.0, cc = 0.0;
    for (int k = 0; k < 10; ++k) {
        ss += ts; cc += tc;
        ts *= -r2 / ((2 * k + 2) * (2 * k + 3));
        tc *= -r2 / ((2 * k + 1) * (2 * k + 2));
    }
    switch (int(q & 3)) {
        case 0: s = ss; c = cc; break;
        case 1: s = cc; c = -ss; break;
        case 2: s = -ss; c = -cc; break;
        default: s = -cc; c = ss; break;
    }
}

// sRGB transfer (IEC 61966-2-1).
inline double srgbToLinear(double v) { return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4); }
inline double linearToSrgb(double l) {
    if (l <= 0) return 0.0;
    return l <= 0.0031308 ? l * 12.92 : 1.055 * pow(l, 1.0 / 2.4) - 0.055;
}

// Integer hash (lowbias32 by Chris Wellons, public domain): cheap, good avalanche.
inline uint32_t hash32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}
inline uint32_t hash3(uint32_t a, uint32_t b, uint32_t c) { return hash32(a ^ hash32(b ^ hash32(c))); }
inline double hashUnit(uint32_t h) { return (h >> 8) * (1.0 / 16777216.0); }  // [0, 1)

}  // namespace qs::dm
