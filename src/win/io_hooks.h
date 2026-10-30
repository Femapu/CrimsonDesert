// QuattroStagioni - Win32 file I/O hooks that observe .paz reads and substitute verified ranges.
//
// Two sources of substitutions, both applied to completed reads:
//   * phase-2 manifest entries (setup.cpp, ArchiveTarget), passed to install();
//   * season packs (.qsp written by qs_gen), one per visual stage. Several packs are open at once and
//     each TEXTURE is served from the pack of its own stage (region calendar + per-asset jitter,
//     core/pack/multi.h); setSeasonDay() keeps the right packs open as the days pass. A manual
//     override (setActivePack, ini [Packs] Active=) serves one global stage instead. Changes affect
//     reads that complete after them.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include <memory>

#include "../core/pack/multi.h"
#include "../core/pack/plan.h"
#include "../core/season/season.h"
#include "setup.h"

namespace qs {
namespace io {

// Installs the hooks (MinHook on KernelBase exports, falling back to kernel32). Returns false and
// leaves everything unhooked on any failure.
bool install(const Config& cfg, const std::wstring& gameRoot, std::vector<ArchiveTarget>&& targets);

// Writes per-archive statistics to the log (called periodically and at shutdown).
void logSummary(const char* reason);

// Disables and removes all hooks (process shutdown).
void uninstall();

// ---- season packs --------------------------------------------------------------------------------
// Call once after a successful install(): remembers the game root and the packs folder
// (Config::packFolder, relative to `scriptsDir`) and, if Config::activePack is set, activates it as a
// manual override. Returns true if packs are usable (even with no pack open); a pack that fails
// verification is logged and not activated.
bool packsStart(const Config& cfg, const std::wstring& gameRoot, const std::wstring& scriptsDir);

// Season-driven serving (call from the season tick, every ~0.5 s, with the absolute day and the live
// config). Cheap when nothing changes. When the set of needed stages changes (planStages over the next
// few hours of game time, all regions and jitter) it opens/verifies/closes packs in the calling thread
// (pack index + cached pamt hashes), never in a hook. Then, whenever the day crosses the stage boundary
// of some texture, it publishes a new immutable per-texture assignment (only the textures whose
// boundary was crossed are re-evaluated). A texture whose stage has no pack is served by the nearest
// earlier stage that has one, else vanilla. While a manual override is active it only records the day.
// Thread safe (serialised with setActivePack).
void setSeasonDay(double day, const season::SeasonConfig& cfg);

// Manual override: loads, verifies and serves ONE global stage: a stage id ("A>W.2", "W"), a file name
// in the packs folder ("A-W.2.qsp") or an absolute path. "U" or "vanilla" forces vanilla textures.
// "" or "none" ends the override: back to per-texture season serving if setSeasonDay() has been called,
// pure pass-through otherwise. Loading happens in the calling thread (pack index, a few MB, and the
// referenced 0.pamt files), so call it from the menu or the tick, never from a hook. On failure the
// previous state stays. Thread safe.
bool setActivePack(const std::string& stageOrFile, std::string* why = nullptr);

// What the read hooks serve right now (per-texture stages or the manual override), for the GPU live
// swap (src/win/gpu/): an immutable snapshot, nullptr while nothing is published. Cheap (atomic copy).
std::shared_ptr<const pack::ServeSnapshot> serveSnapshot();

// Stage id of the manual override ("" if none: season-driven or nothing open).
std::string activePackStage();

// Stage ids of the packs open right now (calendar order; the override pack alone while forced).
std::vector<std::string> openPackStages();

// One open pack, for the menu.
struct PackStatus {
    std::string stageId;              // stage of the pack ("A>W.2")
    std::string file;                 // file name in the packs folder
    uint32_t formatVersion = 0;       // 1 = old pack without region/seed (its textures follow Temperate)
    uint32_t textures = 0;            // records (textures) in the pack
    uint32_t assigned = 0;            // textures currently served from this pack
    uint32_t assignedFallback = 0;    // of which stand in for a missing stage
    uint64_t patched = 0;             // texture substitutions done in game reads since the pack was opened
    uint64_t fallbackPatched = 0;     // of which as a stand-in for a missing stage
    uint64_t already = 0;             // reads that already held the payload
    uint64_t mismatch = 0;            // left untouched: bytes on disk are not the original ones
    uint64_t refused = 0;             // left untouched: unverifiable edge or payload error
    uint64_t payloadBytes = 0;        // payload size of the pack file
    uint64_t cacheBytes = 0;          // payload cache in use for this pack
};

// Everything the F5 menu shows about packs. A copy: cheap enough to call every frame.
struct PacksStatus {
    bool started = false;             // packsStart() succeeded
    bool seasonDriven = false;        // per-texture serving (setSeasonDay called, no override)
    bool forced = false;              // manual override active
    std::string forcedStage;          // override stage ("" with forced = vanilla override)
    double day = 0;                   // last day passed to setSeasonDay
    std::vector<PackStatus> open;     // open packs, calendar order
    uint32_t texturesIndexed = 0;     // distinct textures known by the open packs
    uint32_t texturesServed = 0;      // currently served by some pack
    uint32_t texturesFallback = 0;    // served by an earlier stage because their own pack is missing
    uint32_t texturesMissing = 0;     // their own stage has no pack (fallback or vanilla)
    std::vector<std::string> missingNow;  // stages wanted right now with no usable pack
    std::vector<std::string> available;   // stage ids of the *.qsp files in the packs folder
    std::vector<std::string> refused;     // "A>W.2: reason" for pack files that failed verification
    int planDays = 0;                 // window of `plan` (days from `day`)
    pack::StagePlan plan;             // stages needed for the next planDays days: present/missing, MB
    uint64_t cacheBytes = 0;          // payload cache in use, all packs
    uint64_t cacheLimitBytes = 0;     // [Packs] CacheMB, split between the open packs
    uint64_t indexBytes = 0;          // memory of the per-texture index
    uint64_t snapshotSwaps = 0;       // per-texture assignments published so far
    std::string lastError;            // last load/verification problem ("" if none)
};
PacksStatus packsStatus();

// Stages needed from the current day for `days` days (present / missing / estimated MB), for the
// menu. Uses the cached folder listing; no pack is opened. Missing-stage MB are estimated from the
// average size of the packs present (0 if none).
pack::StagePlan planStages(int days);

// File names of the *.qsp files in the packs folder.
std::vector<std::string> availablePacks();

// Closes every pack and the private archive handles (call before uninstall() at unload).
void packsStop();

}  // namespace io
}  // namespace qs
