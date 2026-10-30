// QuattroStagioni - season tick (see season_tick.h).
#include "season_tick.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <sstream>
#include <thread>

#include "apply/apply.h"
#include "io_hooks.h"
#include "log.h"
#include "qs_state.h"
#include "winutil.h"

namespace qs {
namespace {

std::wstring g_iniPath, g_statePath;
std::thread g_thread;
std::atomic<bool> g_stop{false};
std::mutex g_saveMutex;
double g_day = 0;  // owned by the tick thread; read under g_saveMutex for saving
bool g_packs = false;

std::string readText(const std::wstring& path) {
    std::string out;
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return out;
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    fclose(f);
    return out;
}

bool writeTextAtomic(const std::wstring& path, const std::string& text) {
    const std::wstring tmp = path + L".tmp";
    FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (!f) return false;
    bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
    ok = (fclose(f) == 0) && ok;
    if (!ok) return false;
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

bool isSeasonSection(const std::string& line) {
    std::string s;
    for (char c : line) s += char(tolower(static_cast<unsigned char>(c)));
    size_t a = s.find_first_not_of(" \t");
    if (a == std::string::npos || s[a] != '[') return false;
    return s.compare(a, 9, "[seasons]") == 0 || s.compare(a, 8, "[region.") == 0;
}

// B5: empties a non-empty "Active=" inside [Packs] (keeps the line, its comments and every other key).
// Returns true when something changed.
bool clearPacksActive(std::string& ini) {
    std::istringstream in(ini);
    std::string line, out;
    bool inPacks = false, changed = false;
    while (std::getline(in, line)) {
        bool cr = !line.empty() && line.back() == '\r';
        if (cr) line.pop_back();
        std::string low;
        for (char c : line) low += char(tolower(static_cast<unsigned char>(c)));
        size_t a = low.find_first_not_of(" \t");
        if (a != std::string::npos && low[a] == '[') inPacks = low.compare(a, 7, "[packs]") == 0;
        else if (inPacks && a != std::string::npos && low.compare(a, 6, "active") == 0) {
            size_t eq = low.find('=', a);
            if (eq != std::string::npos && low.find_first_not_of(" \t", a + 6) == eq &&
                line.find_first_not_of(" \t", eq + 1) != std::string::npos) {
                line = line.substr(0, eq + 1);
                changed = true;
            }
        }
        out += line + (cr ? "\r\n" : "\n");
    }
    if (!ini.empty() && ini.back() != '\n' && !out.empty()) out.pop_back();
    if (changed) ini = out;
    return changed;
}

// Drops [Seasons] and [Region.*] (with their comment lines just above) and appends the new text.
std::string replaceSeasonSections(const std::string& ini, const std::string& seasonText) {
    std::istringstream in(ini);
    std::string line, out, pendingComments;
    bool skipping = false;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t a = line.find_first_not_of(" \t");
        bool isSection = a != std::string::npos && line[a] == '[';
        bool isComment = a != std::string::npos && (line[a] == ';' || line[a] == '#');
        if (isSection) {
            skipping = isSeasonSection(line);
            if (skipping) pendingComments.clear();
        }
        if (skipping) continue;
        if (isComment || a == std::string::npos) {
            pendingComments += line + "\r\n";
            continue;
        }
        out += pendingComments + line + "\r\n";
        pendingComments.clear();
    }
    out += pendingComments;
    while (out.size() >= 4 && out.compare(out.size() - 4, 4, "\r\n\r\n") == 0) out.resize(out.size() - 2);
    std::string st;
    for (char c : seasonText) {
        if (c == '\n' && (st.empty() || st.back() != '\r')) st += '\r';
        st += c;
    }
    return out + "\r\n" + st;
}

double loadDay(const season::SeasonConfig& cfg) {
    Ini ini;
    ini.parse(readText(g_statePath));
    std::string v = ini.get("State", "Day");
    if (!v.empty()) {
        char* end = nullptr;
        double d = strtod(v.c_str(), &end);
        if (end != v.c_str() && std::isfinite(d) && d >= 0) return d;
    }
    return cfg.startDay;
}

void saveDay(double day) {
    char buf[160];
    snprintf(buf, sizeof buf,
             "; QuattroStagioni - stato (scritto dal plugin, non serve modificarlo)\r\n[State]\r\nDay=%.4f\r\n", day);
    writeTextAtomic(g_statePath, buf);
}

void tickLoop() {
    season::Engine engine;
    season::SeasonConfig cfg;
    {
        std::lock_guard<std::mutex> lk(shared().mutex);
        cfg = shared().config;
    }
    engine.configure(cfg);
    double day = loadDay(cfg);
    std::string lastStage;
    LARGE_INTEGER f, prev, now;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&prev);
    double sinceSave = 0;
    // B3/B5: [Packs] Active= forces one pack at startup only until the calendar changes stage or the
    // user picks a season/day from the F5 menu; then the per-texture calendar takes over.
    bool iniOverride = g_packs && io::packsStatus().forced;
    std::string overrideBase;  // temperate stage when the override was first seen
    auto releaseOverride = [&](const char* why) {
        if (!iniOverride) return;
        iniOverride = false;
        if (!io::packsStatus().forced) return;  // already gone
        const std::string was = io::activePackStage();
        std::string err;
        if (io::setActivePack("", &err))
            QS_INFO("packs: [Packs] Active=%s released (%s): back to the calendar, \"Salva\" in the menu empties Active",
                    was.empty() ? "vanilla" : was.c_str(), why);
        else
            QS_ERR("packs: could not release [Packs] Active=%s (%s): %s", was.c_str(), why, err.c_str());
    };
    while (!g_stop.load()) {
        Sleep(500);
        QueryPerformanceCounter(&now);
        double dt = double(now.QuadPart - prev.QuadPart) / double(f.QuadPart);
        prev = now;
        if (dt > 5.0) dt = 5.0;  // suspended / debugger: do not jump the calendar
        float clockHours = -1.0f, hourOfDay = -1.0f;
        try {
            if (shared().configDirty.exchange(false)) {
                season::SeasonConfig before = cfg;
                {
                    std::lock_guard<std::mutex> lk(shared().mutex);
                    cfg = shared().config;
                }
                engine.configure(cfg);
                // a season or day chosen in the menu (calendar source or manual day changed)
                if (cfg.daySource != before.daySource || cfg.startDay != before.startDay)
                    releaseOverride("season chosen from the F5 menu");
            }
            if (cfg.daySource == season::DaySource::Manual) {
                day = double(cfg.yearIndex(day)) * cfg.yearLength() + cfg.startDay;
            } else {
                // GameClock: game hours from targets.ini (src/win/apply/) when a clock target is active,
                // otherwise playtime until it is.
                double gameDay = 0, gameHours = 0;
                if (cfg.daySource == season::DaySource::GameClock && applyReadGameClock(gameDay, gameHours)) {
                    day = cfg.advance(day, dt, gameHours);
                    if (!applyGameHourOfDay(hourOfDay)) hourOfDay = -1.0f;
                    clockHours = float(gameHours);
                } else {
                    season::SeasonConfig c = cfg;
                    if (c.daySource == season::DaySource::GameClock) c.daySource = season::DaySource::Playtime;
                    day = c.advance(day, dt, -1.0);
                    clockHours = -1.0f;
                }
            }
            if (hourOfDay >= 0.0f) engine.update(day, clockHours, hourOfDay);
            else engine.update(day, clockHours);
            const season::SeasonState& st = engine.state();
            std::string stage = st.stage[int(season::Region::Temperate)].id();
            if (iniOverride) {
                if (overrideBase.empty()) overrideBase = stage;
                else if (stage != overrideBase) releaseOverride(("calendar moved to stage " + stage).c_str());
            }
            // Per-texture packs: cheap when nothing changes, opens/closes packs when needed.
            if (g_packs) io::setSeasonDay(day, cfg);
            std::string packsText;
            if (g_packs) {
                const io::PacksStatus ps = io::packsStatus();
                if (ps.forced) {
                    packsText = "  |  pacchetto fisso " + (ps.forcedStage.empty() ? std::string("vanilla") : ps.forcedStage);
                } else {
                    std::string ids;
                    for (const auto& p : ps.open) ids += (ids.empty() ? "" : " ") + p.stageId;
                    packsText = "  |  pacchetti " + (ids.empty() ? std::string("nessuno") : ids);
                    if (!ps.missingNow.empty()) packsText += " (" + std::to_string(ps.missingNow.size()) + " mancanti)";
                }
            }
            {
                std::lock_guard<std::mutex> lk(shared().mutex);
                shared().season = st;
                shared().statusLine = cfg.dayToLabel(day) + "  |  stadio " + stage + packsText;
            }
            {
                std::lock_guard<std::mutex> lk(g_saveMutex);
                g_day = day;
            }
            if (stage != lastStage) {
                QS_INFO("season: %s, stage %s (temperate calendar; textures follow their own stage)",
                        cfg.dayToLabel(day).c_str(), stage.c_str());
                lastStage = stage;
            }
            sinceSave += dt;
            if (sinceSave >= 30.0) {
                saveDay(day);
                sinceSave = 0;
            }
        } catch (...) {
            QS_ERR("season tick: unexpected exception, season engine stopped");
            return;
        }
    }
    saveDay(day);
}

}  // namespace

bool seasonStart(const std::wstring& scriptsDir, bool packsActive) {
    g_iniPath = joinPath(scriptsDir, L"QuattroStagioni.ini");
    g_statePath = joinPath(joinPath(scriptsDir, L"QuattroStagioni"), L"state.ini");
    g_packs = packsActive;
    Ini ini;
    ini.parse(readText(g_iniPath));
    season::SeasonConfig cfg;
    std::vector<std::string> errors;
    cfg.load(ini, errors);  // starts from defaults; problems are clamped and reported
    for (const auto& e : errors) QS_INFO("season config: %s", e.c_str());
    {
        std::lock_guard<std::mutex> lk(shared().mutex);
        shared().config = cfg;
    }
    QS_INFO("season engine: year %.1f days, %d visual stages, day source %s", cfg.yearLength(), cfg.stageCount(),
            season::toString(cfg.daySource));
    g_stop = false;
    try {
        g_thread = std::thread(tickLoop);
    } catch (...) {
        QS_ERR("season engine: cannot start thread");
        return false;
    }
    return true;
}

void seasonStop() {
    g_stop = true;
    if (g_thread.joinable()) g_thread.join();
}

bool seasonSave(std::string& error) {
    season::SeasonConfig cfg;
    {
        std::lock_guard<std::mutex> lk(shared().mutex);
        cfg = shared().config;
    }
    std::string text = replaceSeasonSections(readText(g_iniPath), cfg.save());
    bool clearedActive = false;
    if (g_packs && !io::packsStatus().forced) clearedActive = clearPacksActive(text);  // B5
    if (!writeTextAtomic(g_iniPath, text)) {
        error = "impossibile scrivere QuattroStagioni.ini";
        return false;
    }
    double day;
    {
        std::lock_guard<std::mutex> lk(g_saveMutex);
        day = g_day;
    }
    saveDay(day);
    QS_INFO("season config saved from the menu%s", clearedActive ? " ([Packs] Active emptied: the calendar chooses the packs)" : "");
    return true;
}

}  // namespace qs
