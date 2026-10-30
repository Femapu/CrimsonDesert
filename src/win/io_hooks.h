// QuattroStagioni - Win32 file I/O hooks that observe .paz reads and substitute verified ranges.
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

}  // namespace io
}  // namespace qs
