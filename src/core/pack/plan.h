// QuattroStagioni - stage planning: which season packs are needed from a day for the next N days.
//
// Every texture follows its own calendar (region + per-asset jitter, season::stageAt), so at any
// moment the world shows several visual stages at once. Packs stay one per stage; this module says
// which stages can be needed in a window of days, which of them have a pack file and which are
// missing, so that qs_gen can generate exactly those (qs_gen --plan / --stages-needed-file) and the
// plugin can keep just the right packs open (io::setSeasonDay) and show the list in the F5 menu.
//
// Model. A texture of region r with jitter j (|j| <= assetJitterDays = J) sees calendar day x + j at
// real day x. A calendar segment [a, b) of stage s of region r is therefore seen by SOME texture
// during real days [a - J, b + J). The planner walks the unjittered calendar of every region in the
// mask over [day - J, day + days + J] (season::nextStageChangeDay) and intersects those intervals
// with [day, day + days]. It is a superset of what the real textures need (only the seeds that exist
// matter), cheap (a few dozen stageAt calls per region) and needs no texture list.
// The vanilla Summer plateau ("U") is never listed: it needs no pack.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "../season/season.h"

namespace qs::pack {

struct StageNeed {
    std::string stageId;          // season::VisualStage::id(), e.g. "A>W.2"
    int calendarIndex = -1;       // position in season::allStages(cfg)
    double firstDay = 0;          // absolute day from which some texture may show it (clipped to the window)
    double lastDay = 0;           // absolute day until which some texture may show it (clipped)
    uint32_t regionMask = 0;      // bit (1 << season::Region) for every region that needs it
    bool present = false;         // a pack for this stage exists
    uint64_t estimatedBytes = 0;  // pack payload size (present: real size if known; missing: estimate, 0 = unknown)
};

struct StagePlan {
    double fromDay = 0, toDay = 0;
    std::vector<StageNeed> stages;  // ordered by firstDay, then calendar order
    size_t presentCount = 0, missingCount = 0;
    uint64_t presentBytes = 0, missingBytes = 0;
    std::vector<std::string> missingIds() const;
    std::vector<std::string> neededIds() const;
    // Human readable table (qs_gen --plan, plugin log). `cfg` is used for the day labels.
    std::string text(const season::SeasonConfig& cfg) const;
};

// Bit mask of the enabled regions of `cfg`.
uint32_t enabledRegionMask(const season::SeasonConfig& cfg);

// Calendar index of a stage id ("A>W.2") for the transition steps of `cfg`; -1 if it is not a stage
// of that calendar (bad id, or a transition step that does not exist with these settings).
int stageCalendarIndex(const season::SeasonConfig& cfg, const std::string& stageId);
// Stage id of a calendar index (inverse of stageCalendarIndex); "" if out of range.
std::string stageIdAt(const season::SeasonConfig& cfg, int calendarIndex);

// Pack file name of a stage id: "A>W.2" -> "A-W.2.qsp" ('>' is not allowed in Windows file names).
std::string stageFileName(const std::string& stageId);
// Inverse: "A-W.2.qsp" (any case for the extension) -> "A>W.2"; "" if the name is not a stage pack name.
std::string stageIdFromFileName(const std::string& fileName);

// Stages needed in [day, day + days] by textures of the regions in `regionMask` (disabled regions are
// skipped). `presentIds`: stages that have a pack. `estimate(stageId)` (optional) gives the payload
// size for the MB column.
StagePlan planStages(const season::SeasonConfig& cfg, double day, double days, uint32_t regionMask,
                     const std::vector<std::string>& presentIds,
                     const std::function<uint64_t(const std::string&)>& estimate = {});

// Nearest stage at or before `stageId` in calendar order that is in `presentIds`. Returns "" when the
// walk reaches the vanilla Summer plateau first (vanilla textures) or nothing is present.
std::string fallbackStage(const season::SeasonConfig& cfg, const std::string& stageId,
                          const std::vector<std::string>& presentIds);

// Packs to keep open for a plan: every present needed stage, plus the fallback of every missing one.
// Sorted in calendar order, without duplicates.
std::vector<std::string> packsToOpen(const season::SeasonConfig& cfg, const StagePlan& plan,
                                     const std::vector<std::string>& presentIds);

}  // namespace qs::pack
