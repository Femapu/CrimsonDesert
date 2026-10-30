// QuattroStagioni - seasonal recipes: parameter files (data/recipes/*.ini) and the pixel transform.
//
// Every pixel is moved in OKLab/OKLCh (perceptually uniform, so a hue rotation keeps perceived
// lightness and veins/shadows stay coherent) with three soft masks computed from the ORIGINAL pixel:
//   green  - foliage hues (yellow-green .. blue-green) with enough chroma: leaves, grass, needles;
//   petal  - saturated non-green colours: flowers, berries;
//   other  - the rest: bark, twigs, soil, grey/brown parts. Kept almost untouched by default.
// Each group has its own parameters per look (hue pull toward a palette target, chroma, lightness,
// contrast, alpha "drop"). The autumn palette is chosen per asset seed (yellow / orange / red / rust)
// so a forest does not turn one uniform colour. Winter leaf fall is an alpha drop of green pixels
// driven by a coarse, seeded noise field: whole clumps vanish, branches (not green) stay, and because
// the noise is fixed per seed a pixel that is gone at one stage stays gone at every later stage.
//
// Stages interpolate PARAMETERS between the two looks (never pixels), so a transition is a real
// intermediate season. Summer (U) is always identity.
//
// Determinism: only + - * / sqrt and the qs::dm helpers (no libm transcendental functions, no
// strtod: numbers are parsed by hand), so every PC produces the same bytes.
#include "recipe.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <unordered_map>

#include "../lookup3.h"
#include "../texture/dmath.h"

namespace qs::recipe {
namespace {

constexpr int kGroups = 3;  // green, petal, other
const char* const kGroupNames[kGroups] = {"green", "petal", "other"};

// ---------------------------------------------------------------------------------------------------
// Parameters as written in the files.
struct GroupP {
    double hueAmount = 0, hueShift = 0, spread = 1, chroma = 1, light = 1, lightAdd = 0, contrast = 1, drop = 0;
    std::string target;  // palette name, empty = the look's seeded palette
};
struct LookP {
    bool defined = false;
    GroupP g[kGroups];
    std::vector<std::pair<std::string, double>> palette;  // name, weight
    double lumHue = 0, tintA = 0, tintB = 0, frost = 0, jitterHue = 0, jitterAmount = 0;
    double dropCells = 14, dropSoft = 0.05;
    double off[3] = {0, 0, 0};
};
struct PaletteEntry { double hue = 0, chroma = 1, light = 1; };
struct Masks {
    double greenHueMin = 105, greenHueMax = 170, greenHueSoft = 20, greenChromaMin = 0.02, greenChromaSoft = 0.03;
    double petalChromaMin = 0.07, petalChromaSoft = 0.05;
};

// Numeric parameters after palette choice and seed jitter (what the pixel code uses).
struct GroupR {
    double amount = 0, tHue = 0, tChroma = 1, tLight = 1, spread = 1, hueShift = 0, chroma = 1, light = 1,
           lightAdd = 0, contrast = 1, drop = 0;
};
struct Resolved {
    GroupR g[kGroups];
    double lumHue = 0, tintA = 0, tintB = 0, frost = 0, dropCells = 14, dropSoft = 0.05;
    double off[3] = {0, 0, 0};
};

bool sameGroup(const GroupR& a, const GroupR& b) {
    // Target values only matter when the hue pull is active.
    if (a.amount != b.amount || a.hueShift != b.hueShift || a.chroma != b.chroma || a.light != b.light ||
        a.lightAdd != b.lightAdd || a.contrast != b.contrast || a.drop != b.drop)
        return false;
    if (a.amount == 0) return true;
    return a.tHue == b.tHue && a.tChroma == b.tChroma && a.tLight == b.tLight && a.spread == b.spread;
}
bool isIdentity(const Resolved& r) {
    Resolved id;
    for (int i = 0; i < kGroups; ++i) if (!sameGroup(r.g[i], id.g[i])) return false;
    return r.tintA == 0 && r.tintB == 0 && r.frost == 0 && r.off[0] == 0 && r.off[1] == 0 && r.off[2] == 0;
}

// ---------------------------------------------------------------------------------------------------
// Text helpers.
std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}
std::string lower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

// Locale-independent, correctly rounded for up to 15 significant digits: integer mantissa / 10^k.
bool parseNum(const std::string& s0, double& out) {
    std::string s = trim(s0);
    size_t i = 0;
    bool neg = false;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) neg = s[i++] == '-';
    int64_t mant = 0;
    int digits = 0, frac = 0;
    bool dot = false, any = false;
    for (; i < s.size(); ++i) {
        char c = s[i];
        if (c >= '0' && c <= '9') {
            any = true;
            if (digits < 15) { mant = mant * 10 + (c - '0'); ++digits; if (dot) ++frac; }
            else if (!dot) return false;  // too large
        } else if (c == '.' && !dot) {
            dot = true;
        } else {
            return false;
        }
    }
    if (!any) return false;
    double p = 1;
    for (int k = 0; k < frac; ++k) p *= 10;  // exact up to 10^15
    out = double(mant) / p;
    if (neg) out = -out;
    return true;
}

std::vector<std::string> splitList(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ',' || c == ' ' || c == '\t') { if (!cur.empty()) out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

int lookIndex(char c) {
    switch (c) {
        case 's': return 0;
        case 'u': return 1;
        case 'l': return 2;
        case 'a': return 3;
        case 'w': return 4;
    }
    return -1;
}

struct KV { std::string key, value; int line; };
struct Section {
    std::string name, file;
    int line = 0;
    std::vector<KV> kv;
};

double angDiff(double a, double b) {  // a - b wrapped to (-180, 180]
    double d = a - b;
    while (d > 180) d -= 360;
    while (d <= -180) d += 360;
    return d;
}

// ---------------------------------------------------------------------------------------------------
// Colour space.
struct ColorLut {
    double lin[256];
    double thr[255];
    ColorLut() {
        for (int i = 0; i < 256; ++i) lin[i] = dm::srgbToLinear(i / 255.0);
        for (int i = 0; i < 255; ++i) thr[i] = dm::srgbToLinear((i + 0.5) / 255.0);
    }
    uint8_t toSrgb(double l) const {
        int lo = 0, hi = 255;
        while (lo < hi) {
            int mid = (lo + hi) / 2;
            if (l < thr[mid]) hi = mid; else lo = mid + 1;
        }
        return uint8_t(lo);
    }
};
const ColorLut& lut() {
    static const ColorLut l;
    return l;
}

void linToLab(const double rgb[3], double lab[3]) {
    double l = 0.4122214708 * rgb[0] + 0.5363325363 * rgb[1] + 0.0514459929 * rgb[2];
    double m = 0.2119034982 * rgb[0] + 0.6806995451 * rgb[1] + 0.1073969566 * rgb[2];
    double s = 0.0883024619 * rgb[0] + 0.2817188376 * rgb[1] + 0.6299787005 * rgb[2];
    l = dm::cbrt(l); m = dm::cbrt(m); s = dm::cbrt(s);
    lab[0] = 0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s;
    lab[1] = 1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s;
    lab[2] = 0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s;
}
void labToLin(const double lab[3], double rgb[3]) {
    double l = lab[0] + 0.3963377774 * lab[1] + 0.2158037573 * lab[2];
    double m = lab[0] - 0.1055613458 * lab[1] - 0.0638541728 * lab[2];
    double s = lab[0] - 0.0894841775 * lab[1] - 1.2914855480 * lab[2];
    l = l * l * l; m = m * m * m; s = s * s * s;
    rgb[0] = 4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s;
    rgb[1] = -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s;
    rgb[2] = -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s;
}
bool inGamut(const double rgb[3]) {
    const double e = 1e-7;
    return rgb[0] >= -e && rgb[0] <= 1 + e && rgb[1] >= -e && rgb[1] <= 1 + e && rgb[2] >= -e && rgb[2] <= 1 + e;
}

double hueWindow(double h, double lo, double hi, double soft) {
    if (h >= lo && h <= hi) return 1.0;
    double d = std::min(dm::absd(angDiff(h, lo)), dm::absd(angDiff(h, hi)));
    return 1.0 - dm::smoothstep(0.0, soft, d);
}

}  // namespace

// ---------------------------------------------------------------------------------------------------
struct RecipeBook::Impl {
    std::map<std::string, Section> sections;
    std::map<std::string, PaletteEntry> palettes;
    Masks masks;
    struct Profile {
        std::string inherit;
        double strength = 1;
        bool channels = false;
        LookP looks[5];
    };
    std::map<std::string, Profile> profiles;

    bool addText(const std::string& text, const std::string& file, std::vector<std::string>& errors);
    bool build(std::vector<std::string>& errors);
    bool setKey(LookP& lp, const std::string& key, const std::string& value, std::string& err) const;
    bool buildLook(const std::string& prof, int look, LookP& out, int depth, std::vector<std::string>& errors);
    Resolved resolve(const LookP& lp, uint32_t seed) const;
    bool stageParams(const Context& ctx, const Stage& st, Resolved& out, bool& channels, std::string* error) const;
};

namespace {

std::string where(const Section& s, int line) { return s.file + ":" + std::to_string(line); }

}  // namespace

bool RecipeBook::Impl::addText(const std::string& text, const std::string& file, std::vector<std::string>& errors) {
    bool ok = true;
    std::istringstream in(text);
    std::string raw, cur;
    int ln = 0;
    while (std::getline(in, raw)) {
        ++ln;
        std::string line = raw;
        if (ln == 1 && line.size() >= 3 && uint8_t(line[0]) == 0xEF && uint8_t(line[1]) == 0xBB && uint8_t(line[2]) == 0xBF)
            line = line.substr(3);
        // strip comments: ';' or '#' at line start or after whitespace
        for (size_t i = 0; i < line.size(); ++i)
            if ((line[i] == ';' || line[i] == '#') && (i == 0 || line[i - 1] == ' ' || line[i - 1] == '\t')) {
                line = line.substr(0, i);
                break;
            }
        line = trim(line);
        if (line.empty()) continue;
        if (line[0] == '[') {
            size_t c = line.find(']');
            if (c == std::string::npos || trim(line.substr(c + 1)) != "") {
                errors.push_back(file + ":" + std::to_string(ln) + ": malformed section header");
                ok = false;
                cur.clear();
                continue;
            }
            cur = lower(trim(line.substr(1, c - 1)));
            if (sections.count(cur)) {
                errors.push_back(file + ":" + std::to_string(ln) + ": duplicate section [" + cur + "] (first at " +
                                 sections[cur].file + ":" + std::to_string(sections[cur].line) + ")");
                ok = false;
                cur.clear();
                continue;
            }
            Section& s = sections[cur];
            s.name = cur;
            s.file = file;
            s.line = ln;
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) {
            errors.push_back(file + ":" + std::to_string(ln) + ": expected key = value");
            ok = false;
            continue;
        }
        if (cur.empty()) {
            errors.push_back(file + ":" + std::to_string(ln) + ": key outside of a section");
            ok = false;
            continue;
        }
        sections[cur].kv.push_back({lower(trim(line.substr(0, eq))), trim(line.substr(eq + 1)), ln});
    }
    return ok;
}

bool RecipeBook::Impl::setKey(LookP& lp, const std::string& key, const std::string& value, std::string& err) const {
    double v = 0;
    auto num = [&](double& dst) {
        if (!parseNum(value, v)) { err = "bad number '" + value + "' for " + key; return false; }
        dst = v;
        return true;
    };
    if (key == "palette") {
        lp.palette.clear();
        for (const std::string& item : splitList(value)) {
            std::string name = lower(item);
            double w = 1;
            size_t c = name.find(':');
            if (c != std::string::npos) {
                if (!parseNum(name.substr(c + 1), w) || w <= 0) { err = "bad palette weight in '" + item + "'"; return false; }
                name = name.substr(0, c);
            }
            if (!palettes.count(name)) { err = "unknown palette colour '" + name + "'"; return false; }
            lp.palette.push_back({name, w});
        }
        if (lp.palette.empty()) { err = "empty palette"; return false; }
        return true;
    }
    if (key == "lum_hue") return num(lp.lumHue);
    if (key == "tint_a") return num(lp.tintA);
    if (key == "tint_b") return num(lp.tintB);
    if (key == "frost") return num(lp.frost);
    if (key == "jitter_hue") return num(lp.jitterHue);
    if (key == "jitter_amount") return num(lp.jitterAmount);
    if (key == "drop_cells") { if (!num(lp.dropCells)) return false; if (v < 1) { err = "drop_cells must be >= 1"; return false; } return true; }
    if (key == "drop_soft") { if (!num(lp.dropSoft)) return false; if (v <= 0 || v > 0.4) { err = "drop_soft must be in (0, 0.4]"; return false; } return true; }
    if (key == "r_offset") return num(lp.off[0]);
    if (key == "g_offset") return num(lp.off[1]);
    if (key == "b_offset") return num(lp.off[2]);
    size_t dot = key.find('.');
    if (dot != std::string::npos) {
        std::string grp = key.substr(0, dot), k = key.substr(dot + 1);
        int gi = -1;
        for (int i = 0; i < kGroups; ++i) if (grp == kGroupNames[i]) gi = i;
        if (gi >= 0) {
            GroupP& g = lp.g[gi];
            if (k == "target") {
                std::string name = lower(value);
                if (!palettes.count(name)) { err = "unknown palette colour '" + name + "'"; return false; }
                g.target = name;
                return true;
            }
            if (k == "hue_amount") { if (!num(g.hueAmount)) return false; if (v < 0 || v > 1) { err = key + " must be in [0, 1]"; return false; } return true; }
            if (k == "drop") { if (!num(g.drop)) return false; if (v < 0 || v > 1) { err = key + " must be in [0, 1]"; return false; } return true; }
            if (k == "hue_shift") return num(g.hueShift);
            if (k == "spread") return num(g.spread);
            if (k == "chroma") { if (!num(g.chroma)) return false; if (v < 0) { err = key + " must be >= 0"; return false; } return true; }
            if (k == "light") { if (!num(g.light)) return false; if (v < 0) { err = key + " must be >= 0"; return false; } return true; }
            if (k == "light_add") return num(g.lightAdd);
            if (k == "contrast") return num(g.contrast);
        }
    }
    err = "unknown key '" + key + "'";
    return false;
}

namespace {

void scaleLook(LookP& lp, double s) {
    auto sc = [&](double& x, double id) { x = id + (x - id) * s; };
    for (GroupP& g : lp.g) {
        sc(g.hueAmount, 0); sc(g.hueShift, 0); sc(g.chroma, 1); sc(g.light, 1); sc(g.lightAdd, 0);
        sc(g.contrast, 1); sc(g.drop, 0);
    }
    sc(lp.tintA, 0); sc(lp.tintB, 0); sc(lp.frost, 0);
    for (double& o : lp.off) sc(o, 0);
}

}  // namespace

bool RecipeBook::Impl::buildLook(const std::string& prof, int look, LookP& out, int depth, std::vector<std::string>& errors) {
    out = LookP();
    if (depth > 8) { errors.push_back("profile '" + prof + "': inherit chain too deep (cycle?)"); return false; }
    auto pit = profiles.find(prof);
    if (pit == profiles.end()) return true;
    const Profile& p = pit->second;
    if (!p.inherit.empty()) {
        if (!buildLook(p.inherit, look, out, depth + 1, errors)) return false;
        if (p.strength != 1) scaleLook(out, p.strength);
    }
    static const char kL[5] = {'s', 'u', 'l', 'a', 'w'};
    auto sit = sections.find(prof + "." + kL[look]);
    if (sit != sections.end()) {
        out.defined = true;
        for (const KV& kv : sit->second.kv) {
            std::string err;
            if (!setKey(out, kv.key, kv.value, err)) {
                errors.push_back(where(sit->second, kv.line) + ": [" + sit->first + "] " + err);
                return false;
            }
        }
    }
    return true;
}

bool RecipeBook::Impl::build(std::vector<std::string>& errors) {
    bool ok = true;
    palettes.clear();
    profiles.clear();
    masks = Masks();
    // 1) palette and masks
    if (sections.count("palette")) {
        const Section& s = sections["palette"];
        for (const KV& kv : s.kv) {
            std::vector<std::string> parts = splitList(kv.value);
            PaletteEntry e;
            if (parts.size() != 3 || !parseNum(parts[0], e.hue) || !parseNum(parts[1], e.chroma) || !parseNum(parts[2], e.light)) {
                errors.push_back(where(s, kv.line) + ": palette entry must be 'hue, chroma, light'");
                ok = false;
                continue;
            }
            palettes[kv.key] = e;
        }
    }
    if (sections.count("masks")) {
        const Section& s = sections["masks"];
        for (const KV& kv : s.kv) {
            static const struct { const char* k; double Masks::*m; } keys[] = {
                {"green_hue_min", &Masks::greenHueMin}, {"green_hue_max", &Masks::greenHueMax},
                {"green_hue_soft", &Masks::greenHueSoft}, {"green_chroma_min", &Masks::greenChromaMin},
                {"green_chroma_soft", &Masks::greenChromaSoft}, {"petal_chroma_min", &Masks::petalChromaMin},
                {"petal_chroma_soft", &Masks::petalChromaSoft}};
            bool found = false;
            for (auto& k : keys)
                if (kv.key == k.k) {
                    found = true;
                    if (!parseNum(kv.value, masks.*(k.m))) {
                        errors.push_back(where(s, kv.line) + ": bad number '" + kv.value + "'");
                        ok = false;
                    }
                }
            if (!found) { errors.push_back(where(s, kv.line) + ": [masks] unknown key '" + kv.key + "'"); ok = false; }
        }
    }
    // 2) profile sections
    for (const auto& it : sections) {
        const std::string& name = it.first;
        if (name == "palette" || name == "masks") continue;
        size_t dot = name.find('.');
        if (dot == std::string::npos) {
            Profile& p = profiles[name];
            for (const KV& kv : it.second.kv) {
                if (kv.key == "inherit") p.inherit = lower(kv.value);
                else if (kv.key == "strength") {
                    if (!parseNum(kv.value, p.strength) || p.strength < 0) {
                        errors.push_back(where(it.second, kv.line) + ": bad strength '" + kv.value + "'");
                        ok = false;
                    }
                } else if (kv.key == "mode") {
                    std::string m = lower(kv.value);
                    if (m == "channels") p.channels = true;
                    else if (m == "color" || m == "colour") p.channels = false;
                    else { errors.push_back(where(it.second, kv.line) + ": mode must be color or channels"); ok = false; }
                } else if (kv.key == "description") {
                } else {
                    errors.push_back(where(it.second, kv.line) + ": [" + name + "] unknown key '" + kv.key + "'");
                    ok = false;
                }
            }
        } else {
            std::string prof = name.substr(0, dot), lk = lower(name.substr(dot + 1));
            int li = lk.size() == 1 ? lookIndex(lk[0]) : -1;
            if (li < 0) {
                errors.push_back(where(it.second, it.second.line) + ": [" + name + "] look must be S, L, A or W");
                ok = false;
            } else if (li == 1) {
                errors.push_back(where(it.second, it.second.line) + ": [" + name + "] summer is vanilla (identity), it cannot be changed");
                ok = false;
            }
            profiles[prof];  // a look section alone also defines the profile
        }
    }
    for (const auto& it : profiles)
        if (!it.second.inherit.empty() && !profiles.count(it.second.inherit)) {
            errors.push_back("profile '" + it.first + "' inherits unknown profile '" + it.second.inherit + "'");
            ok = false;
        }
    if (!ok) return false;
    // 3) resolve inheritance and validate every key
    for (auto& it : profiles)
        for (int l = 0; l < 5; ++l) {
            if (l == 1) continue;
            LookP lp;
            if (!buildLook(it.first, l, lp, 0, errors)) { ok = false; continue; }
            it.second.looks[l] = lp;
        }
    // inherit "channels" mode
    for (auto& it : profiles) {
        std::string p = it.second.inherit;
        for (int d = 0; d < 8 && !p.empty() && profiles.count(p); ++d) {
            if (profiles[p].channels) it.second.channels = true;
            p = profiles[p].inherit;
        }
    }
    return ok;
}

Resolved RecipeBook::Impl::resolve(const LookP& lp, uint32_t seed) const {
    Resolved r;
    if (!lp.defined) return r;
    // Seeded choices: one uniform number per decision, shared by all looks of the asset.
    const double uPal = dm::hashUnit(dm::hash32(seed ^ 0x5EA5011u));
    const double uHue = dm::hashUnit(dm::hash32(seed ^ 0x0D15C0u)) * 2 - 1;
    const double uAmt = dm::hashUnit(dm::hash32(seed ^ 0xA770u)) * 2 - 1;
    PaletteEntry pick;
    if (!lp.palette.empty()) {
        double tot = 0;
        for (auto& p : lp.palette) tot += p.second;
        double x = uPal * tot, acc = 0;
        pick = palettes.at(lp.palette.back().first);
        for (auto& p : lp.palette) {
            acc += p.second;
            if (x < acc) { pick = palettes.at(p.first); break; }
        }
    }
    for (int i = 0; i < kGroups; ++i) {
        const GroupP& g = lp.g[i];
        GroupR& o = r.g[i];
        PaletteEntry t = g.target.empty() ? pick : palettes.at(g.target);
        o.amount = g.hueAmount > 0 ? dm::clamp01(g.hueAmount + uAmt * lp.jitterAmount) : 0;
        o.tHue = t.hue + uHue * lp.jitterHue;
        o.tChroma = t.chroma;
        o.tLight = t.light;
        o.spread = g.spread;
        o.hueShift = g.hueShift;
        o.chroma = g.chroma;
        o.light = g.light;
        o.lightAdd = g.lightAdd;
        o.contrast = g.contrast;
        o.drop = g.drop;
    }
    r.lumHue = lp.lumHue;
    r.tintA = lp.tintA;
    r.tintB = lp.tintB;
    r.frost = lp.frost;
    r.dropCells = lp.dropCells;
    r.dropSoft = lp.dropSoft;
    for (int k = 0; k < 3; ++k) r.off[k] = lp.off[k];
    return r;
}

namespace {

Resolved lerpResolved(Resolved a, Resolved b, double t) {
    for (int i = 0; i < kGroups; ++i) {
        // A side without hue pull borrows the other side's target so only the amount fades.
        if (a.g[i].amount == 0) { a.g[i].tHue = b.g[i].tHue; a.g[i].tChroma = b.g[i].tChroma; a.g[i].tLight = b.g[i].tLight; a.g[i].spread = b.g[i].spread; }
        if (b.g[i].amount == 0) { b.g[i].tHue = a.g[i].tHue; b.g[i].tChroma = a.g[i].tChroma; b.g[i].tLight = a.g[i].tLight; b.g[i].spread = a.g[i].spread; }
    }
    if (a.lumHue == 0 && b.lumHue != 0) a.lumHue = b.lumHue;
    if (b.lumHue == 0 && a.lumHue != 0) b.lumHue = a.lumHue;
    Resolved r;
    // When leaves fall or regrow, colour leads the alpha: leaves turn brown before they drop, and come
    // back green while still sparse.
    const bool leafFall = (a.g[0].drop + a.g[1].drop + a.g[2].drop + b.g[0].drop + b.g[1].drop + b.g[2].drop) > 0;
    const double tc = leafFall ? 1 - (1 - t) * (1 - t) : t;
    auto L = [tc](double x, double y) { return x + (y - x) * tc; };
    auto LD = [t](double x, double y) { return x + (y - x) * t; };
    for (int i = 0; i < kGroups; ++i) {
        const GroupR &x = a.g[i], &y = b.g[i];
        GroupR& o = r.g[i];
        o.amount = L(x.amount, y.amount);
        o.tHue = x.tHue + angDiff(y.tHue, x.tHue) * tc;
        o.tChroma = L(x.tChroma, y.tChroma);
        o.tLight = L(x.tLight, y.tLight);
        o.spread = L(x.spread, y.spread);
        o.hueShift = L(x.hueShift, y.hueShift);
        o.chroma = L(x.chroma, y.chroma);
        o.light = L(x.light, y.light);
        o.lightAdd = L(x.lightAdd, y.lightAdd);
        o.contrast = L(x.contrast, y.contrast);
        o.drop = LD(x.drop, y.drop);
    }
    r.lumHue = L(a.lumHue, b.lumHue);
    r.tintA = L(a.tintA, b.tintA);
    r.tintB = L(a.tintB, b.tintB);
    r.frost = L(a.frost, b.frost);
    r.dropCells = (a.g[0].drop + a.g[1].drop + a.g[2].drop) > 0 ? a.dropCells : b.dropCells;
    r.dropSoft = (a.g[0].drop + a.g[1].drop + a.g[2].drop) > 0 ? a.dropSoft : b.dropSoft;
    for (int k = 0; k < 3; ++k) r.off[k] = L(a.off[k], b.off[k]);
    return r;
}

struct PixelOut {
    uint8_t rgb[3];
    float drop;  // alpha drop amount for this colour (0 = keep)
};

PixelOut transformColor(const Resolved& R, const Masks& M, const uint8_t in[3]) {
    const ColorLut& T = lut();
    double rgb[3] = {T.lin[in[0]], T.lin[in[1]], T.lin[in[2]]}, lab[3];
    linToLab(rgb, lab);
    const double L = lab[0], C = std::sqrt(lab[1] * lab[1] + lab[2] * lab[2]);
    double h = dm::atan2(lab[2], lab[1]) * (180.0 / dm::kPi);
    if (h < 0) h += 360;
    // masks from the original colour
    const double hw = hueWindow(h, M.greenHueMin, M.greenHueMax, M.greenHueSoft);
    double m[kGroups];
    m[0] = hw * dm::smoothstep(M.greenChromaMin, M.greenChromaMin + M.greenChromaSoft, C);
    m[1] = (1 - hw) * dm::smoothstep(M.petalChromaMin, M.petalChromaMin + M.petalChromaSoft, C);
    m[2] = std::max(0.0, 1 - m[0] - m[1]);
    double oL = 0, oa = 0, ob = 0, drop = 0;
    for (int i = 0; i < kGroups; ++i) {
        const GroupR& g = R.g[i];
        drop += m[i] * g.drop;
        if (m[i] == 0) continue;
        double L2 = L, a2 = lab[1], b2 = lab[2];
        if (g.amount != 0 || g.hueShift != 0 || g.chroma != 1 || g.light != 1 || g.lightAdd != 0 || g.contrast != 1) {
            double h2 = h, C2 = C;
            if (g.amount != 0) {
                double tgt = g.tHue + R.lumHue * (L - 0.5);
                if (i == 0) tgt += g.spread * angDiff(h, 130.0);
                h2 = h + g.amount * angDiff(tgt, h);
                C2 = C * (1 + (g.tChroma - 1) * g.amount);
                L2 = L * (1 + (g.tLight - 1) * g.amount);
            }
            h2 += g.hueShift;
            C2 *= g.chroma;
            L2 = (0.5 + (L2 - 0.5) * g.contrast) * g.light + g.lightAdd;
            double s, c;
            dm::sincos(h2 * (dm::kPi / 180.0), s, c);
            a2 = C2 * c;
            b2 = C2 * s;
        }
        oL += m[i] * L2;
        oa += m[i] * a2;
        ob += m[i] * b2;
    }
    oa += R.tintA;
    ob += R.tintB;
    if (R.frost > 0) {
        double f = dm::clamp01(R.frost * (0.6 + 0.5 * oL));
        oL = dm::lerp(oL, 0.9, f);
        oa = dm::lerp(oa, -0.004, f);
        ob = dm::lerp(ob, -0.012, f);
    }
    double outLab[3] = {dm::clamp(oL, 0.0, 0.995), oa, ob}, lin[3];
    labToLin(outLab, lin);
    if (!inGamut(lin)) {  // keep L and hue, reduce chroma (fixed bisection)
        double lo = 0, hi = 1;
        for (int it = 0; it < 14; ++it) {
            double mid = (lo + hi) / 2, t[3] = {outLab[0], outLab[1] * mid, outLab[2] * mid};
            labToLin(t, lin);
            if (inGamut(lin)) lo = mid; else hi = mid;
        }
        double t[3] = {outLab[0], outLab[1] * lo, outLab[2] * lo};
        labToLin(t, lin);
    }
    PixelOut o;
    for (int k = 0; k < 3; ++k) o.rgb[k] = T.toSrgb(dm::clamp01(lin[k]));
    o.drop = float(drop);
    return o;
}

// Coarse seeded noise in [0, 1) used for leaf fall (normalised image coordinates).
double dropNoise(double u, double v, double cells, uint32_t seed) {
    auto vn = [&](double x, double y, uint32_t s) {
        double fx = std::floor(x), fy = std::floor(y);
        int xi = int(fx), yi = int(fy);
        double tx = x - fx, ty = y - fy;
        tx = tx * tx * (3 - 2 * tx);
        ty = ty * ty * (3 - 2 * ty);
        auto h = [&](int a, int b) { return dm::hashUnit(dm::hash3(uint32_t(a), uint32_t(b), s)); };
        return dm::lerp(dm::lerp(h(xi, yi), h(xi + 1, yi), tx), dm::lerp(h(xi, yi + 1), h(xi + 1, yi + 1), tx), ty);
    };
    double n = 0.75 * vn(u * cells, v * cells, seed) + 0.25 * vn(u * cells * 4.1, v * cells * 4.1, seed ^ 0x9E37u);
    return dm::clamp((n - 0.5) * 1.7 + 0.5, 0.0, 0.999999);  // stretch toward uniform
}

}  // namespace

bool RecipeBook::Impl::stageParams(const Context& ctx, const Stage& st, Resolved& out, bool& channels,
                                   std::string* error) const {
    auto it = profiles.find(lower(ctx.profile));
    if (it == profiles.end()) {
        if (error) *error = "unknown profile '" + ctx.profile + "'";
        return false;
    }
    channels = it->second.channels;
    const int a = int(st.from), b = int(st.to);
    if (a < 0 || a > 4 || b < 0 || b > 4) { if (error) *error = "bad look"; return false; }
    const double t = dm::clamp01(double(st.t));
    Resolved ra = resolve(it->second.looks[a], ctx.seed), rb = resolve(it->second.looks[b], ctx.seed);
    out = (a == b || t == 0) ? ra : (t == 1 ? rb : lerpResolved(ra, rb, t));
    return true;
}

// ---------------------------------------------------------------------------------------------------
RecipeBook::RecipeBook() : impl_(new Impl) {}
RecipeBook::~RecipeBook() { delete impl_; }

bool RecipeBook::loadText(const std::string& text, std::vector<std::string>& errors) {
    *impl_ = Impl();
    bool ok = impl_->addText(text, "<text>", errors);
    return impl_->build(errors) && ok;
}

bool RecipeBook::load(const std::string& dir, std::vector<std::string>& errors) {
    *impl_ = Impl();
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file() && lower(it->path().extension().string()) == ".ini") files.push_back(it->path());
    if (ec) { errors.push_back("cannot read directory " + dir + ": " + ec.message()); return false; }
    if (files.empty()) { errors.push_back("no .ini recipe files in " + dir); return false; }
    std::sort(files.begin(), files.end());
    bool ok = true;
    for (const auto& f : files) {
        std::ifstream in(f, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        if (!in.good() && !in.eof()) { errors.push_back("cannot read " + f.string()); ok = false; continue; }
        ok = impl_->addText(ss.str(), f.filename().string(), errors) && ok;
    }
    return impl_->build(errors) && ok;
}

bool RecipeBook::affects(const Context& ctx, const Stage& st) const {
    Resolved r;
    bool ch;
    if (!impl_->stageParams(ctx, st, r, ch, nullptr)) return false;
    return !isIdentity(r);
}

bool RecipeBook::apply(const Context& ctx, const Stage& st, tex::Image& img, std::string* error) const {
    Resolved R;
    bool channels = false;
    if (!impl_->stageParams(ctx, st, R, channels, error)) return false;
    if (img.rgba.size() < size_t(img.width) * img.height * 4) { if (error) *error = "bad image"; return false; }
    if (isIdentity(R)) return true;
    const size_t n = size_t(img.width) * img.height;
    if (channels) {
        int off[3];
        for (int k = 0; k < 3; ++k) off[k] = int(R.off[k] < 0 ? R.off[k] - 0.5 : R.off[k] + 0.5);
        for (size_t i = 0; i < n; ++i)
            for (int k = 0; k < 3; ++k) {
                int v = img.rgba[i * 4 + k] + off[k];
                img.rgba[i * 4 + k] = uint8_t(v < 0 ? 0 : (v > 255 ? 255 : v));
            }
        return true;
    }
    std::unordered_map<uint32_t, PixelOut> cache;
    cache.reserve(4096);
    const bool anyDrop = R.g[0].drop > 0 || R.g[1].drop > 0 || R.g[2].drop > 0;
    // Leaf fall only makes sense where the texture already uses alpha (cut-out foliage).
    bool hasCutout = false;
    for (size_t i = 0; i < n && !hasCutout; ++i) hasCutout = img.rgba[i * 4 + 3] < 128;
    const bool dropping = anyDrop && hasCutout;
    std::vector<float> dropAmt(dropping ? n : 0, 0.0f);
    for (size_t i = 0; i < n; ++i) {
        uint8_t* p = &img.rgba[i * 4];
        if (p[3] == 0) continue;  // invisible texels stay byte-identical
        const uint32_t key = uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16);
        auto it = cache.find(key);
        if (it == cache.end()) it = cache.emplace(key, transformColor(R, impl_->masks, p)).first;
        const PixelOut& o = it->second;
        p[0] = o.rgb[0]; p[1] = o.rgb[1]; p[2] = o.rgb[2];
        if (dropping) dropAmt[i] = o.drop;
    }
    if (!dropping) return true;
    // Leaf fall. Small connected groups of droppable texels (single leaves, small clusters) fall as
    // a whole, using one random value per group; large connected canopies fall in clumps following a
    // coarse noise field. Both are fixed per seed, so the fallen set only grows with the drop amount.
    const uint32_t W = img.width, H = img.height;
    std::vector<int32_t> comp(n, -1);
    std::vector<double> compVal;
    std::vector<size_t> stack;
    const size_t maxLeaf = std::max<size_t>(64, n * 8 / 100);
    // Membership is relative to the stage's strongest drop so the grouping is the same at every stage.
    const float member = float(0.3 * std::max(R.g[0].drop, std::max(R.g[1].drop, R.g[2].drop)));
    for (size_t i = 0; i < n; ++i) {
        if (comp[i] >= 0 || dropAmt[i] < member || img.rgba[i * 4 + 3] < 128) continue;
        const int32_t id = int32_t(compVal.size());
        std::vector<size_t> members;
        stack.assign(1, i);
        comp[i] = id;
        while (!stack.empty()) {
            size_t q = stack.back();
            stack.pop_back();
            members.push_back(q);
            const uint32_t qx = uint32_t(q % W), qy = uint32_t(q / W);
            const size_t nb[4] = {qx > 0 ? q - 1 : q, qx + 1 < W ? q + 1 : q, qy > 0 ? q - W : q, qy + 1 < H ? q + W : q};
            for (size_t r : nb)
                if (comp[r] < 0 && dropAmt[r] >= member && img.rgba[r * 4 + 3] >= 128) { comp[r] = id; stack.push_back(r); }
        }
        // one value per small group, keyed by its first texel in scan order (layout independent of labels)
        compVal.push_back(members.size() <= maxLeaf
                              ? dm::hashUnit(dm::hash3(uint32_t(i % W), uint32_t(i / W), ctx.seed ^ 0x1EAFu))
                              : -1.0);
    }
    const double soft = R.dropSoft;
    for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 0; x < W; ++x) {
            const size_t i = size_t(y) * W + x;
            if (dropAmt[i] <= 0) continue;
            uint8_t* p = &img.rgba[i * 4];
            double nz = dropNoise((x + 0.5) / W, (y + 0.5) / H, R.dropCells, ctx.seed ^ 0x1EAF);
            if (comp[i] >= 0 && compVal[size_t(comp[i])] >= 0) nz = 0.92 * compVal[size_t(comp[i])] + 0.08 * nz;
            // Hard decision: cut-out foliage is alpha tested, a half-transparent leaf would be a ghost.
            nz = soft + nz * (1 - 2 * soft);
            if (nz < dropAmt[i]) p[3] = 0;
        }
    return true;
}

bool RecipeBook::applyPalette(const Context& ctx, const Stage& st, tex::Image& colours, std::string* error) const {
    Resolved R;
    bool channels = false;
    if (!impl_->stageParams(ctx, st, R, channels, error)) return false;
    if (isIdentity(R)) return true;
    const size_t n = size_t(colours.width) * colours.height;
    for (size_t i = 0; i < n; ++i) {
        uint8_t* p = &colours.rgba[i * 4];
        if (channels) {
            for (int k = 0; k < 3; ++k) {
                double v = p[k] + R.off[k];
                p[k] = uint8_t(dm::clamp(v, 0, 255) + 0.5);
            }
            continue;
        }
        PixelOut o = transformColor(R, impl_->masks, p);
        p[0] = o.rgb[0]; p[1] = o.rgb[1]; p[2] = o.rgb[2];
    }
    return true;
}

std::vector<std::string> RecipeBook::profiles() const {
    std::vector<std::string> out;
    for (const auto& it : impl_->profiles) out.push_back(it.first);
    return out;
}

bool RecipeBook::hasProfile(const std::string& profile) const { return impl_->profiles.count(lower(profile)) != 0; }

bool RecipeBook::changesLook(const std::string& profile, Look l) const {
    auto it = impl_->profiles.find(lower(profile));
    if (it == impl_->profiles.end() || l == Look::Summer) return false;
    // Seed-independent: a look changes something iff its resolved form is not identity for seed 0.
    return !isIdentity(impl_->resolve(it->second.looks[int(l)], 0));
}

// ---------------------------------------------------------------------------------------------------
const char* toString(Look l) {
    switch (l) {
        case Look::Spring: return "spring";
        case Look::Summer: return "summer";
        case Look::LateSummer: return "late_summer";
        case Look::Autumn: return "autumn";
        case Look::Winter: return "winter";
    }
    return "?";
}

char toChar(Look l) {
    static const char c[5] = {'S', 'U', 'L', 'A', 'W'};
    return c[int(l) % 5];
}

bool lookFromString(const std::string& s0, Look& out) {
    std::string s = lower(trim(s0));
    static const struct { const char* n; Look l; } t[] = {
        {"s", Look::Spring}, {"spring", Look::Spring}, {"u", Look::Summer}, {"summer", Look::Summer},
        {"l", Look::LateSummer}, {"late_summer", Look::LateSummer}, {"late", Look::LateSummer},
        {"a", Look::Autumn}, {"autumn", Look::Autumn}, {"w", Look::Winter}, {"winter", Look::Winter}};
    for (auto& e : t) if (s == e.n) { out = e.l; return true; }
    return false;
}

void paletteThunk(void* binding, tex::Image& colours) {
    const PaletteBinding* b = static_cast<const PaletteBinding*>(binding);
    if (b && b->book) b->book->applyPalette(b->ctx, b->stage, colours);
}

uint32_t seedForAsset(const std::string& asset) { return qs::hashlittle(asset.data(), asset.size(), qs::kCdHashSeed); }

}  // namespace qs::recipe
