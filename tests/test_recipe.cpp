// QuattroStagioni - unit tests for the seasonal recipe engine (synthetic images only).
// Usage: test_recipe [data_dir]   (run from the repo root: loads data/recipes)
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../src/core/lookup3.h"
#include "../src/core/recipe/recipe.h"
#include "../src/core/texture/synth.h"

using namespace qs::recipe;
using qs::tex::Image;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond)                                                                         \
    do {                                                                                    \
        if (cond) ++g_pass;                                                                 \
        else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); }       \
    } while (0)

static Context ctxFor(const std::string& profile, uint32_t seed) {
    Context c;
    c.profile = profile;
    c.seed = seed;
    return c;
}

static double meanAbsDiff(const Image& a, const Image& b) {
    double s = 0;
    size_t n = 0;
    for (size_t i = 0; i < a.rgba.size(); i += 4) {
        if (a.rgba[i + 3] == 0) continue;
        for (int k = 0; k < 3; ++k) s += std::fabs(double(a.rgba[i + k]) - b.rgba[i + k]);
        n += 3;
    }
    return n ? s / double(n) : 0;
}

static size_t opaqueCount(const Image& img) {
    size_t c = 0;
    for (size_t i = 3; i < img.rgba.size(); i += 4) c += img.rgba[i] >= 128;
    return c;
}

// ---------------------------------------------------------------------------------------------
static void testShippedRecipes(const RecipeBook& book) {
    static const char* kProfiles[] = {"deciduous", "conifer", "evergreen", "grass", "flower", "crop_cereal", "crop_veg",
                                      "shrub", "vine", "wetland", "bark", "ground_grass", "ground_decal", "ground_soil",
                                      "moss_surface", "leaf_litter", "terrain_color", "global_colormap", "climate_temperature"};
    for (const char* p : kProfiles) {
        CHECK(book.hasProfile(p));
        CHECK(!book.changesLook(p, Look::Summer));
    }
    CHECK(book.changesLook("deciduous", Look::Autumn));
    CHECK(book.changesLook("deciduous", Look::Winter));
    CHECK(!book.changesLook("conifer", Look::Spring));
    CHECK(book.changesLook("conifer", Look::Winter));

    Image leaves = qs::tex::synth::make(qs::tex::synth::Kind::Leaves, 128, 1);
    // Summer is identity, for every profile.
    for (const char* p : kProfiles) {
        Image img = leaves;
        CHECK(!book.affects(ctxFor(p, 7), pureStage(Look::Summer)));
        CHECK(book.apply(ctxFor(p, 7), pureStage(Look::Summer), img));
        CHECK(img.rgba == leaves.rgba);
    }
    // Looks a profile does not define are identity too.
    CHECK(!book.affects(ctxFor("conifer", 1), pureStage(Look::Autumn)));
    CHECK(!book.affects(ctxFor("conifer", 1), Stage{Look::Summer, Look::LateSummer, 0.5f}));
    CHECK(book.affects(ctxFor("conifer", 1), Stage{Look::Autumn, Look::Winter, 0.5f}));
    // Unknown profile.
    Image img = leaves;
    std::string err;
    CHECK(!book.affects(ctxFor("no_such_profile", 1), pureStage(Look::Autumn)));
    CHECK(!book.apply(ctxFor("no_such_profile", 1), pureStage(Look::Autumn), img, &err));
    CHECK(!err.empty());

    // Determinism: same input, same bytes (golden hash printed for cross-compiler checks).
    Image a = leaves, b = leaves;
    book.apply(ctxFor("deciduous", 3), pureStage(Look::Autumn), a);
    book.apply(ctxFor("deciduous", 3), pureStage(Look::Autumn), b);
    CHECK(a.rgba == b.rgba);
    CHECK(a.rgba != leaves.rgba);
    std::printf("golden: deciduous A seed 3 %08x\n", qs::hashlittle(a.rgba.data(), a.rgba.size(), 0));
    // invisible texels are never touched
    bool same = true;
    for (size_t i = 0; i < a.rgba.size(); i += 4)
        if (leaves.rgba[i + 3] == 0 && std::memcmp(&a.rgba[i], &leaves.rgba[i], 4) != 0) same = false;
    CHECK(same);

    // Seeds pick different autumn palettes somewhere among a few assets.
    std::vector<uint32_t> hashes;
    for (uint32_t s = 1; s <= 6; ++s) {
        Image c = leaves;
        book.apply(ctxFor("deciduous", s), pureStage(Look::Autumn), c);
        hashes.push_back(qs::hashlittle(c.rgba.data(), c.rgba.size(), 0));
    }
    bool differ = false;
    for (size_t i = 1; i < hashes.size(); ++i) differ |= hashes[i] != hashes[0];
    CHECK(differ);
}

// ---------------------------------------------------------------------------------------------
static void testBlends(const RecipeBook& book) {
    Image leaves = qs::tex::synth::make(qs::tex::synth::Kind::Leaves, 128, 2);
    // U -> A: distance from vanilla grows monotonically with t.
    double prev = -1;
    bool mono = true;
    for (int k = 0; k <= 8; ++k) {
        Image img = leaves;
        book.apply(ctxFor("deciduous", 5), Stage{Look::Summer, Look::Autumn, float(k) / 8}, img);
        double d = meanAbsDiff(leaves, img);
        if (d + 1e-9 < prev) mono = false;
        prev = d;
    }
    CHECK(mono);
    CHECK(prev > 10);
    // t = 0 and t = 1 equal the pure looks.
    Image p0 = leaves, p1 = leaves, s0 = leaves, s1 = leaves;
    book.apply(ctxFor("deciduous", 5), Stage{Look::Autumn, Look::Winter, 0.0f}, s0);
    book.apply(ctxFor("deciduous", 5), pureStage(Look::Autumn), p0);
    book.apply(ctxFor("deciduous", 5), Stage{Look::Autumn, Look::Winter, 1.0f}, s1);
    book.apply(ctxFor("deciduous", 5), pureStage(Look::Winter), p1);
    CHECK(s0.rgba == p0.rgba);
    CHECK(s1.rgba == p1.rgba);
    // A -> W: leaves fall monotonically, a fallen texel never comes back, branches stay.
    std::vector<uint8_t> prevAlpha;
    size_t prevCount = opaqueCount(leaves);
    bool fewer = true, subset = true;
    for (int k = 0; k <= 8; ++k) {
        Image img = leaves;
        book.apply(ctxFor("deciduous", 5), Stage{Look::Autumn, Look::Winter, float(k) / 8}, img);
        size_t c = opaqueCount(img);
        if (c > prevCount) fewer = false;
        prevCount = c;
        std::vector<uint8_t> al;
        for (size_t i = 3; i < img.rgba.size(); i += 4) al.push_back(img.rgba[i] >= 128);
        if (!prevAlpha.empty())
            for (size_t i = 0; i < al.size(); ++i) if (al[i] && !prevAlpha[i]) subset = false;
        prevAlpha = al;
    }
    CHECK(fewer);
    CHECK(subset);
    Image w = leaves;
    book.apply(ctxFor("deciduous", 5), pureStage(Look::Winter), w);
    size_t greenBefore = 0, greenAfter = 0, brownBefore = 0, brownAfter = 0;
    for (size_t i = 0; i < leaves.rgba.size(); i += 4) {
        const uint8_t* p = &leaves.rgba[i];
        if (p[3] < 128) continue;
        bool green = p[1] > p[0] + 15, brown = p[0] > p[1] && p[1] > p[2];
        if (green) { ++greenBefore; greenAfter += w.rgba[i + 3] >= 128; }
        if (brown) { ++brownBefore; brownAfter += w.rgba[i + 3] >= 128; }
    }
    std::printf("winter: leaf texels kept %zu/%zu, branch texels kept %zu/%zu\n", greenAfter, greenBefore, brownAfter, brownBefore);
    CHECK(greenAfter * 5 < greenBefore);       // bare
    CHECK(brownAfter * 10 >= brownBefore * 9);  // branches stay
    // An opaque texture (no cut-outs) never gets holes.
    Image terrain = qs::tex::synth::make(qs::tex::synth::Kind::Terrain, 64, 1);
    book.apply(ctxFor("deciduous", 5), pureStage(Look::Winter), terrain);
    CHECK(opaqueCount(terrain) == 64u * 64u);
}

// ---------------------------------------------------------------------------------------------
static void testPaletteAndChannels(const RecipeBook& book) {
    // applyPalette == apply for opaque pixels (pointwise colour part).
    Image t = qs::tex::synth::make(qs::tex::synth::Kind::Terrain, 32, 4);
    Image a = t;
    book.apply(ctxFor("grass", 9), pureStage(Look::LateSummer), a);
    Image row;
    row.width = 32 * 32;
    row.height = 1;
    row.rgba = t.rgba;
    book.applyPalette(ctxFor("grass", 9), pureStage(Look::LateSummer), row);
    CHECK(row.rgba == a.rgba);
    // PaletteBinding glue
    PaletteBinding pb;
    pb.book = &book;
    pb.ctx = ctxFor("grass", 9);
    pb.stage = pureStage(Look::LateSummer);
    Image row2 = t;
    row2.width = 32 * 32;
    row2.height = 1;
    paletteThunk(&pb, row2);
    CHECK(row2.rgba == a.rgba);
    // climate: R offset only
    Image c = qs::tex::synth::make(qs::tex::synth::Kind::Climate, 32, 1), c2 = c;
    book.apply(ctxFor("climate_temperature", 1), pureStage(Look::Winter), c2);
    bool gbSame = true, rLower = true;
    for (size_t i = 0; i < c.rgba.size(); i += 4) {
        if (c.rgba[i + 1] != c2.rgba[i + 1] || c.rgba[i + 2] != c2.rgba[i + 2] || c.rgba[i + 3] != c2.rgba[i + 3]) gbSame = false;
        if (c2.rgba[i] > c.rgba[i] || (c.rgba[i] >= 45 && c2.rgba[i] != c.rgba[i] - 45)) rLower = false;
    }
    CHECK(gbSame);
    CHECK(rLower);
    // helpers
    Look l;
    CHECK(lookFromString("A", l) && l == Look::Autumn);
    CHECK(lookFromString("winter", l) && l == Look::Winter);
    CHECK(!lookFromString("x", l));
    CHECK(toChar(Look::LateSummer) == 'L');
    CHECK(seedForAsset("climate_texture_1") == qs::hashlittle("climate_texture_1", 17, 0xC5EDE));
}

// ---------------------------------------------------------------------------------------------
static void testParsing() {
    const std::string base = "[palette]\nred = 30, 1.5, 0.9\n";
    struct Case { const char* text; const char* expect; };
    const Case bad[] = {
        {"[p.A]\ngreen.hue_amout = 0.5\n", "unknown key"},
        {"[p.A]\ngreen.chroma = abc\n", "bad number"},
        {"[p.A]\npalette = blue\n", "unknown palette"},
        {"[p.U]\ngreen.chroma = 1.1\n", "summer"},
        {"[p.X]\ngreen.chroma = 1.1\n", "look must be"},
        {"[p.A]\ngreen.chroma = 1\n[p.A]\ngreen.chroma = 2\n", "duplicate section"},
        {"green.chroma = 1\n", "outside of a section"},
        {"[q]\ninherit = nothing\n", "inherits unknown"},
        {"[p.A\n", "malformed"},
        {"[p.A]\njust text\n", "expected key"},
        {"[p.A]\ngreen.hue_amount = 1.5\n", "[0, 1]"},
        {"[a]\ninherit = b\n[b]\ninherit = a\n[a.W]\nfrost = 0.1\n", "too deep"},
    };
    for (const Case& c : bad) {
        RecipeBook b;
        std::vector<std::string> errs;
        const bool standalone = std::strncmp(c.text, "green.chroma = 1\n", 17) == 0 && c.text[17] == 0;
        bool ok = b.loadText(standalone ? std::string(c.text) : base + c.text, errs);
        bool found = false;
        for (auto& e : errs) found |= e.find(c.expect) != std::string::npos;
        if (ok || !found) std::printf("  parse case did not fail as expected: %s\n", c.text);
        CHECK(!ok);
        CHECK(found);
    }
    // error messages carry the line number
    {
        RecipeBook b;
        std::vector<std::string> errs;
        b.loadText(base + "[p.A]\n\n\ngreen.bogus = 1\n", errs);
        CHECK(!errs.empty() && errs[0].find(":6:") != std::string::npos);
    }
    // a valid file with comments, inheritance and strength
    RecipeBook b;
    std::vector<std::string> errs;
    const std::string good = base +
        "; comment\n[P]\ndescription = x\n[p.A]  # inline\npalette = red:2, red:1\ngreen.hue_amount = 0.8 ; inline\n"
        "[child]\ninherit = p\nstrength = 0\n[half]\ninherit = p\nstrength = 0.5\n";
    CHECK(b.loadText(good, errs));
    for (auto& e : errs) std::printf("  unexpected: %s\n", e.c_str());
    CHECK(b.changesLook("p", Look::Autumn));
    CHECK(!b.changesLook("child", Look::Autumn));  // strength 0 = identity
    CHECK(b.changesLook("half", Look::Autumn));
    Image leaves = qs::tex::synth::make(qs::tex::synth::Kind::Leaves, 64, 1), full = leaves, half = leaves;
    b.apply(ctxFor("p", 1), pureStage(Look::Autumn), full);
    b.apply(ctxFor("half", 1), pureStage(Look::Autumn), half);
    double dFull = meanAbsDiff(leaves, full), dHalf = meanAbsDiff(leaves, half);
    CHECK(dHalf > 0.2 * dFull && dHalf < 0.8 * dFull);
}

int main(int argc, char** argv) {
    RecipeBook book;
    std::vector<std::string> errs;
    bool ok = book.load("data/recipes", errs);
    for (auto& e : errs) std::printf("recipe error: %s\n", e.c_str());
    CHECK(ok);
    if (ok) {
        testShippedRecipes(book);
        testBlends(book);
        testPaletteAndChannels(book);
    }
    testParsing();
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
