// QuattroStagioni - startup checks: game version guard and loading of verified substitutions.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../core/range_patch.h"

namespace qs {

struct Config {
    bool enabled = true;
    bool substitute = true;          // false = observe only (hooks + log, bytes untouched)
    int logLevel = 1;
    int maxReadLogLines = 3000;      // cap for per-read debug lines
    bool waitForTargetReads = true;  // complete async reads touching a target inside the ReadFile hook
    int summaryIntervalSeconds = 30;
    // [Debug] (test harness only)
    bool testMode = false;
    std::wstring gameRootOverride;
};

// One .paz archive that contains at least one substitution.
struct ArchiveTarget {
    std::string key;                 // "0012/3.paz" (relative to the game root, lower case, '/')
    std::wstring fullPathLower;      // canonical lower-case path, without the \\?\ prefix
    std::vector<Substitution> subs;
};

struct SetupResult {
    bool versionOk = false;
    std::string versionReport;
    std::wstring gameRoot;           // folder that contains bin64, meta, 0000...
    std::vector<ArchiveTarget> targets;
};

Config loadConfig(const std::wstring& iniPath);

// Runs every check. Never throws; any failure leaves `targets` empty or drops the failing entry.
SetupResult runSetup(const Config& cfg, const std::wstring& scriptsDir);

}  // namespace qs
