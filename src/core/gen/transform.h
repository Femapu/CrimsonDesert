// QuattroStagioni - the "transform a texture" interface the generator pipeline is written against.
//
// The pipeline (fitter.h) only moves bytes: it decodes stored entries to plain DDS files, asks a
// TextureTransform for a TextureWork (the modified texture at a given strength), pulls encoded mips
// out of it with increasing fitBias until each fits its original stored size, and packs the result.
// Implementations:
//   * makeRecipeTransform (bind_recipes.cpp): tex:: codec + recipe::RecipeBook (the real seasons);
//   * makeTintTransform (tint_transform.cpp): BC1/BC3 endpoint tint without a codec, a smoke test
//     of the whole chain on real data before the recipes exist;
//   * tests provide their own.
// Implementations must be deterministic and safe to call from several threads at once.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "inventory.h"
#include "stage.h"

namespace qs::gen {

// One modified texture at a fixed strength. encodeMip may be called several times per mip with
// different fitBias values (0 = best quality .. 10 = most LZ4-friendly).
class TextureWork {
public:
    virtual ~TextureWork() = default;
    virtual size_t mipCount() const = 0;
    // Encoded (block compressed or raw) bytes of mip `mip`; size must equal the mip's raw size.
    virtual bool encodeMip(size_t mip, int fitBias, std::vector<uint8_t>& out, std::string* error) = 0;
};

class TextureTransform {
public:
    virtual ~TextureTransform() = default;
    virtual std::string name() const = 0;
    // Version of the rules (recipes), stored in the pack header.
    virtual uint32_t version() const { return 0; }
    // False -> the texture keeps its vanilla bytes for this stage (not written to the pack).
    virtual bool affects(const Item& item, const EffectiveStage& st) const = 0;
    // Builds the modified texture. `strength` in (0, 1] scales the effect towards vanilla (the fitter
    // lowers it when a mip cannot fit); `plainDds` is the decoded original DDS file (header + mips).
    virtual std::unique_ptr<TextureWork> begin(const Item& item, const EffectiveStage& st, float strength,
                                               const uint8_t* plainDds, size_t plainSize, std::string* error) const = 0;
};

// Endpoint tint for DXT1/DXT5 (no codec needed). Always available.
std::unique_ptr<TextureTransform> makeTintTransform();

// The real recipes (tex:: + recipe::). Returns nullptr with an explanation when this build does not
// contain the texture/recipe modules yet (see bind_recipes.cpp).
std::unique_ptr<TextureTransform> makeRecipeTransform(const std::string& recipeDir, std::string* error);
bool recipeTransformAvailable();

}  // namespace qs::gen
