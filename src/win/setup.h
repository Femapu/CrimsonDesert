// QuattroStagioni - startup checks: game version guard and loading of verified substitutions.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../core/pack/serve.h"
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
    // [Packs] season packs written by qs_gen (see io_hooks.h, packsStart / setActivePack)
    std::string activePack;          // stage id ("A>W.2") or file name ("A-W.2.qsp"); empty = none
    std::wstring packFolder = L"QuattroStagioni\\packs";  // relative to bin64\scripts (or absolute)
    int packCacheMB = 64;            // payload cache bound
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

// Build id of <gameRoot>\bin64\CrimsonDesert.exe in the form stored by qs_gen (pack::gameBuildId).
std::string gameBuildIdOf(const std::wstring& gameRoot);

// Opens a .qsp pack and checks it against the installed game: header/index integrity, game build,
// self hash of every group's 0.pamt, size of every referenced .paz. Returns nullptr and sets `why`
// on any failure (the caller then serves nothing from it). Reads only; never throws.
std::shared_ptr<pack::PackServer> loadVerifiedPack(const std::wstring& gameRoot, const std::wstring& packPath,
                                                   size_t cacheBytes, std::string& why);

// Pack file for a stage id ("A>W.2" -> <dir>\A-W.2.qsp) or a file name / absolute path.
std::wstring resolvePackPath(const std::wstring& packsDir, const std::string& stageOrFile);

}  // namespace qs
