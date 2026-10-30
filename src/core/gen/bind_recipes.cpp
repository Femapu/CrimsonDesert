// QuattroStagioni - binding of the generator pipeline to the real texture codec and recipes.
//
// This is the only generator file that uses tex:: (texture/texture.h) and recipe:: (recipe/recipe.h).
// Those modules are written by another stream; until their .cpp files exist this file compiles to a
// stub (makeRecipeTransform returns nullptr, qs_gen explains and offers --transform tint) so every
// binary still links. The real binding switches on automatically when both
//   src/core/texture/texture.cpp   and   src/core/recipe/recipe.cpp
// exist. If the other stream uses different file names, add -DQS_HAVE_RECIPES to EXTRA_CORE_FLAGS in
// build.sh (one line); -DQS_NO_RECIPES forces the stub.
//
// Strength (fitter fallback): 1 = the stage as designed. Below 1 a plateau X becomes the stage
// U -> X at t = strength (interpolation in recipe parameter space, as for calendar transitions); a
// transition stage is applied in full and then blended towards the vanilla pixels by `strength`.
#include "transform.h"

#if !defined(QS_NO_RECIPES) && (defined(QS_HAVE_RECIPES) || \
    (defined(__has_include) && __has_include("../texture/texture.cpp") && __has_include("../recipe/recipe.cpp")))
#define QS_BIND_RECIPES 1
#endif

#ifdef QS_BIND_RECIPES

#include <algorithm>

#include "../recipe/recipe.h"
#include "../texture/texture.h"

namespace qs::gen {
namespace {

recipe::Look toLook(char c) {
    switch (c) {
        case 'S': return recipe::Look::Spring;
        case 'L': return recipe::Look::LateSummer;
        case 'A': return recipe::Look::Autumn;
        case 'W': return recipe::Look::Winter;
        default: return recipe::Look::Summer;
    }
}

recipe::Context contextFor(const Item& it) {
    recipe::Context c;
    c.path = it.path;
    c.cls = it.cls;
    c.profile = it.profile;
    c.role = it.role;
    c.region = it.region;
    c.seed = it.seed;  // lookup3(asset): textures of one asset share the seed (CONTRACTS.md)
    return c;
}

recipe::Stage toStage(const EffectiveStage& st) {
    recipe::Stage s;
    s.from = toLook(st.from);
    s.to = toLook(st.to);
    s.t = st.t;
    return s;
}

class RecipeWork : public TextureWork {
public:
    explicit RecipeWork(tex::Texture t) : t_(std::move(t)) {}
    size_t mipCount() const override { return t_.mips.size(); }
    bool encodeMip(size_t mip, int fitBias, std::vector<uint8_t>& out, std::string* error) override {
        tex::EncodeOptions o;
        o.fitBias = fitBias;
        return tex::encodeMip(t_, mip, o, out, error);
    }

private:
    tex::Texture t_;
};

class RecipeTransform : public TextureTransform {
public:
    bool load(const std::string& dir, std::string* error) {
        std::vector<std::string> errs;
        bool ok = book_.load(dir, errs);
        if (!ok && error) {
            *error = "recipes in " + dir + ":";
            for (const auto& e : errs) *error += " " + e + ";";
        }
        return ok;
    }
    std::string name() const override { return "recipes"; }
    uint32_t version() const override { return 1; }
    bool affects(const Item& item, const EffectiveStage& st) const override {
        return !st.vanilla() && book_.affects(contextFor(item), toStage(st));
    }
    std::unique_ptr<TextureWork> begin(const Item& item, const EffectiveStage& st, float strength, const uint8_t* dds,
                                       size_t n, std::string* error) const override {
        tex::Texture t;
        if (!tex::decodeDds(dds, n, t, error)) return nullptr;
        if (t.mips.empty()) { if (error) *error = "texture has no mips"; return nullptr; }
        strength = std::min(std::max(strength, 0.0f), 1.0f);
        recipe::Stage s = toStage(st);
        bool blend = false;
        if (strength < 1.0f) {
            if (st.from == st.to) {  // plateau X -> U..X at t = strength
                s.from = recipe::Look::Summer;
                s.to = toLook(st.to);
                s.t = strength;
            } else {
                blend = true;
            }
        }
        std::vector<uint8_t> orig;
        if (blend) orig = t.mips[0].rgba;
        if (!book_.apply(contextFor(item), s, t.mips[0], error)) return nullptr;
        if (blend && orig.size() == t.mips[0].rgba.size()) {
            for (size_t i = 0; i < orig.size(); ++i) {
                float v = float(orig[i]) + (float(t.mips[0].rgba[i]) - float(orig[i])) * strength;
                t.mips[0].rgba[i] = uint8_t(std::min(255.0f, std::max(0.0f, v + 0.5f)));
            }
        }
        const bool srgb = item.role != "data";
        tex::rebuildMips(t, srgb);
        return std::unique_ptr<TextureWork>(new RecipeWork(std::move(t)));
    }

private:
    recipe::RecipeBook book_;
};

}  // namespace

std::unique_ptr<TextureTransform> makeRecipeTransform(const std::string& recipeDir, std::string* error) {
    std::unique_ptr<RecipeTransform> t(new RecipeTransform());
    if (!t->load(recipeDir, error)) return nullptr;
    return std::unique_ptr<TextureTransform>(t.release());
}

bool recipeTransformAvailable() { return true; }

}  // namespace qs::gen

#else  // stub: texture/recipe modules not built yet

namespace qs::gen {

std::unique_ptr<TextureTransform> makeRecipeTransform(const std::string&, std::string* error) {
    if (error)
        *error = "this build does not contain the texture codec and recipes yet (src/core/texture, src/core/recipe); "
                 "use --transform tint for a pipeline test";
    return nullptr;
}

bool recipeTransformAvailable() { return false; }

}  // namespace qs::gen

#endif
