// QuattroStagioni - camera position and the DLC area exclusion (see world_pos.h).
#include "world_pos.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include "../core/apply/dlc_area.h"
#include "apply/weather_win.h"
#include "log.h"
#include "probe/memory.h"
#include "winutil.h"

namespace qs {
namespace {

// Camera in the climate manager (same offsets as weather_win.cpp / climate_win.cpp; Y is the float
// between X and Z, not checked against the game yet: shown as "probable").
constexpr uintptr_t kCameraX = 0x1001F8, kCameraY = 0x1001FC, kCameraZ = 0x100200;

std::mutex g_mu;
apply::DlcAreaConfig g_cfg;
std::wstring g_borderPath;
WorldPosInfo g_info;
bool g_inside = false;      // tick thread decision (kept while the camera is unknown)
uint64_t g_lastLog = 0;
int g_marked = 0;

const char* areaName(WorldArea a) {
    return a == WorldArea::Dlc ? "DLC" : a == WorldArea::Base ? "base" : "unknown";
}

bool readCamera(float& x, float& y, float& z) {
    const uintptr_t mgr = weatherClimateManager();
    if (!mgr || !probe::mem::readT(mgr + kCameraX, x) || !probe::mem::readT(mgr + kCameraZ, z)) return false;
    if (!std::isfinite(x) || !std::isfinite(z)) return false;
    if (!probe::mem::readT(mgr + kCameraY, y) || !std::isfinite(y)) y = 0;
    return true;
}

}  // namespace

void worldPosConfigure(const Ini& ini, const std::wstring& scriptsDir) {
    apply::DlcAreaConfig cfg;
    std::vector<std::string> warnings;
    cfg.load(ini, warnings);
    for (const std::string& w : warnings) QS_ERR("dlc: %s", w.c_str());
    std::lock_guard<std::mutex> lk(g_mu);
    g_cfg = cfg;
    g_borderPath = joinPath(joinPath(scriptsDir, L"QuattroStagioni"), L"dlc_border.txt");
    g_info.enabled = cfg.enabled;
    g_info.exclusionActive = cfg.active();
    g_info.points = int(cfg.points.size());
    if (cfg.active())
        QS_INFO("dlc: DLC area excluded (%zu points, margin %.0f m): %s", cfg.points.size(), double(cfg.marginM),
                apply::formatPoints(cfg.points).c_str());
    else
        QS_INFO("dlc: DLC area exclusion inactive (%s); mark its border from F5 > Info",
                !cfg.enabled ? "[DLCArea] Enabled=0" : "[DLCArea] Points has fewer than 3 points");
}

bool worldPosTick() {
    float x = 0, y = 0, z = 0;
    const bool known = readCamera(x, y, z);
    const uint64_t now = GetTickCount64();
    std::lock_guard<std::mutex> lk(g_mu);
    if (known) {
        const bool inside = apply::dlcInside(g_cfg, x, z, g_inside);
        if (inside != g_inside)
            QS_INFO(inside ? "dlc: entering the DLC area (%.0f, %.0f): the game's own textures, weather and light"
                           : "dlc: leaving (%.0f, %.0f): back to the seasons",
                    double(x), double(z));
        g_inside = inside;
        g_info.x = x;
        g_info.y = y;
        g_info.z = z;
    }
    g_info.known = known;
    g_info.area = known ? (g_inside ? WorldArea::Dlc : WorldArea::Base) : WorldArea::Unknown;
    g_info.marked = g_marked;
    if (known && g_cfg.logIntervalSec > 0 && now - g_lastLog >= uint64_t(g_cfg.logIntervalSec * 1000.0f)) {
        g_lastLog = now;
        QS_INFO("position: %.1f %.1f %s (y %.1f)", double(x), double(z), areaName(g_info.area), double(y));
    }
    return g_inside;
}

WorldPosInfo worldPosInfo() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_info;
}

bool worldPosMarkBorder(std::string& msg) {
    float x = 0, y = 0, z = 0;
    if (!readCamera(x, y, z)) {
        msg = "position not known yet";
        return false;
    }
    SYSTEMTIME t;
    GetLocalTime(&t);
    char line[160];
    std::snprintf(line, sizeof line, "%.0f, %.0f    # %04u-%02u-%02u %02u:%02u:%02u, y %.0f\r\n", double(x), double(z),
                  unsigned(t.wYear), unsigned(t.wMonth), unsigned(t.wDay), unsigned(t.wHour), unsigned(t.wMinute),
                  unsigned(t.wSecond), double(y));
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_borderPath.empty()) {
        msg = "not configured";
        return false;
    }
    CreateDirectoryW(dirName(g_borderPath).c_str(), nullptr);  // normally there already (state.ini)
    const bool fresh = !fileExists(g_borderPath);
    HANDLE h = CreateFileW(g_borderPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        msg = "cannot open dlc_border.txt (error " + std::to_string(GetLastError()) + ")";
        QS_ERR("dlc: border point %.0f, %.0f not saved: %s", double(x), double(z), msg.c_str());
        return false;
    }
    std::string text;
    if (fresh)
        text = "# QuattroStagioni: border points of the DLC area (world metres x, z), one per line, in walking order.\r\n"
               "# Copy them into QuattroStagioni.ini [DLCArea] Points as x1,z1; x2,z2; ...\r\n";
    text += line;
    DWORD wr = 0;
    const bool ok = WriteFile(h, text.data(), DWORD(text.size()), &wr, nullptr) && wr == DWORD(text.size());
    CloseHandle(h);
    if (!ok) {
        msg = "write to dlc_border.txt failed";
        QS_ERR("dlc: border point %.0f, %.0f not saved: %s", double(x), double(z), msg.c_str());
        return false;
    }
    ++g_marked;
    g_info.marked = g_marked;
    msg = std::string(line, std::strlen(line) - 2);  // without CRLF
    QS_INFO("dlc: border point %d: %s", g_marked, msg.c_str());
    return true;
}

}  // namespace qs
