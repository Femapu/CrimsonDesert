// QuattroStagioni - texture inventory (data/vegetation_classes_v1.json): which archive entries the
// generator transforms, with their class and seasonal profile. Paths and numbers only (no game data).
// The game index is the source of truth: every item is re-checked against the live pamt before use.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "../pamt.h"
#include "../season/season.h"

namespace qs::gen {

struct Item {
    std::string path, group;           // archive path, group ("0001", "dlc/dlc_01/0015")
    uint32_t paz = 0;
    uint64_t offset = 0;
    uint32_t stored = 0, size = 0;
    std::string fmt;                   // "DXT1", "DXT5", "rgb32bpp"
    uint32_t w = 0, h = 0, mips = 0;
    uint32_t mipStored[4] = {0, 0, 0, 0};
    std::string role, cls, profile, looks, region, asset;
    int tier = 0;
    uint32_t seed = 0;                 // lookup3(asset or path, 0xC5EDE)
    bool hasLook(char l) const { return looks.find(l) != std::string::npos; }
};

struct Inventory {
    int version = 0;
    std::string game;
    std::vector<Item> items;
};

// Loads the classes file. Items with missing mandatory fields are reported in `errors` and skipped.
bool loadInventory(const std::string& utf8Path, Inventory& out, std::vector<std::string>& errors);
bool parseInventory(const std::string& jsonText, Inventory& out, std::vector<std::string>& errors);

// Region whose calendar the texture follows (stored in pack v2 records). "shared_temperate",
// "north_cold", "swamp_jungle", "aquatic" map to their season::Region. "by_tile" (terrain tiles whose
// region depends on where the tile lies) and "world" (global maps) are Temperate for now.
// TODO: terrain tile -> region map (from the tile coordinates in the path) once the world layout is known.
season::Region regionOf(const Item& it);

// Compares an item with the live index record. Returns an empty string when they match.
std::string compareWithIndex(const Item& it, const PamtEntry& e);

}  // namespace qs::gen
