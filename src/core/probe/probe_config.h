// QuattroStagioni - probes.ini parsing (portable, unit tested).
//
// The runtime probe framework (src/win/probe/) is driven entirely by
// bin64\scripts\QuattroStagioni\probes.ini so a session on the game PC can iterate WITHOUT
// recompiling: edit the ini, relaunch the game, read QuattroStagioni\probe.log.
// See dist/bin64/scripts/QuattroStagioni/probes.ini for the documented key list and
// docs/recon/probe_plan.md for the plan that uses it.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "../ini.h"
#include "diff.h"
#include "pattern.h"

namespace qs::probe {

enum class InstanceMode : uint8_t { None, Capture, Scan, Global };
enum class HookKind : uint8_t { Vtable, MinHook };
const char* toString(InstanceMode m);

struct WatchField {
    uint32_t offset = 0;
    FieldType type = FieldType::F32;
    std::string name;  // optional label for the log
};

struct ProbeSpec {
    std::string name;  // section name without "probe."
    bool enabled = true;

    // anchor (exactly one of: className, listGlob, signature, rva)
    std::string className;   // "pa::X" or ".?AVX@pa@@"
    std::string listGlob;    // ListClasses=*Weather*: log matching classes only
    std::string signatureText;
    Pattern signature;
    bool sigInData = false;  // SigSection=rdata|data: scan non-executable sections instead of code
    int64_t sigOffset = 0;
    RipSpec resolve;
    bool allowMultiple = false;  // accept several matches if they all resolve to the same address
    uint64_t rva = 0;            // Rva=0x... (build specific, only honoured with ExpectTimestamp)

    // static logging
    bool logVtable = false;
    uint32_t maxSlots = 64;
    bool logHierarchy = false;

    // instances
    InstanceMode instance = InstanceMode::None;
    int slot = -1;                     // vtable slot index for capture
    HookKind hook = HookKind::Vtable;
    bool captureRdx = false;           // CaptureArg=rdx (second argument instead of `this`)
    bool logCallers = true;
    std::vector<int64_t> chain;        // Global: p = *(p + chain[i]) for each element
    uint32_t scanMaxMB = 65536;        // per full pass
    uint32_t scanSliceMs = 150;        // B4: scan in time slices so other probes keep sampling
    uint32_t scanRegionMaxMB = 1024;   // B4: skip huge regions (GPU/streaming pools), 0 = no limit
    uint32_t rescanSec = 0;            // 0 = scan once (and again while nothing found)
    uint32_t maxInstances = 4;
    bool checkVtable = true;           // drop instances whose vtable pointer changes

    // per-instance actions
    uint32_t dumpOffset = 0, dumpSize = 0;
    std::vector<WatchField> watches;
    double epsilon = 1e-4;
    uint32_t intervalMs = 0;           // 0 = [General] IntervalMs
    uint32_t durationSec = 0;          // 0 = until probeStop
    bool stats = false;
    std::vector<std::pair<float, float>> statsRanges;  // default {0,24},{0,1}
};

struct ProbeConfig {
    bool enabled = false;
    uint32_t startDelayMs = 20000;
    uint32_t intervalMs = 500;
    bool allowHooks = false;     // master switch: without it capture probes are skipped
    uint32_t maxLogMB = 64;
    uint32_t summarySec = 30;
    bool logSections = true;
    uint32_t expectTimestamp = 0;  // PE TimeDateStamp that Rva= probes were written for
    std::vector<ProbeSpec> probes;
};

// Parses an already loaded Ini. Invalid probes are dropped with a message in `errors` (the rest
// still run). Returns false only if [General] is missing.
bool parseProbeConfig(const Ini& ini, ProbeConfig& out, std::vector<std::string>& errors);
bool parseProbeConfigText(const std::string& text, ProbeConfig& out, std::vector<std::string>& errors);

// Helpers exposed for tests.
bool parseWatchList(const std::string& s, std::vector<WatchField>& out, std::string* err);
bool parseIntList(const std::string& s, std::vector<int64_t>& out);   // "0x0, 0x60, -8"
bool parseOffsetSize(const std::string& s, uint32_t& off, uint32_t& size);  // "0x40:0x100"
bool parseBool(const std::string& s, bool def);

}  // namespace qs::probe
