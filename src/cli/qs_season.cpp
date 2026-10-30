// QuattroStagioni - season timeline: prints a year of the season engine for a given ini (tuning aid).
//
// Usage: qs_season [options] [QuattroStagioni.ini]
//   --step <days>    sampling step (default 0.5)
//   --hour <h>       hour of day of every sample (default 12)
//   --years <n>      years to print (default 1)
//   --from <day>     first day (default 0)
//   --defaults       print the documented default [Seasons]/[Region.*] template and exit
//   --stages         print the list of visual stages and exit
// Snow is integrated between samples (step * 24 game hours), exactly as the plugin would over time.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "ini.h"
#include "season/season.h"

using namespace qs::season;

static const char* kShort[kRegionCount] = {"Tmp", "Nrd", "Pal", "Acq", "Des", "Abi"};

static const char* dominant(const WeatherWeights& w) {
    const char* names[6] = {"sole", "nuvole", "pioggia", "tempor.", "neve", "vento"};
    float v[6] = {w.sunny, w.cloudy, w.rainy, w.storm, w.snowy, w.windy};
    int best = 0;
    for (int i = 1; i < 6; ++i)
        if (v[i] > v[best]) best = i;
    return names[best];
}

static std::string hhmm(float h) {
    int m = int(h * 60.0f + 0.5f);
    char b[16];
    std::snprintf(b, sizeof b, "%02d:%02d", (m / 60) % 24, m % 60);
    return b;
}

int main(int argc, char** argv) {
    double step = 0.5, hour = 12, years = 1, from = 0;
    std::string iniPath;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](double& out) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value after %s\n", a.c_str());
                std::exit(2);
            }
            out = std::atof(argv[++i]);
        };
        if (a == "--step") next(step);
        else if (a == "--hour") next(hour);
        else if (a == "--years") next(years);
        else if (a == "--from") next(from);
        else if (a == "--defaults") {
            std::fputs(defaultIniText().c_str(), stdout);
            return 0;
        } else if (a == "--stages") {
            SeasonConfig c;
            c.setDefaults();
            for (const auto& s : allStages(c)) std::printf("%-7s t=%.3f%s\n", s.id().c_str(), s.t, s.isVanilla() ? "  (vanilla)" : "");
            return 0;
        } else if (a == "-h" || a == "--help") {
            std::puts("usage: qs_season [--step d] [--hour h] [--years n] [--from day] [--defaults] [--stages] [file.ini]");
            return 0;
        } else iniPath = a;
    }
    if (step <= 0.001) step = 0.001;

    SeasonConfig cfg;
    cfg.setDefaults();
    if (!iniPath.empty()) {
        std::ifstream f(iniPath, std::ios::binary);
        if (!f) {
            std::fprintf(stderr, "cannot open %s\n", iniPath.c_str());
            return 1;
        }
        std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        qs::Ini ini;
        ini.parse(text);
        std::vector<std::string> errors;
        if (!cfg.load(ini, errors))
            for (const auto& e : errors) std::printf("ini: %s\n", e.c_str());
    }

    std::printf("year %.2f days, %d visual stages (%d to generate), transition %.0f%% in %d steps, jitter +-%.1f d\n",
                cfg.yearLength(), cfg.stageCount(), cfg.stageCount() - 1, cfg.transitionFraction * 100.0,
                cfg.transitionSteps, cfg.assetJitterDays);
    for (int r = 0; r < kRegionCount; ++r) {
        const RegionConfig& rc = cfg.regions[r];
        std::printf("  %-11s %s", toString(Region(r)), rc.enabled ? "on " : "off");
        if (rc.enabled)
            for (int p = 0; p < kPhaseCount; ++p)
                std::printf("  %c %5.2f+%5.2f", toChar(Phase(p)), cfg.phaseStartDay(Phase(p), Region(r)),
                            cfg.phaseLength(Phase(p), Region(r)));
        std::printf("\n");
    }

    std::vector<int> act;
    for (int r = 0; r < kRegionCount; ++r)
        if (cfg.regions[r].enabled) act.push_back(r);
    std::printf("\n  day  %-27s", "calendario (Tmp)");
    for (int r : act) std::printf(" %-6s", kShort[r]);
    for (int r : act) std::printf(" %5s", kShort[r]);
    std::printf(" ");
    for (int r : act) std::printf(" %4s", kShort[r]);
    std::printf("  meteo   alba  tram. g.x  n.x  fogl\n");
    std::printf("  %4s %-27s", "", "");
    for (size_t i = 0; i < act.size(); ++i) std::printf(" %-6s", i == 0 ? "stadio" : "");
    for (size_t i = 0; i < act.size(); ++i) std::printf(" %5s", i == 0 ? "C" : "");
    std::printf(" ");
    for (size_t i = 0; i < act.size(); ++i) std::printf(" %4s", i == 0 ? "neve" : "");
    std::printf("\n");

    Engine eng;
    eng.configure(cfg);
    double end = from + years * cfg.yearLength();
    for (double d = from; d <= end + 1e-9; d += step) {
        double day = d;
        eng.update(day, float(step * 24.0), float(hour));
        const SeasonState& s = eng.state();
        std::printf("%6.2f %-27s", d, cfg.dayToLabel(day).c_str());
        for (int r : act) std::printf(" %-6s", s.stage[r].id().c_str());
        for (int r : act) std::printf(" %5.1f", s.temperatureC[r]);
        std::printf(" ");
        for (int r : act) std::printf(" %4.2f", s.snowCover[r]);
        std::printf("  %-7s %s %s %4.2f %4.2f %4.2f\n", dominant(s.weather[0]), hhmm(s.sunriseHour).c_str(),
                    hhmm(s.sunsetHour).c_str(), s.dayScale, s.nightScale, s.fallingLeaves);
    }
    return 0;
}
