// QuattroStagioni - inventory verification against the user's game files (qs_gen --verify-inventory).
//
// For every inventory item: the entry must exist in the live <group>/0.pamt with the same paz index,
// offset, stored size and size; with readData the stored bytes are read and decoded strictly and the
// DDS header must match the inventory (format, width, height, mip count, the four mip stored sizes).
// Entries that a DLC group also contains are reported as "shadowed" (the game may read the DLC copy).
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "archive.h"
#include "inventory.h"

namespace qs::gen {

struct VerifyIssue {
    std::string group, path, kind, detail;  // kind: missing, index, decode, header, shadowed, group
};

struct VerifyReport {
    size_t checked = 0, ok = 0, missing = 0, indexMismatch = 0, decodeErrors = 0, headerMismatch = 0, shadowed = 0,
           groupErrors = 0;
    uint64_t bytesRead = 0;
    std::vector<VerifyIssue> issues;
    bool clean() const { return missing + indexMismatch + decodeErrors + headerMismatch + groupErrors == 0; }
};

VerifyReport verifyInventory(GameArchive& archive, const std::vector<Item>& items, bool readData,
                             const std::function<void(size_t done, size_t total)>& progress = nullptr);

}  // namespace qs::gen
