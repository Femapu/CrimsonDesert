// QuattroStagioni - substitution manifest (bin64\scripts\QuattroStagioni\manifest.ini).
//
// Each [entry.<id>] section describes one archive entry to replace and the data file holding the
// replacement stored bytes. Every number is cross-checked at startup against the live game files;
// any difference disables that entry (never the game).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "ini.h"

namespace qs {

struct ManifestEntry {
    std::string id;            // section suffix
    bool enabled = true;
    std::string path;          // archive path, e.g. ui/texture/cd_loading_00.dds
    std::string group;         // "0012" (base game group folder)
    uint32_t pamtHash = 0;     // expected self hash of <group>/0.pamt
    uint32_t pazIndex = 0;
    uint64_t pazSize = 0;
    uint64_t offset = 0;
    uint32_t storedSize = 0;
    uint32_t size = 0;
    uint32_t flags = 0;
    uint32_t origHash = 0;     // lookup3(original stored bytes, 0xC5EDE)
    uint32_t origHash0 = 0;    // lookup3(original stored bytes, 0)
    std::string dataFile;      // relative to the manifest folder
    uint32_t dataHash = 0;     // lookup3(replacement, 0xC5EDE)
    uint32_t dataHash0 = 0;    // lookup3(replacement, 0)
    std::string description;
};

struct Manifest {
    std::string gameVersion;
    std::vector<ManifestEntry> entries;
};

// Parses the manifest. Malformed entries are skipped and reported in `errors`.
Manifest parseManifest(const Ini& ini, std::vector<std::string>& errors);

}  // namespace qs
