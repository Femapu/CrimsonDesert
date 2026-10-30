// QuattroStagioni - season pack (.qsp): replacement stored bytes for one visual stage.
//
// Contract (docs/dev/CONTRACTS.md, owner: generator stream). Written by qs_gen on the user's PC,
// read by the ASI (io_hooks). Contains only data derived on that PC; never shipped with the mod.
// Every record keeps the exact original stored size. The pack header binds it to the game build
// (pamt self hashes) so a patched game ignores stale packs.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace qs::pack {

struct Record {
    std::string group;          // "0001", "dlc/dlc_01/0015"
    std::string path;           // archive path
    uint32_t pazIndex = 0;
    uint64_t offset = 0;        // inside the paz
    uint32_t storedSize = 0;    // == original == replacement length
    uint32_t origHash = 0;      // lookup3(original stored bytes, 0xC5EDE)
    uint32_t dataHash = 0;      // lookup3(replacement, 0xC5EDE)
    uint64_t dataOffset = 0;    // inside the .qsp file
    // Added by the generator stream (qsp_io.h):
    uint32_t size = 0;          // decoded entry size (pamt), informational
    uint8_t flags = 0;          // pamt flags (compression | crypto << 4), informational
    uint32_t pazSlot = 0;       // index into PackReader::paz()
    uint32_t chunkFirst = 0;    // first original-chunk hash of this record (see qsp_io.h)
    // Added for per-texture stages (format v2, multi.h):
    uint8_t region = 0;         // season::Region of the texture (Temperate when unknown)
    uint32_t assetSeed = 0;     // lookup3(asset, 0xC5EDE): same seed for every texture of one asset
    bool hasMeta = false;       // region/assetSeed were read from a v2 pack (false for v1 packs)
};

struct Header {
    std::string stageId;        // VisualStage::id()
    std::string gameBuild;      // exe size/timestamp string
    std::vector<std::pair<std::string, uint32_t>> pamtHashes;  // group -> self hash
    uint32_t recipeVersion = 0;
    // Added by the generator stream (qsp_io.h):
    std::string generator;      // qs_gen version and transform name
    // Added for per-texture stages (format v2):
    uint32_t formatVersion = 0; // file format version read from the pack (1 or 2)
    uint32_t transitionSteps = 0;  // transition steps the stage id refers to (0 = unknown, v1)
};

// Writer/reader API: qsp_io.h (file format), serve.h (substitution of game reads from a pack).

}  // namespace qs::pack
