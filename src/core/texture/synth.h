// QuattroStagioni - synthetic test textures (our own procedural images, never game data).
//
// Used by the unit tests, by the fitBias measurements and by qs_preview to tune the recipes without
// game files in the repository. They imitate the structure of real foliage atlases: alpha-tested
// leaves on brown twigs, grass blades with a dark base and lighter tips, flower petals on green stems,
// opaque terrain colour. Deterministic for a given (kind, size, seed).
#pragma once
#include <cstdint>
#include <string>

#include "texture.h"

namespace qs::tex::synth {

enum class Kind { Leaves, Grass, Flowers, Terrain, Bark, Conifer, Climate };

bool kindFromString(const std::string& s, Kind& out);
const char* toString(Kind k);
// A sensible default recipe profile for a kind (e.g. Leaves -> "deciduous").
const char* defaultProfile(Kind k);

Image make(Kind k, uint32_t size, uint32_t seed = 1);

}  // namespace qs::tex::synth
