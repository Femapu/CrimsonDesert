// QuattroStagioni - stage planning. See plan.h.
#include "plan.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>

namespace qs::pack {
namespace {

int stepsOf(const season::SeasonConfig& cfg) { return cfg.stageCount() / season::kPhaseCount - 1; }

bool contains(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

}  // namespace

std::vector<std::string> StagePlan::missingIds() const {
    std::vector<std::string> v;
    for (const StageNeed& s : stages)
        if (!s.present) v.push_back(s.stageId);
    return v;
}

std::vector<std::string> StagePlan::neededIds() const {
    std::vector<std::string> v;
    for (const StageNeed& s : stages) v.push_back(s.stageId);
    return v;
}

std::string StagePlan::text(const season::SeasonConfig& cfg) const {
    std::string o;
    char buf[256];
    std::snprintf(buf, sizeof buf, "stages needed from day %.2f to %.2f (%s -> %s):\n", fromDay, toDay,
                  cfg.dayToLabel(fromDay).c_str(), cfg.dayToLabel(toDay).c_str());
    o += buf;
    for (const StageNeed& s : stages) {
        std::string regions;
        for (int r = 0; r < season::kRegionCount; ++r)
            if (s.regionMask & (1u << r)) regions += std::string(regions.empty() ? "" : ",") + season::toString(season::Region(r));
        std::snprintf(buf, sizeof buf, "  %-7s %-12s days %8.2f .. %8.2f  %-8s %8.1f MB%s  [%s]\n", s.stageId.c_str(),
                      stageFileName(s.stageId).c_str(), s.firstDay, s.lastDay, s.present ? "present" : "MISSING",
                      double(s.estimatedBytes) / 1048576.0, (!s.present && s.estimatedBytes == 0) ? " (?)" : "",
                      regions.c_str());
        o += buf;
    }
    std::snprintf(buf, sizeof buf, "%zu needed: %zu present (%.1f MB), %zu missing (%.1f MB to generate)\n",
                  stages.size(), presentCount, double(presentBytes) / 1048576.0, missingCount,
                  double(missingBytes) / 1048576.0);
    o += buf;
    return o;
}

uint32_t enabledRegionMask(const season::SeasonConfig& cfg) {
    uint32_t m = 0;
    for (int r = 0; r < season::kRegionCount; ++r)
        if (cfg.regions[r].enabled) m |= 1u << r;
    return m;
}

int stageCalendarIndex(const season::SeasonConfig& cfg, const std::string& stageId) {
    const int n = stepsOf(cfg);
    season::VisualStage vs;
    if (!season::parseStageId(stageId, n > 0 ? n : 1, vs)) return -1;
    if (vs.step > n) return -1;
    return int(vs.from) * (n + 1) + vs.step;
}

std::string stageIdAt(const season::SeasonConfig& cfg, int idx) {
    const int n = stepsOf(cfg);
    if (idx < 0 || idx >= season::kPhaseCount * (n + 1)) return "";
    season::VisualStage vs;
    vs.from = season::Phase(idx / (n + 1));
    vs.step = idx % (n + 1);
    vs.to = vs.step ? season::nextPhase(vs.from) : vs.from;
    return vs.id();
}

std::string stageFileName(const std::string& id) {
    std::string f = id;
    for (char& c : f)
        if (c == '>') c = '-';
    return f + ".qsp";
}

std::string stageIdFromFileName(const std::string& name) {
    std::string s = name;
    if (s.size() > 4) {
        std::string ext = s.substr(s.size() - 4);
        for (char& c : ext) c = char(tolower(static_cast<unsigned char>(c)));
        if (ext == ".qsp") s.resize(s.size() - 4);
    }
    for (char& c : s) c = char(toupper(static_cast<unsigned char>(c)));
    if (s.size() >= 5 && s[1] == '-') s[1] = '>';
    season::VisualStage vs;
    if (!season::parseStageId(s, 16, vs)) return "";
    return vs.id() == s ? s : "";
}

StagePlan planStages(const season::SeasonConfig& cfg, double day, double days, uint32_t regionMask,
                     const std::vector<std::string>& presentIds, const std::function<uint64_t(const std::string&)>& estimate) {
    StagePlan plan;
    if (!(days >= 0)) days = 0;
    plan.fromDay = day;
    plan.toDay = day + days;
    const double J = std::max(0.0, double(cfg.assetJitterDays));
    std::map<std::string, StageNeed> needs;
    for (int r = 0; r < season::kRegionCount; ++r) {
        if (!(regionMask & (1u << r)) || !cfg.regions[r].enabled) continue;
        const season::Region reg = season::Region(r);
        const double end = day + days + J;
        double t = day - J;
        for (int guard = 0; guard < 100000 && t <= end; ++guard) {
            double next = cfg.nextStageChangeDay(t, reg);
            if (!(next > t + 1e-9)) next = t + 1e-6;  // rounding at a boundary: step over it
            const double segEnd = std::min(next, end);
            // The stage at t and in the middle of the segment: exactly at a boundary stageAt may still
            // round to the previous stage while nextStageChangeDay already skips to the next boundary.
            const season::VisualStage at[2] = {season::stageAt(cfg, reg, t),
                                               season::stageAt(cfg, reg, std::isfinite(segEnd) ? 0.5 * (t + segEnd) : t)};
            for (int q = 0; q < 2; ++q) {
                const season::VisualStage& s = at[q];
                if (s.isVanilla() || (q == 1 && s == at[0])) continue;
                const double first = std::max(day, t - J), last = std::min(day + days, segEnd + J);
                if (first > last) continue;
                StageNeed& n = needs[s.id()];
                if (n.stageId.empty()) {
                    n.stageId = s.id();
                    n.calendarIndex = stageCalendarIndex(cfg, n.stageId);
                    n.firstDay = first;
                    n.lastDay = last;
                }
                n.firstDay = std::min(n.firstDay, first);
                n.lastDay = std::max(n.lastDay, last);
                n.regionMask |= 1u << r;
            }
            if (next == std::numeric_limits<double>::infinity()) break;
            t = next;
        }
    }
    for (auto& kv : needs) {
        StageNeed n = kv.second;
        n.present = contains(presentIds, n.stageId);
        if (estimate) n.estimatedBytes = estimate(n.stageId);
        if (n.present) { plan.presentCount++; plan.presentBytes += n.estimatedBytes; }
        else { plan.missingCount++; plan.missingBytes += n.estimatedBytes; }
        plan.stages.push_back(n);
    }
    std::sort(plan.stages.begin(), plan.stages.end(), [](const StageNeed& a, const StageNeed& b) {
        if (a.firstDay != b.firstDay) return a.firstDay < b.firstDay;
        return a.calendarIndex < b.calendarIndex;
    });
    return plan;
}

std::string fallbackStage(const season::SeasonConfig& cfg, const std::string& stageId,
                          const std::vector<std::string>& presentIds) {
    const int count = cfg.stageCount();
    int idx = stageCalendarIndex(cfg, stageId);
    if (idx < 0) return "";
    for (int k = 0; k < count; ++k) {
        const std::string id = stageIdAt(cfg, ((idx - k) % count + count) % count);
        if (id == "U") return "";  // walked back to vanilla summer
        if (contains(presentIds, id)) return id;
    }
    return "";
}

std::vector<std::string> packsToOpen(const season::SeasonConfig& cfg, const StagePlan& plan,
                                     const std::vector<std::string>& presentIds) {
    std::vector<std::pair<int, std::string>> v;
    for (const StageNeed& s : plan.stages) {
        const std::string id = s.present ? s.stageId : fallbackStage(cfg, s.stageId, presentIds);
        if (id.empty()) continue;
        std::pair<int, std::string> e(stageCalendarIndex(cfg, id), id);
        if (std::find(v.begin(), v.end(), e) == v.end()) v.push_back(e);
    }
    std::sort(v.begin(), v.end());
    std::vector<std::string> out;
    for (auto& e : v) out.push_back(e.second);
    return out;
}

}  // namespace qs::pack
