// QuattroStagioni - GPU live swap, part 4 (see live.h).
#include "live.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>

#include "../ini.h"

namespace qs::gpu {
namespace {

const char* const kSec = "GpuSwap";

bool readBool(const Ini& ini, const char* key, bool def, std::vector<std::string>& w) {
    if (!ini.has(kSec, key)) return def;
    const std::string v = ini.get(kSec, key);
    if (v == "1" || v == "true" || v == "True" || v == "TRUE") return true;
    if (v == "0" || v == "false" || v == "False" || v == "FALSE") return false;
    w.push_back(std::string("[GpuSwap] ") + key + "=" + v + " not 0/1, using " + (def ? "1" : "0"));
    return def;
}

int readInt(const Ini& ini, const char* key, int def, int lo, int hi, std::vector<std::string>& w) {
    if (!ini.has(kSec, key)) return def;
    const std::string v = ini.get(kSec, key);
    char* end = nullptr;
    const long x = std::strtol(v.c_str(), &end, 10);
    if (v.empty() || !end || *end) {
        w.push_back(std::string("[GpuSwap] ") + key + "=" + v + " is not a number, using " + std::to_string(def));
        return def;
    }
    if (x < lo || x > hi) {
        const int c = int(std::min<long>(hi, std::max<long>(lo, x)));
        w.push_back(std::string("[GpuSwap] ") + key + "=" + v + " out of range [" + std::to_string(lo) + ", " +
                    std::to_string(hi) + "], using " + std::to_string(c));
        return c;
    }
    return int(x);
}

}  // namespace

void GpuSwapSettings::load(const Ini& ini, std::vector<std::string>& w) {
    *this = GpuSwapSettings();
    enabled = readBool(ini, "Enabled", enabled, w);
    if (ini.has(kSec, "MaxMBPerFrame")) {
        const std::string v = ini.get(kSec, "MaxMBPerFrame");
        char* end = nullptr;
        const double x = std::strtod(v.c_str(), &end);
        if (v.empty() || !end || *end || !std::isfinite(x)) {
            w.push_back("[GpuSwap] MaxMBPerFrame=" + v + " is not a number, using 4");
        } else if (x < 0.25 || x > 64.0) {
            maxMBPerFrame = std::min(64.0, std::max(0.25, x));
            char b[160];
            std::snprintf(b, sizeof(b), "[GpuSwap] MaxMBPerFrame=%s out of range [0.25, 64], using %.2f", v.c_str(),
                          maxMBPerFrame);
            w.push_back(b);
        } else {
            maxMBPerFrame = x;
        }
    }
    logLevel = readInt(ini, "LogLevel", logLevel, 0, 2, w);
    ringMB = readInt(ini, "RingMB", ringMB, 12, 512, w);
    cacheMB = readInt(ini, "CacheMB", cacheMB, 16, 2048, w);
    assumeCommonState = readBool(ini, "AssumeCommonState", assumeCommonState, w);
    preloadAllPacks = readBool(ini, "PreloadAllPacks", preloadAllPacks, w);
}

uint64_t GpuSwapSettings::budgetBytesPerFrame() const {
    const double mb = std::min(64.0, std::max(0.25, maxMBPerFrame));
    return uint64_t(mb * 1048576.0);
}

std::string gpuSwapIniTemplate() {
    return "[GpuSwap]\n"
           "Enabled=1\n"
           "MaxMBPerFrame=4\n"
           "LogLevel=1\n"
           "RingMB=48\n"
           "CacheMB=96\n"
           "AssumeCommonState=0\n"
           "PreloadAllPacks=0\n";
}

bool splitTextureKey(const std::string& key, std::string& pazKey, uint64_t& offset) {
    const size_t at = key.rfind('@');
    if (at == std::string::npos || at == 0 || at + 1 >= key.size()) return false;
    const std::string num = key.substr(at + 1);
    if (num.find_first_not_of("0123456789") != std::string::npos || num.size() > 19) return false;
    pazKey = key.substr(0, at);
    offset = std::strtoull(num.c_str(), nullptr, 10);
    return true;
}

TargetPlan planTargets(const FingerprintIndex& index, const ServedStageFn& served, const SlotOfFn& slotOf,
                       const TargetForFn& targetFor) {
    TargetPlan p;
    const size_t n = index.textureCount();
    p.slot.assign(n, -1);
    std::set<std::string> missing;
    for (size_t t = 0; t < n; ++t) {
        TextureDesc d;
        std::string paz, stage;
        uint64_t off = 0;
        if (!index.texture(uint32_t(t), d) || !splitTextureKey(d.key, paz, off) || !served(paz, off, stage)) {
            ++p.unknown;
            continue;
        }
        const int s = stage.empty() ? 0 : slotOf(stage);
        if (s < 0) {
            missing.insert(stage);
            ++p.waiting;
            continue;
        }
        const int tgt = targetFor(uint32_t(t), s);
        p.slot[t] = tgt;
        if (tgt == 0) ++p.vanilla;
        else if (tgt > 0) ++p.staged;
    }
    p.missingStages.assign(missing.begin(), missing.end());
    return p;
}

TargetPlan planGlobal(const FingerprintIndex& index, int slot, const TargetForFn& targetFor) {
    TargetPlan p;
    const size_t n = index.textureCount();
    p.slot.assign(n, -1);
    if (slot < 0) {
        p.unknown = n;
        return p;
    }
    for (size_t t = 0; t < n; ++t) {
        const int tgt = targetFor(uint32_t(t), slot);
        p.slot[t] = tgt;
        if (tgt == 0) ++p.vanilla;
        else if (tgt > 0) ++p.staged;
    }
    return p;
}

bool checkDestination(const LiveDesc& live, const Resident& r, const TextureDesc& tex, uint32_t sub, uint32_t ddsMip,
                      const MipLayout& data, std::string* why) {
    auto no = [why](const std::string& s) {
        if (why) *why = s;
        return false;
    };
    if (live.dimension != kDimTexture2D) return no("not a 2D texture");
    if (live.sampleCount != 1) return no("multisampled");
    if (live.layout != 0) return no("non-default texture layout");
    if (live.flags != 0) return no("resource flags (render target, UAV or simultaneous access)");
    if (live.depthOrArraySize != 1 || r.shape.arraySize != 1) return no("texture array");
    const uint16_t liveMips = live.mipLevels ? live.mipLevels : 1;
    if (live.dxgiFormat != r.shape.dxgiFormat || live.width != r.shape.width || live.height != r.shape.height ||
        liveMips != r.shape.mipLevels)
        return no("description changed since tracking");
    if (live.width > 0xFFFFFFFFull) return no("width out of range");
    const Family fam = familyFromDxgi(live.dxgiFormat);
    if (fam == Family::Unknown || fam != tex.family) return no("format family differs from the texture data");
    if (sub >= liveMips || sub >= r.content.size()) return no("subresource out of range");
    if (r.mipOffset < 0 || int64_t(sub) + r.mipOffset != int64_t(ddsMip)) return no("mip offset mismatch");
    if (ddsMip >= tex.mipCount) return no("mip beyond the texture data");
    // The resource's mip 0 must be the texture's mip `mipOffset`.
    const MipLayout top = mipLayout(fam, tex.width, tex.height, uint32_t(r.mipOffset));
    if (top.width != uint32_t(live.width) || top.height != live.height) return no("size differs from the texture data");
    const MipLayout l = mipLayout(fam, uint32_t(live.width), live.height, sub);
    if (l.width != data.width || l.height != data.height || l.rowBytes != data.rowBytes || l.rows != data.rows)
        return no("mip layout differs from the data");
    return true;
}

double FrameRate::sample(uint64_t totalBytes, uint64_t totalFrames) {
    const uint64_t db = totalBytes - bytes0_, df = totalFrames - frames0_;
    bytes0_ = totalBytes;
    frames0_ = totalFrames;
    return df ? double(db) / double(df) / 1048576.0 : 0.0;
}

}  // namespace qs::gpu
