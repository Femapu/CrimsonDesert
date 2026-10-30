// QuattroStagioni - Win32 file I/O hooks that observe .paz reads and substitute verified ranges.
//
// Two sources of substitutions, both applied to completed reads:
//   * phase-2 manifest entries (setup.cpp, ArchiveTarget), passed to install();
//   * the active season pack (.qsp written by qs_gen), served by core/pack/serve.h. Packs can be
//     switched at runtime; a switch affects reads that complete after it.
#pragma once
#include <string>
#include <vector>

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
// (Config::packFolder, relative to `scriptsDir`) and activates Config::activePack if set.
// Returns true if packs are usable (even with no active pack); a pack that fails verification is
// logged and not activated.
bool packsStart(const Config& cfg, const std::wstring& gameRoot, const std::wstring& scriptsDir);

// Loads, verifies and activates a pack: a stage id ("A>W.2", "W") or a file name in the packs folder
// ("A-W.2.qsp") or an absolute path. "" or "none" deactivates (pure pass-through). Loading happens in
// the calling thread (reads the pack index, a few MB, and the referenced 0.pamt files), so call it from
// the season tick or the menu, never from a hook. On failure the previously active pack stays active.
// Thread safe.
bool setActivePack(const std::string& stageOrFile, std::string* why = nullptr);

// Stage id of the active pack ("" if none).
std::string activePackStage();

// File names of the *.qsp files in the packs folder.
std::vector<std::string> availablePacks();

// Deactivates the pack and closes private archive handles (call before uninstall() at unload).
void packsStop();

}  // namespace io
}  // namespace qs
