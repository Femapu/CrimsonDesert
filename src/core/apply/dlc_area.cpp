// QuattroStagioni - the DLC area polygon (see dlc_area.h).
#include "dlc_area.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <sstream>

#include "targets.h"

namespace qs::apply {

namespace {

std::string trimmed(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

}  // namespace

bool parsePoints(const std::string& text, std::vector<WorldPoint>& out, std::string* why) {
    out.clear();
    std::vector<WorldPoint> pts;
    std::stringstream all(text);
    std::string item;
    int n = 0;
    while (std::getline(all, item, ';')) {
        ++n;
        item = trimmed(item);
        if (item.empty()) continue;  // "a; ; b" or a trailing ';'
        std::string flat = item;
        std::replace(flat.begin(), flat.end(), ',', ' ');
        std::stringstream ss(flat);
        std::string tx, tz, extra;
        double x = 0, z = 0;
        if (!(ss >> tx >> tz) || (ss >> extra) || !parseDouble(tx, x) || !parseDouble(tz, z) || !std::isfinite(x) ||
            !std::isfinite(z)) {
            if (why) *why = "point " + std::to_string(n) + " '" + item + "' is not 'x, z' (decimals with a dot)";
            return false;
        }
        pts.push_back({float(x), float(z)});
    }
    out = std::move(pts);
    return true;
}

std::string formatPoints(const std::vector<WorldPoint>& pts) {
    std::string s;
    char buf[64];
    for (const WorldPoint& p : pts) {
        std::snprintf(buf, sizeof buf, "%s%.0f, %.0f", s.empty() ? "" : "; ", double(p.x), double(p.z));
        s += buf;
    }
    return s;
}

bool insidePolygon(const std::vector<WorldPoint>& poly, float x, float z) {
    if (poly.size() < 3 || !std::isfinite(x) || !std::isfinite(z)) return false;
    bool in = false;
    for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
        const double xi = poly[i].x, zi = poly[i].z, xj = poly[j].x, zj = poly[j].z;
        if ((zi > z) != (zj > z) && double(x) < (xj - xi) * (double(z) - zi) / (zj - zi) + xi) in = !in;
    }
    return in;
}

float distanceToPolygon(const std::vector<WorldPoint>& poly, float x, float z) {
    if (poly.empty()) return std::numeric_limits<float>::infinity();
    double best = std::numeric_limits<double>::infinity();
    for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
        const double ax = poly[j].x, az = poly[j].z, bx = poly[i].x, bz = poly[i].z;
        const double dx = bx - ax, dz = bz - az, len2 = dx * dx + dz * dz;
        double t = len2 > 0 ? ((x - ax) * dx + (z - az) * dz) / len2 : 0.0;
        t = std::clamp(t, 0.0, 1.0);
        const double px = ax + t * dx - x, pz = az + t * dz - z;
        best = std::min(best, std::sqrt(px * px + pz * pz));
    }
    return float(best);
}

bool dlcInside(const DlcAreaConfig& cfg, float x, float z, bool wasInside) {
    if (!cfg.active() || !std::isfinite(x) || !std::isfinite(z)) return false;
    if (insidePolygon(cfg.points, x, z)) return true;
    return wasInside && cfg.marginM > 0 && distanceToPolygon(cfg.points, x, z) <= cfg.marginM;
}

void DlcAreaConfig::load(const Ini& ini, std::vector<std::string>& warnings) {
    const char* S = "DLCArea";
    auto num = [&](const char* key, float& out, float lo, float hi) {
        if (!ini.has(S, key)) return;
        double d;
        if (parseDouble(ini.get(S, key), d) && std::isfinite(d)) out = std::clamp(float(d), lo, hi);
        else warnings.push_back(std::string("[DLCArea] ") + key + "=" + ini.get(S, key) + " not a number: default kept");
    };
    if (ini.has(S, "Enabled")) enabled = ini.getInt(S, "Enabled", 1) != 0;
    num("Margin", marginM, 0.0f, 2000.0f);
    num("LogIntervalSec", logIntervalSec, 0.0f, 3600.0f);
    if (ini.has(S, "Points")) {
        std::string why;
        if (!parsePoints(ini.get(S, "Points"), points, &why))
            warnings.push_back("[DLCArea] Points: " + why + ": the DLC area is not excluded");
        else if (!points.empty() && points.size() < 3)
            warnings.push_back("[DLCArea] Points: " + std::to_string(points.size()) +
                               " points, at least 3 are needed: the DLC area is not excluded");
    }
}

}  // namespace qs::apply
