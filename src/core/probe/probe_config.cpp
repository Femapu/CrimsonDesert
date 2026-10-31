// QuattroStagioni - probes.ini parsing.
#include "probe_config.h"

#include <cstdlib>

namespace qs::probe {
namespace {

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) --b;
    return s.substr(a, b - a);
}

std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t p = 0;
    while (p <= s.size()) {
        size_t c = s.find(sep, p);
        if (c == std::string::npos) c = s.size();
        std::string t = trim(s.substr(p, c - p));
        if (!t.empty()) out.push_back(t);
        p = c + 1;
    }
    return out;
}

bool parseI64(const std::string& sIn, int64_t& out) {
    std::string s = trim(sIn);
    bool neg = false;
    if (!s.empty() && (s[0] == '-' || s[0] == '+')) {
        neg = s[0] == '-';
        s = s.substr(1);
    }
    uint64_t v;
    if (!parseU64(s, v) || v > uint64_t(INT64_MAX)) return false;
    out = neg ? -int64_t(v) : int64_t(v);
    return true;
}

uint32_t getU32(const Ini& ini, const std::string& sec, const char* key, uint32_t def) {
    uint64_t v = ini.getU64(sec, key, def);
    return v > 0xFFFFFFFFull ? def : uint32_t(v);
}

}  // namespace

const char* toString(InstanceMode m) {
    switch (m) {
        case InstanceMode::None: return "none";
        case InstanceMode::Capture: return "capture";
        case InstanceMode::Scan: return "scan";
        case InstanceMode::Global: return "global";
        case InstanceMode::Provided: return "provided";
    }
    return "?";
}

bool parseBool(const std::string& sIn, bool def) {
    std::string s = lower(trim(sIn));
    if (s == "1" || s == "true" || s == "yes" || s == "on") return true;
    if (s == "0" || s == "false" || s == "no" || s == "off") return false;
    return def;
}

bool parseIntList(const std::string& s, std::vector<int64_t>& out) {
    out.clear();
    for (const std::string& t : split(s, ',')) {
        int64_t v;
        if (!parseI64(t, v)) return false;
        out.push_back(v);
    }
    return true;
}

bool parseOffsetSize(const std::string& s, uint32_t& off, uint32_t& size) {
    std::vector<std::string> p = split(s, ':');
    uint64_t a, b;
    if (p.size() != 2 || !parseU64(p[0], a) || !parseU64(p[1], b) || a > 0xFFFFFFF || b == 0 || b > 0x100000) return false;
    off = uint32_t(a);
    size = uint32_t(b);
    return true;
}

bool parseWatchList(const std::string& s, std::vector<WatchField>& out, std::string* err) {
    out.clear();
    for (const std::string& item : split(s, ',')) {
        std::vector<std::string> p = split(item, ':');
        WatchField w;
        uint64_t off;
        if (p.empty() || p.size() > 3 || !parseU64(p[0], off) || off > 0xFFFFFFF) {
            if (err) *err = "bad watch item '" + item + "'";
            return false;
        }
        w.offset = uint32_t(off);
        if (p.size() >= 2 && !parseFieldType(p[1], w.type)) {
            if (err) *err = "bad field type in '" + item + "'";
            return false;
        }
        if (p.size() == 3) w.name = p[2];
        out.push_back(w);
    }
    return true;
}

bool parseProbeConfig(const Ini& ini, ProbeConfig& out, std::vector<std::string>& errors) {
    out = ProbeConfig();
    std::vector<std::string> secs = ini.sections();
    bool hasGeneral = false;
    for (const std::string& s : secs) hasGeneral = hasGeneral || s == "general";
    if (!hasGeneral) {
        errors.push_back("[General] section missing");
        return false;
    }
    const std::string g = "general";
    out.enabled = parseBool(ini.get(g, "Enabled"), false);
    out.startDelayMs = getU32(ini, g, "StartDelayMs", out.startDelayMs);
    out.intervalMs = getU32(ini, g, "IntervalMs", out.intervalMs);
    if (out.intervalMs < 10) out.intervalMs = 10;
    out.allowHooks = parseBool(ini.get(g, "AllowHooks"), false);
    out.maxLogMB = getU32(ini, g, "MaxLogMB", out.maxLogMB);
    out.summarySec = getU32(ini, g, "SummarySec", out.summarySec);
    out.logSections = parseBool(ini.get(g, "LogSections"), true);
    out.expectTimestamp = getU32(ini, g, "ExpectTimestamp", 0);

    for (const std::string& sec : secs) {
        if (sec.compare(0, 6, "probe.") != 0) continue;
        ProbeSpec p;
        p.name = sec.substr(6);
        auto bad = [&](const std::string& why) { errors.push_back("probe '" + p.name + "': " + why); };
        p.enabled = parseBool(ini.get(sec, "Enabled"), true);
        if (!p.enabled) continue;

        p.className = ini.get(sec, "Class");
        p.listGlob = ini.get(sec, "ListClasses");
        p.signatureText = ini.get(sec, "Signature");
        p.provider = lower(ini.get(sec, "Provider"));
        int anchors = !p.className.empty() + !p.listGlob.empty() + !p.signatureText.empty() + ini.has(sec, "Rva") + !p.provider.empty();
        if (anchors != 1) {
            bad("needs exactly one of Class, ListClasses, Signature, Rva, Provider");
            continue;
        }
        std::string err;
        if (!p.signatureText.empty() && !parsePattern(p.signatureText, p.signature, &err)) {
            bad("Signature: " + err);
            continue;
        }
        std::string sigSec = lower(ini.get(sec, "SigSection", "code"));
        if (sigSec != "code" && sigSec != "rdata" && sigSec != "data") {
            bad("SigSection must be code, rdata or data");
            continue;
        }
        p.sigInData = sigSec != "code";
        if (ini.has(sec, "SigOffset") && !parseI64(ini.get(sec, "SigOffset"), p.sigOffset)) {
            bad("bad SigOffset");
            continue;
        }
        if (!parseRipSpec(ini.get(sec, "Resolve"), p.resolve)) {
            bad("Resolve must be none, call, jmp or rip:<dispOffset>:<insnLen>");
            continue;
        }
        p.allowMultiple = parseBool(ini.get(sec, "AllowMultiple"), false);
        if (ini.has(sec, "Rva")) {
            if (!parseU64(ini.get(sec, "Rva"), p.rva) || p.rva == 0) {
                bad("bad Rva");
                continue;
            }
        }
        p.logVtable = parseBool(ini.get(sec, "LogVtable"), false);
        p.maxSlots = getU32(ini, sec, "MaxSlots", p.maxSlots);
        p.logHierarchy = parseBool(ini.get(sec, "LogHierarchy"), false);

        std::string inst = lower(ini.get(sec, "Instance", "none"));
        if (inst == "none") p.instance = InstanceMode::None;
        else if (inst == "capture") p.instance = InstanceMode::Capture;
        else if (inst == "scan") p.instance = InstanceMode::Scan;
        else if (inst == "global") p.instance = InstanceMode::Global;
        else if (inst == "provided") p.instance = InstanceMode::Provided;
        else {
            bad("Instance must be none, capture, scan, global or provided");
            continue;
        }
        p.slot = int(ini.getInt(sec, "Slot", -1));
        std::string hk = lower(ini.get(sec, "Hook", "vtable"));
        if (hk == "vtable") p.hook = HookKind::Vtable;
        else if (hk == "minhook") p.hook = HookKind::MinHook;
        else {
            bad("Hook must be vtable or minhook");
            continue;
        }
        std::string arg = lower(ini.get(sec, "CaptureArg", "rcx"));
        if (arg != "rcx" && arg != "rdx") {
            bad("CaptureArg must be rcx or rdx");
            continue;
        }
        p.captureRdx = arg == "rdx";
        p.logCallers = parseBool(ini.get(sec, "LogCallers"), true);
        if (!parseIntList(ini.get(sec, "Chain"), p.chain)) {
            bad("bad Chain");
            continue;
        }
        p.scanMaxMB = getU32(ini, sec, "ScanMaxMB", p.scanMaxMB);
        p.rescanSec = getU32(ini, sec, "RescanSec", p.rescanSec);
        p.scanSliceMs = getU32(ini, sec, "ScanSliceMs", p.scanSliceMs);
        if (p.scanSliceMs < 20) p.scanSliceMs = 20;
        p.scanRegionMaxMB = getU32(ini, sec, "ScanRegionMaxMB", p.scanRegionMaxMB);
        p.maxInstances = getU32(ini, sec, "MaxInstances", p.maxInstances);
        if (p.maxInstances < 1) p.maxInstances = 1;
        if (p.maxInstances > 64) p.maxInstances = 64;
        p.checkVtable = parseBool(ini.get(sec, "CheckVtable"), true);
        if (ini.has(sec, "Dump") && !parseOffsetSize(ini.get(sec, "Dump"), p.dumpOffset, p.dumpSize)) {
            bad("Dump must be <offset>:<size> (size 1..0x100000)");
            continue;
        }
        if (!parseWatchList(ini.get(sec, "Watch"), p.watches, &err)) {
            bad("Watch: " + err);
            continue;
        }
        std::string eps = ini.get(sec, "Epsilon");
        if (!eps.empty()) p.epsilon = std::strtod(eps.c_str(), nullptr);
        p.intervalMs = getU32(ini, sec, "IntervalMs", 0);
        p.durationSec = getU32(ini, sec, "DurationSec", 0);
        p.stats = parseBool(ini.get(sec, "Stats"), false);
        std::string ranges = ini.get(sec, "StatsRanges", "0:24, 0:1");
        for (const std::string& r : split(ranges, ',')) {
            std::vector<std::string> ab = split(r, ':');
            if (ab.size() == 2) p.statsRanges.push_back({std::strtof(ab[0].c_str(), nullptr), std::strtof(ab[1].c_str(), nullptr)});
        }

        // cross-field validation
        const bool isClass = !p.className.empty();
        const bool isFunc = !p.signatureText.empty() || p.rva;
        if (p.instance == InstanceMode::Capture) {
            if (p.hook == HookKind::Vtable && (!isClass || p.slot < 0)) {
                bad("Instance=capture with Hook=vtable needs Class and Slot");
                continue;
            }
            if (p.hook == HookKind::MinHook && !(isFunc || (isClass && p.slot >= 0))) {
                bad("Instance=capture with Hook=minhook needs Signature/Rva, or Class and Slot");
                continue;
            }
        }
        if (p.instance == InstanceMode::Scan && !isClass) {
            bad("Instance=scan needs Class (it looks for the vtable pointer in heap memory)");
            continue;
        }
        if (p.instance == InstanceMode::Global && !isFunc) {
            bad("Instance=global needs Signature (usually with Resolve=rip:..) or Rva");
            continue;
        }
        if ((p.instance == InstanceMode::Provided) != !p.provider.empty()) {
            bad("Provider and Instance=provided go together");
            continue;
        }
        if (!p.provider.empty() && p.provider != "climate_manager") {
            bad("Provider must be climate_manager");
            continue;
        }
        if ((p.dumpSize || !p.watches.empty()) && p.instance == InstanceMode::None) {
            bad("Dump/Watch need an Instance mode");
            continue;
        }
        if (p.stats && !p.dumpSize) {
            bad("Stats needs Dump");
            continue;
        }
        if (p.instance != InstanceMode::None && !p.listGlob.empty()) {
            bad("ListClasses cannot have instances");
            continue;
        }
        out.probes.push_back(p);
    }
    return true;
}

bool parseProbeConfigText(const std::string& text, ProbeConfig& out, std::vector<std::string>& errors) {
    Ini ini;
    ini.parse(text);
    return parseProbeConfig(ini, out, errors);
}

}  // namespace qs::probe
