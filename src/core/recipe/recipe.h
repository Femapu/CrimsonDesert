// QuattroStagioni - seasonal recipes: deterministic image transforms per texture class.
//
// Contract (docs/dev/CONTRACTS.md, owner: recipes stream). A recipe never uses game pixels from other
// files; it only transforms the texture it is given, driven by parameters loaded from
// data/recipes/*.ini (shipped with the mod, our own data). Looks: S spring, U summer (vanilla, identity),
// L late dry summer, A autumn, W winter.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "../texture/texture.h"

namespace qs::recipe {

enum class Look : uint8_t { Spring = 0, Summer = 1, LateSummer = 2, Autumn = 3, Winter = 4 };
const char* toString(Look l);
char toChar(Look l);  // 'S','U','L','A','W'

// What the generator knows about a texture (from data/vegetation_classes_v1.json).
struct Context {
    std::string path;     // archive path (seed source)
    std::string cls;      // class, e.g. "broadleaf", "impostor_grass", "terrain_color_tile"
    std::string profile;  // seasonal behaviour profile, e.g. "deciduous", "grass", "flower" (see CONTRACTS)
    std::string role;     // "albedo", "subsurface", ...
    std::string region;   // "shared_temperate", "north_cold", ...
    uint32_t seed = 0;    // lookup3(path) - per asset variety
};

// A point on the seasonal path: blend `t` (0..1) from look `from` to look `to`.
struct Stage {
    Look from = Look::Summer, to = Look::Summer;
    float t = 0.0f;
};

class RecipeBook {
public:
    // Loads every *.ini in `dir`. Returns false and fills `errors` on malformed files.
    bool load(const std::string& dir, std::vector<std::string>& errors);
    // Loads from an in-memory ini text (tests).
    bool loadText(const std::string& text, std::vector<std::string>& errors);

    // True if this context is changed at all by the given stage (false -> keep vanilla bytes).
    bool affects(const Context& ctx, const Stage& st) const;

    // Applies the stage to mip 0 of `tex` in place (caller rebuilds the other mips).
    // Must be deterministic and must not change dimensions.
    bool apply(const Context& ctx, const Stage& st, tex::Image& img, std::string* error = nullptr) const;

    // ---- additions (texture + recipes stream) ----

    // Colour-only, strictly pointwise version of apply() (no alpha changes, no spatial effects), for
    // the recolour fallback of tex::fitMip(): `colours` is a 1-row image of endpoint colours.
    bool applyPalette(const Context& ctx, const Stage& st, tex::Image& colours, std::string* error = nullptr) const;

    // Profiles defined by the loaded files, and whether a profile changes a given look at all.
    std::vector<std::string> profiles() const;
    bool hasProfile(const std::string& profile) const;
    bool changesLook(const std::string& profile, Look l) const;

    struct Impl;
    RecipeBook();
    ~RecipeBook();
    RecipeBook(const RecipeBook&) = delete;
    RecipeBook& operator=(const RecipeBook&) = delete;

private:
    Impl* impl_;
};

// A pure look as a stage (t = 0).
inline Stage pureStage(Look l) { return Stage{l, l, 0.0f}; }

// Glue for tex::fitMip()'s recolour fallback:
//   PaletteBinding pb{&book, ctx, stage};
//   tex::RecolorFallback rf{&paletteThunk, &pb};
//   tex::fitMip(texture, mip, target, original, size, opts, &rf);
struct PaletteBinding {
    const RecipeBook* book = nullptr;
    Context ctx;
    Stage stage;
};
void paletteThunk(void* binding, tex::Image& colours);

// Seed for a texture as defined in docs/dev/CONTRACTS.md: lookup3(asset, 0xC5EDE).
uint32_t seedForAsset(const std::string& asset);

// Parses "S", "U", "L", "A", "W" (case-insensitive) or full names ("spring", "autumn"...).
bool lookFromString(const std::string& s, Look& out);

}  // namespace qs::recipe
