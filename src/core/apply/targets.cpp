// QuattroStagioni - targets.ini parsing and validation.
#include "targets.h"

#include <cstdlib>

#include "../probe/probe_config.h"
#include "../season/season.h"

namespace qs::apply {
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

struct Names {
    const char* name;
    Semantic sem;
};
const Names kSemantics[] = {
    {"hour", Semantic::Hour},
    {"minute", Semantic::Minute},
    {"day_counter", Semantic::DayCounter},
    {"time_scale_day", Semantic::TimeScaleDay},
    {"time_scale_night", Semantic::TimeScaleNight},
    {"time_scale_current", Semantic::TimeScaleCurrent},
    {"weather_manual_flag", Semantic::WeatherManualFlag},
    {"weather_state_flags", Semantic::WeatherStateFlags},
    {"weather_index", Semantic::WeatherIndex},
    {"weather_weight", Semantic::WeatherWeight},
    {"weather_mix", Semantic::WeatherMix},
    {"precipitation", Semantic::Precipitation},
    {"rain_amount", Semantic::RainAmount},
    {"snow_rate", Semantic::SnowRate},
    {"temperature_offset", Semantic::TemperatureOffset},
    {"temperature", Semantic::Temperature},
    {"snow_amount", Semantic::SnowAmount},
    {"sun_elevation_offset", Semantic::SunElevationOffset},
    {"light_tint_r", Semantic::LightTintR},
    {"light_tint_g", Semantic::LightTintG},
    {"light_tint_b", Semantic::LightTintB},
    {"saturation", Semantic::Saturation},
    {"leaves_particles", Semantic::LeavesParticles},
    {"breath_visible", Semantic::BreathVisible},
};

}  // namespace

const char* toString(Feature f) {
    switch (f) {
        case Feature::Weather: return "weather";
        case Feature::Snow: return "snow";
        case Feature::Light: return "light";
        case Feature::DayLength: return "daylength";
        case Feature::Particles: return "particles";
        case Feature::Clock: return "clock";
        default: return "?";
    }
}

bool featureFromString(const std::string& s, Feature& out) {
    std::string l = lower(trim(s));
    for (int i = 0; i < kFeatureCount; ++i)
        if (l == toString(Feature(i))) {
            out = Feature(i);
            return true;
        }
    return false;
}

const char* toString(Weather w) {
    static const char* n[] = {"sunny", "cloudy", "rainy", "storm", "snowy", "windy"};
    return int(w) < kWeatherCount ? n[int(w)] : "?";
}

bool weatherFromString(const std::string& s, Weather& out) {
    std::string l = lower(trim(s));
    for (int i = 0; i < kWeatherCount; ++i)
        if (l == toString(Weather(i))) {
            out = Weather(i);
            return true;
        }
    return false;
}

const char* toString(Semantic s) {
    for (const Names& n : kSemantics)
        if (n.sem == s) return n.name;
    return "?";
}

bool semanticFromString(const std::string& sIn, Semantic& out, Weather* weather) {
    std::string s = lower(trim(sIn));
    for (const char* prefix : {"weather_weight_", "weather_mix_"}) {
        std::string p = prefix;
        if (s.compare(0, p.size(), p) == 0) {
            Weather w;
            if (!weatherFromString(s.substr(p.size()), w)) return false;
            if (weather) *weather = w;
            out = p == "weather_weight_" ? Semantic::WeatherWeight : Semantic::WeatherMix;
            return true;
        }
    }
    for (const Names& n : kSemantics)
        if (s == n.name && n.sem != Semantic::WeatherWeight && n.sem != Semantic::WeatherMix) {
            out = n.sem;
            return true;
        }
    return false;
}

Feature featureOf(Semantic s) {
    switch (s) {
        case Semantic::Hour:
        case Semantic::Minute:
        case Semantic::DayCounter: return Feature::Clock;
        case Semantic::TimeScaleDay:
        case Semantic::TimeScaleNight:
        case Semantic::TimeScaleCurrent: return Feature::DayLength;
        case Semantic::SnowAmount: return Feature::Snow;
        case Semantic::SunElevationOffset:
        case Semantic::LightTintR:
        case Semantic::LightTintG:
        case Semantic::LightTintB:
        case Semantic::Saturation: return Feature::Light;
        case Semantic::LeavesParticles:
        case Semantic::BreathVisible: return Feature::Particles;
        default: return Feature::Weather;
    }
}

bool isReadOnlySemantic(Semantic s) { return featureOf(s) == Feature::Clock; }

const char* toString(FieldKind k) {
    switch (k) {
        case FieldKind::F32: return "f32";
        case FieldKind::F64: return "f64";
        case FieldKind::I32: return "i32";
        case FieldKind::U32: return "u32";
        case FieldKind::U8: return "u8";
        case FieldKind::Flags32: return "flags32";
    }
    return "?";
}

bool fieldKindFromString(const std::string& s, FieldKind& out) {
    std::string l = lower(trim(s));
    if (l == "float") l = "f32";
    if (l == "double") l = "f64";
    if (l == "int") l = "i32";
    if (l == "byte" || l == "bool") l = "u8";
    for (FieldKind k : {FieldKind::F32, FieldKind::F64, FieldKind::I32, FieldKind::U32, FieldKind::U8, FieldKind::Flags32})
        if (l == toString(k)) {
            out = k;
            return true;
        }
    return false;
}

size_t fieldBytes(FieldKind k) {
    switch (k) {
        case FieldKind::F64: return 8;
        case FieldKind::U8: return 1;
        default: return 4;
    }
}

const char* toString(Mode m) {
    switch (m) {
        case Mode::Write: return "write";
        case Mode::Read: return "read";
        case Mode::Scale: return "scale";
        case Mode::Offset: return "offset";
    }
    return "?";
}

bool modeFromString(const std::string& s, Mode& out) {
    std::string l = lower(trim(s));
    if (l == "read-only" || l == "readonly") l = "read";
    if (l == "scale-original") l = "scale";
    if (l == "add") l = "offset";
    for (Mode m : {Mode::Write, Mode::Read, Mode::Scale, Mode::Offset})
        if (l == toString(m)) {
            out = m;
            return true;
        }
    return false;
}

const char* toString(InstanceKind k) {
    switch (k) {
        case InstanceKind::Scan: return "scan";
        case InstanceKind::Global: return "global";
        case InstanceKind::Capture: return "capture";
    }
    return "?";
}

bool parseDouble(const std::string& sIn, double& out) {
    std::string s = trim(sIn);
    if (s.empty()) return false;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        uint64_t v;
        if (!parseU64(s, v)) return false;
        out = double(v);
        return true;
    }
    char* end = nullptr;
    double v = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || *end != 0 || !std::isfinite(v)) return false;
    out = v;
    return true;
}

bool parseCheckList(const std::string& s, std::vector<ObjectCheck>& out, std::string* err) {
    out.clear();
    for (const std::string& item : split(s, ',')) {
        std::vector<std::string> p = split(item, ':');
        ObjectCheck c;
        uint64_t off;
        if (p.size() != 4 || !parseU64(p[0], off) || off > 0x100000 || !fieldKindFromString(p[1], c.kind) ||
            !parseDouble(p[2], c.minV) || !parseDouble(p[3], c.maxV) || c.maxV < c.minV) {
            if (err) *err = "bad Check item '" + item + "' (expected offset:type:min:max)";
            return false;
        }
        c.offset = uint32_t(off);
        out.push_back(c);
    }
    return true;
}

namespace {

bool parseObject(const Ini& ini, const std::string& sec, ObjectSpec& o, std::string& err) {
    o.className = ini.get(sec, "Class");
    std::string inst = lower(ini.get(sec, "Instance", o.className.empty() ? "global" : "scan"));
    if (inst == "scan") o.instance = InstanceKind::Scan;
    else if (inst == "global") o.instance = InstanceKind::Global;
    else if (inst == "capture") o.instance = InstanceKind::Capture;
    else return err = "Instance must be scan, global or capture", false;

    o.signatureText = ini.get(sec, "Signature");
    if (!o.signatureText.empty()) {
        std::string e;
        if (!probe::parsePattern(o.signatureText, o.signature, &e)) return err = "Signature: " + e, false;
        int64_t so = 0;
        if (ini.has(sec, "SigOffset") && !parseI64(ini.get(sec, "SigOffset"), so)) return err = "bad SigOffset", false;
        o.sigOffset = so;
        std::string rs = ini.get(sec, "Resolve", "none");
        if (!probe::parseRipSpec(rs, o.resolve)) return err = "bad Resolve (none|call|jmp|rip:<disp>:<len>)", false;
        std::string ss = lower(ini.get(sec, "SigSection", "code"));
        if (ss != "code" && ss != "rdata" && ss != "data") return err = "SigSection must be code, rdata or data", false;
        o.sigInData = ss != "code";
    }
    o.rva = ini.getU64(sec, "Rva", 0);
    if (ini.has(sec, "Chain") && !probe::parseIntList(ini.get(sec, "Chain"), o.chain)) return err = "bad Chain", false;
    if (o.chain.size() > 8) return err = "Chain longer than 8", false;

    std::string hook = lower(ini.get(sec, "Hook", "vtable"));
    if (hook == "vtable") o.hook = HookKind::Vtable;
    else if (hook == "minhook") o.hook = HookKind::MinHook;
    else return err = "Hook must be vtable or minhook", false;
    o.slot = int(ini.getInt(sec, "Slot", -1));
    std::string arg = lower(ini.get(sec, "CaptureArg", "rcx"));
    if (arg != "rcx" && arg != "rdx") return err = "CaptureArg must be rcx or rdx", false;
    o.captureRdx = arg == "rdx";
    o.keepHook = probe::parseBool(ini.get(sec, "KeepHook"), false);

    std::string mul = lower(ini.get(sec, "Multiple", "refuse"));
    if (mul == "refuse") o.multiple = MultiplePolicy::Refuse;
    else if (mul == "first") o.multiple = MultiplePolicy::First;
    else if (mul == "all") o.multiple = MultiplePolicy::All;
    else return err = "Multiple must be refuse, first or all", false;
    uint64_t mi = ini.getU64(sec, "MaxInstances", 4);
    o.maxInstances = uint32_t(mi < 1 ? 1 : mi > 16 ? 16 : mi);
    uint64_t mb = ini.getU64(sec, "ScanMaxMB", 4096);
    o.scanMaxMB = uint32_t(mb < 16 ? 16 : mb > 65536 ? 65536 : mb);
    if (ini.has(sec, "Check")) {
        std::string e;
        if (!parseCheckList(ini.get(sec, "Check"), o.checks, &e)) return err = e, false;
    }
    o.checkVtable = probe::parseBool(ini.get(sec, "CheckVtable"), true);

    // anchors per instance kind
    switch (o.instance) {
        case InstanceKind::Scan:
            if (o.className.empty()) return err = "Instance=scan needs Class", false;
            break;
        case InstanceKind::Global:
            if (o.signatureText.empty() == (o.rva == 0)) return err = "Instance=global needs exactly one of Signature or Rva", false;
            break;
        case InstanceKind::Capture:
            if (o.hook == HookKind::Vtable && (o.className.empty() || o.slot < 0))
                return err = "Instance=capture with Hook=vtable needs Class and Slot", false;
            if (o.hook == HookKind::MinHook && !(o.slot >= 0 && !o.className.empty()) && o.signatureText.empty() && !o.rva)
                return err = "Instance=capture with Hook=minhook needs Class+Slot, Signature or Rva", false;
            if (o.slot > 4096) return err = "Slot out of range", false;
            break;
    }
    return true;
}

bool parseTarget(const Ini& ini, const std::string& sec, const TargetsConfig& cfg, TargetSpec& t, std::string& err) {
    t.enabled = probe::parseBool(ini.get(sec, "Enabled"), false);
    t.object = lower(trim(ini.get(sec, "Object")));
    if (t.object.empty()) return err = "missing Object", false;
    for (size_t i = 0; i < cfg.objects.size(); ++i)
        if (cfg.objects[i].name == t.object) t.objectIndex = int(i);
    if (t.objectIndex < 0) return err = "Object '" + t.object + "' is not a valid [object.<name>] section", false;

    std::string sem = ini.get(sec, "Semantic");
    if (!semanticFromString(sem, t.semantic, &t.weather)) return err = "unknown Semantic '" + sem + "'", false;
    t.feature = featureOf(t.semantic);
    if (ini.has(sec, "Feature") && !featureFromString(ini.get(sec, "Feature"), t.feature)) return err = "bad Feature", false;

    uint64_t off;
    if (!parseU64(ini.get(sec, "Offset"), off) || off > 0x100000) return err = "missing or bad Offset (0..0x100000)", false;
    t.offset = uint32_t(off);
    std::string ty = ini.get(sec, "Type", "f32");
    if (!fieldKindFromString(ty, t.kind)) return err = "bad Type '" + ty + "' (f32 f64 i32 u32 u8 flags32)", false;
    if (ini.has(sec, "Bit")) {
        long long b = ini.getInt(sec, "Bit", -1);
        int maxBit = t.kind == FieldKind::U8 ? 7 : 31;
        if (t.kind == FieldKind::F32 || t.kind == FieldKind::F64 || t.kind == FieldKind::I32)
            return err = "Bit needs Type u8, u32 or flags32", false;
        if (b < 0 || b > maxBit) return err = "Bit out of range", false;
        t.bit = int(b);
    }
    if (ini.has(sec, "Mask")) {
        uint64_t m;
        if (t.kind != FieldKind::Flags32 || !parseU64(ini.get(sec, "Mask"), m) || !m || m > 0xFFFFFFFFull)
            return err = "Mask needs Type flags32 and a non-zero 32-bit value", false;
        t.mask = uint32_t(m);
    }
    // default mode by semantic
    if (isReadOnlySemantic(t.semantic)) t.mode = Mode::Read;
    else if (t.semantic == Semantic::TimeScaleDay || t.semantic == Semantic::TimeScaleNight ||
             t.semantic == Semantic::TimeScaleCurrent || t.semantic == Semantic::LightTintR ||
             t.semantic == Semantic::LightTintG || t.semantic == Semantic::LightTintB || t.semantic == Semantic::Saturation)
        t.mode = Mode::Scale;
    else t.mode = Mode::Write;
    if (ini.has(sec, "Mode") && !modeFromString(ini.get(sec, "Mode"), t.mode)) return err = "Mode must be write, read, scale or offset", false;
    if (isReadOnlySemantic(t.semantic) && t.mode != Mode::Read) return err = "clock semantics are read-only (Mode=read)", false;
    const bool floatKind = t.kind == FieldKind::F32 || t.kind == FieldKind::F64;
    if ((t.mode == Mode::Scale || t.mode == Mode::Offset) && (!floatKind && t.kind != FieldKind::I32 && t.kind != FieldKind::U32))
        return err = "Mode=scale/offset needs a numeric Type (f32 f64 i32 u32)", false;
    if ((t.kind == FieldKind::Flags32 || t.bit >= 0) && t.mode != Mode::Write && t.mode != Mode::Read)
        return err = "bit/flag fields support only Mode=write or read", false;
    if (t.kind == FieldKind::Flags32 && t.bit < 0 && t.semantic != Semantic::WeatherStateFlags && t.mode == Mode::Write)
        return err = "flags32 without Bit is only for weather_state_flags", false;

    auto getD = [&](const char* key, double& v) -> bool {
        if (!ini.has(sec, key)) return true;
        return parseDouble(ini.get(sec, key), v);
    };
    if (!getD("Scale", t.scale) || !getD("Bias", t.bias) || !getD("Min", t.minV) || !getD("Max", t.maxV) ||
        !getD("MaxRate", t.maxRate) || !getD("Tolerance", t.tolerance))
        return err = "bad number in Scale/Bias/Min/Max/MaxRate/Tolerance", false;
    if (t.mode == Mode::Read && t.scale == 0) return err = "Scale must not be 0 for Mode=read", false;
    if (!std::isnan(t.minV) && !std::isnan(t.maxV) && t.maxV < t.minV) return err = "Max < Min", false;
    if (t.maxRate < 0) return err = "MaxRate < 0", false;
    t.restore = probe::parseBool(ini.get(sec, "Restore"), true);
    return true;
}

}  // namespace

bool parseTargets(const Ini& ini, TargetsConfig& out, std::vector<std::string>& errors) {
    out = TargetsConfig();
    bool hasGeneral = false;
    for (const std::string& s : ini.sections()) hasGeneral = hasGeneral || s == "general";
    if (!hasGeneral) {
        errors.push_back("missing [General] section");
        return false;
    }
    auto u32 = [&](const char* sec, const char* key, uint32_t def, uint32_t lo, uint32_t hi) {
        uint64_t v = ini.getU64(sec, key, def);
        if (v < lo || v > hi) {
            errors.push_back(std::string("[") + sec + "] " + key + " out of range, using default");
            return def;
        }
        return uint32_t(v);
    };
    out.enabled = probe::parseBool(ini.get("General", "Enabled"), false);
    out.startDelayMs = u32("General", "StartDelayMs", out.startDelayMs, 0, 600000);
    out.intervalMs = u32("General", "IntervalMs", out.intervalMs, 20, 5000);
    out.resolveRetryMs = u32("General", "ResolveRetryMs", out.resolveRetryMs, 100, 600000);
    out.anomalyLimit = u32("General", "AnomalyLimit", out.anomalyLimit, 1, 1000);
    out.allowHooks = probe::parseBool(ini.get("General", "AllowHooks"), false);
    out.expectTimestamp = u32("General", "ExpectTimestamp", 0, 0, 0xFFFFFFFFu);
    out.logLevel = int(u32("General", "LogLevel", 1, 0, 3));
    if (ini.has("General", "Region")) {
        season::Region r;
        if (season::regionFromName(ini.get("General", "Region"), r)) out.region = int(r);
        else errors.push_back("[General] Region unknown, using Temperate");
    }
    for (int i = 0; i < kFeatureCount; ++i) {
        // [Features] keys: Weather Snow Light DayLength Particles Clock
        static const char* keys[] = {"Weather", "Snow", "Light", "DayLength", "Particles", "Clock"};
        out.features[i] = probe::parseBool(ini.get("Features", keys[i]), true);
    }
    auto dbl = [&](const char* key, double& v, double lo, double hi) {
        if (!ini.has("Weather", key)) return;
        double d;
        if (!parseDouble(ini.get("Weather", key), d) || d < lo || d > hi)
            errors.push_back(std::string("[Weather] ") + key + " invalid, using default");
        else v = d;
    };
    dbl("HoldHours", out.holdHours, 0.25, 240);
    dbl("BlendHours", out.blendHours, 0, 48);
    dbl("Persistence", out.persistence, 0, 0.95);
    if (out.blendHours > out.holdHours) out.blendHours = out.holdHours;
    out.seed = u32("Weather", "Seed", out.seed, 0, 0xFFFFFFFFu);
    for (int w = 0; w < kWeatherCount; ++w) {
        std::string n = toString(Weather(w));
        n[0] = char(n[0] - 'a' + 'A');
        out.flag[w] = u32("Weather", ("Flag" + n).c_str(), out.flag[w], 0, 0xFFFFFFFFu);
        out.index[w] = int(u32("Weather", ("Index" + n).c_str(), uint32_t(out.index[w]), 0, 0xFFFF));
    }

    std::vector<std::string> secs = ini.sections();
    for (const std::string& sec : secs) {
        if (sec.compare(0, 7, "object.") != 0) continue;
        ObjectSpec o;
        o.name = sec.substr(7);
        std::string err;
        if (o.name.empty() || !parseObject(ini, sec, o, err)) {
            errors.push_back("object '" + o.name + "': " + err + " (dropped)");
            continue;
        }
        out.objects.push_back(std::move(o));
    }
    for (const std::string& sec : secs) {
        if (sec.compare(0, 7, "target.") != 0) continue;
        TargetSpec t;
        t.name = sec.substr(7);
        std::string err;
        if (t.name.empty() || !parseTarget(ini, sec, out, t, err)) {
            errors.push_back("target '" + t.name + "': " + err + " (dropped)");
            continue;
        }
        out.targets.push_back(std::move(t));
    }
    return true;
}

bool parseTargetsText(const std::string& text, TargetsConfig& out, std::vector<std::string>& errors) {
    Ini ini;
    ini.parse(text);
    return parseTargets(ini, out, errors);
}

}  // namespace qs::apply
