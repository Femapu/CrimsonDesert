// QuattroStagioni - season engine internals shared by season.cpp and season_config.cpp (not a contract).
#pragma once
#include "season.h"

namespace qs::season::detail {

// A regional calendar in local days: phase starts and lengths (sum = year), plus the region offset.
struct Cal {
    double start[kPhaseCount] = {}, len[kPhaseCount] = {};
    double year = 0, offset = 0;
};

// Position inside a calendar.
struct Pos {
    int phase = 0;
    double progress = 0;  // 0..1 inside the phase
    double blend = 0;     // 0 on the plateau, 0..1 across the transition tail
    double local = 0;     // local day of year
};

double wrap(double x, double y);                       // into [0, y)
Cal makeCal(const SeasonConfig& c, int region);        // region < 0: global calendar (no scale/offset)
Pos locate(const SeasonConfig& c, const Cal& cal, double local);
int effectiveSteps(const SeasonConfig& c);             // 0 when transitionFraction == 0
VisualStage stageFromPos(const SeasonConfig& c, const Pos& p);
double localDay(const SeasonConfig& c, const Cal& cal, double day);

}  // namespace qs::season::detail
