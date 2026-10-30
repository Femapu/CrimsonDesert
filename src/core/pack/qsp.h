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
};

struct Header {
    std::string stageId;        // VisualStage::id()
    std::string gameBuild;      // exe size/timestamp string
    std::vector<std::pair<std::string, uint32_t>> pamtHashes;  // group -> self hash
    uint32_t recipeVersion = 0;
};

// Writer/reader API is defined by the generator stream in this folder (qsp_io.h); the ASI must be
// able to read the index without loading payloads (payloads are read on demand, memory bounded).

}  // namespace qs::pack
