// QuattroStagioni - the "Charting the Unknown" DLC area, left to the game like the desert.
//
// The DLC map reuses the base game's vegetation textures, so the mod cannot tell its trees apart:
// while the camera is inside the area the whole mod steps aside (textures served as the game's for new
// reads, weather, snow, temperatures, particles and light all handed back), exactly like an excluded
// region. The area is a polygon in world metres (camera X/Z, the climate manager's coordinates),
// written by hand in [DLCArea] Points from the border points marked in game (F5 > Info).
//
// This file is the portable part (unit tested in tests/test_dlc_area.cpp):
//   DlcAreaConfig    [DLCArea] keys of QuattroStagioni.ini (empty Points = exclusion inactive)
//   parsePoints      "x1,z1; x2,z2; ..." -> polygon
//   insidePolygon    even-odd point in polygon
//   distanceToPolygon distance from a point to the polygon's border
//   dlcInside        the decision, with a Margin band so that walking along the border does not flicker
// The Win32 side (src/win/world_pos.cpp) reads the camera and applies the decision in the season tick.
#pragma once
#include <string>
#include <vector>

#include "../ini.h"

namespace qs::apply {

struct WorldPoint {
    float x = 0, z = 0;
};

struct DlcAreaConfig {
    bool enabled = true;              // Enabled: 0 = the DLC area gets the seasons like the rest
    std::vector<WorldPoint> points;   // Points: the area's border (at least 3 points, else inactive)
    float marginM = 50.0f;            // Margin: metres past the border before the area is left again
    float logIntervalSec = 10.0f;     // LogIntervalSec: "position:" log line period (0 = never)

    // True when the exclusion can apply (enabled and a polygon of at least 3 points).
    bool active() const { return enabled && points.size() >= 3; }
    void load(const Ini& ini, std::vector<std::string>& warnings);
};

// Parses "x1,z1; x2,z2; ..." (also "x1 z1; x2 z2"). Decimals with a dot. Empty text -> empty list,
// true. On a malformed point returns false, `why` says which, and `out` is left empty.
bool parsePoints(const std::string& text, std::vector<WorldPoint>& out, std::string* why = nullptr);

// "x1,z1; x2,z2; ..." with whole metres (the inverse of parsePoints, for logs and the menu).
std::string formatPoints(const std::vector<WorldPoint>& pts);

// Even-odd rule; false for fewer than 3 points or non finite coordinates.
bool insidePolygon(const std::vector<WorldPoint>& poly, float x, float z);

// Distance (metres) from (x, z) to the nearest edge of the closed polygon; infinity for no points.
float distanceToPolygon(const std::vector<WorldPoint>& poly, float x, float z);

// The decision for one camera position: inside the polygon -> true; outside, true only while
// `wasInside` and within Margin of the border (leaving needs Margin metres, entering is at the border).
// Inactive config or non finite coordinates -> false.
bool dlcInside(const DlcAreaConfig& cfg, float x, float z, bool wasInside);

}  // namespace qs::apply
