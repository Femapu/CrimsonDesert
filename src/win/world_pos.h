// QuattroStagioni - camera position in world coordinates and the DLC area exclusion.
//
// The position is the camera the climate manager keeps (the same X/Z the weather and the climate
// modules use for the desert: [mgr+0x1001F8] X, [mgr+0x100200] Z; Y read from +0x1001FC, the float in
// between, assumed to be the height). It is read by the season tick (every 500 ms) through
// worldPosTick(), which also decides whether the camera is in the "Charting the Unknown" DLC area
// ([DLCArea] polygon, src/core/apply/dlc_area.h). While it is, the season tick publishes the season
// state as if every region were excluded (regionActive false: weather, snow, temperatures, particles,
// light, lightning and day length leave the game alone, each with its own fade) and serves the
// textures as the game's for new reads (all regions disabled for io::setSeasonDay), like the desert.
//
// Border capture: the F5 Info tab shows the position and the area; its button appends "x, z" with the
// date to bin64\scripts\QuattroStagioni\dlc_border.txt (and the log).
#pragma once
#include <string>

#include "../core/ini.h"

namespace qs {

// Area of the camera: unknown (camera not readable yet), the base game, or the DLC area.
enum class WorldArea : int { Unknown = -1, Base = 0, Dlc = 1 };

struct WorldPosInfo {
    bool known = false;          // camera read in the last tick
    float x = 0, y = 0, z = 0;   // world metres (y: probable height)
    WorldArea area = WorldArea::Unknown;
    bool exclusionActive = false;  // [DLCArea] Enabled with a polygon of at least 3 points
    bool enabled = true;           // [DLCArea] Enabled
    int points = 0;                // polygon points
    int marked = 0;                // border points marked in this session
};

// Reads [DLCArea] from the main ini (called by seasonStart before the tick starts).
void worldPosConfigure(const Ini& ini, const std::wstring& scriptsDir);

// One step (season tick thread): reads the camera, decides the area, logs transitions
// ("dlc: entering the DLC area" / "dlc: leaving") and, every LogIntervalSec, "position: x z area".
// Returns true while the camera is in the DLC area (exclusion on).
bool worldPosTick();

// Last values, for the menu. Cheap.
WorldPosInfo worldPosInfo();

// Appends the current position to dlc_border.txt and logs it. False (with `msg` saying why) when the
// position is unknown or the file cannot be written; `msg` is the line written on success.
bool worldPosMarkBorder(std::string& msg);

}  // namespace qs
