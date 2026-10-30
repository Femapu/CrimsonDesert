// QuattroStagioni - in-process view of the game's main module for the probes.
//
// Builds a qs::probe::ImageView over the mapped exe (GetModuleHandle(nullptr)), with every section
// checked by VirtualQuery. Obfuscated (protector) sections are marked inaccessible on purpose:
// probes never read, scan or patch them (recon C: Denuvo VM and relocated trigger functions).
#pragma once
#include <windows.h>

#include <string>
#include <vector>

#include "../../core/probe/pattern.h"
#include "../../core/probe/pe_view.h"

namespace qs::probe {

struct ModuleInfo {
    HMODULE handle = nullptr;
    uintptr_t base = 0;
    ImageView view;
    std::wstring path;
};

// Main exe by default. Returns false (with *err) if the headers cannot be parsed.
bool describeModule(HMODULE module, ModuleInfo& out, std::string* err);

// Scans the primary code section (or, with inData, every accessible non-executable section).
// Returns RVAs of all matches (capped at maxHits).
std::vector<uint64_t> scanModule(const ModuleInfo& m, const Pattern& p, bool inData, size_t maxHits = 64);

}  // namespace qs::probe
