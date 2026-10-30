// QuattroStagioni - qs_gen, the season pack generator (runs on the user's PC, reads only).
//
// Reads the user's own Crimson Desert archives, applies the seasonal recipes to the inventory textures
// for one visual stage and writes QuattroStagioni\packs\<stage>.qsp, which the plugin serves.
// Nothing in the game folder is ever written. See docs/GENERATORE.md (Italian) for usage.
//
//   qs_gen --game "<Crimson Desert dir>" --stage A>W.2[,W,...|@file] --out <dir> [--classes f] [--recipes d]
//          [--transform recipes|tint] [--steps N] [--config ini] [--threads N] [--only REGEX] [--dry-run]
//   qs_gen --plan [--days N] [--day D | --state state.ini] [--config ini] [--packs dir]
//          [--stages-needed-file f]   (no --game needed)
//   qs_gen --game "<dir>" --verify-inventory [--quick] [--report file]
//   qs_gen --list-stages [--steps N]
//   qs_gen --check-pack <file.qsp> [--game "<dir>"]
#include <cmath>
#include <cstdarg>
#include <map>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "gen/archive.h"
#include "gen/inventory.h"
#include "gen/pipeline.h"
#include "gen/stage.h"
#include "gen/transform.h"
#include "gen/verify.h"
#include "ini.h"
#include "pack/plan.h"
#include "pack/qsp_io.h"
#include "season/season.h"

namespace fs = std::filesystem;
using namespace qs;

namespace {

const char* const kVersion = "qs_gen 0.4.0";

struct Args {
    std::string game, classes = "data/vegetation_classes_v1.json", recipes = "data/recipes", transform = "recipes";
    std::string stage, out, only, report, checkPack;
    std::string config, state, packs, neededFile, day;
    int steps = 3, threads = 0, days = 7;
    bool stepsGiven = false;
    bool verify = false, quick = false, listStages = false, dryRun = false, quiet = false, help = false, plan = false;
};

void usage() {
    std::printf(
        "%s - QuattroStagioni season pack generator (reads the game files, never writes them)\n\n"
        "Generate one or more stages (several stages share one read pass over the archives):\n"
        "  qs_gen --game \"<Crimson Desert dir>\" --stage <id>[,<id>...] --out <dir> [options]\n"
        "  qs_gen --game \"<Crimson Desert dir>\" --stage @<file> --out <dir>   ids from a file (--stages-needed-file)\n"
        "    --classes <file>     inventory (default data/vegetation_classes_v1.json)\n"
        "    --recipes <dir>      recipe folder (default data/recipes)\n"
        "    --transform <name>   recipes (default) or tint (pipeline test without recipes)\n"
        "    --steps <N>          transition stages per phase change (default 3, or TransitionSteps of --config)\n"
        "    --config <ini>       QuattroStagioni.ini with the [Seasons] settings of the plugin\n"
        "    --threads <N>        worker threads (default: all cores); output is identical for any N\n"
        "    --only <regex>       only archive paths matching the regex (case-insensitive)\n"
        "    --dry-run            select and check only, write nothing\n"
        "    --quiet              no progress line\n"
        "Other commands:\n"
        "  qs_gen --game \"<dir>\" --verify-inventory [--quick] [--report <file>]\n"
        "        re-reads every inventory entry from the live pamt (--quick: index only, no decode)\n"
        "  qs_gen --list-stages [--steps N]\n"
        "  qs_gen --plan [--days N] [--day D | --state <state.ini>] [--config <ini>] [--packs <dir>]\n"
        "        [--stages-needed-file <file>]   stages needed from day D for the next N days (default 7)\n"
        "        across regions and per-asset jitter: present / missing packs and estimated MB; the file\n"
        "        lists the missing ids for qs_gen --stage @<file>. --packs defaults to --out.\n"
        "  qs_gen --check-pack <file.qsp> [--game \"<dir>\"]   integrity (and game binding) of a pack\n"
        "Stage ids: S U L A W (plateaus) and X>Y.n transitions (S>U, U>L, L>A, A>W, W>S), e.g. A>W.2.\n"
        "Exit codes: 0 ok, 1 error, 2 usage, 3 verification found problems.\n",
        kVersion);
}

bool parseArgs(int argc, char** argv, Args& a, std::string& err) {
    for (int i = 1; i < argc; ++i) {
        std::string k = argv[i];
        auto val = [&](std::string& dst) {
            if (i + 1 >= argc) { err = k + " needs a value"; return false; }
            dst = argv[++i];
            return true;
        };
        auto num = [&](int& dst) {
            std::string v;
            if (!val(v)) return false;
            char* end = nullptr;
            long n = std::strtol(v.c_str(), &end, 10);
            if (!end || *end || n < 0 || n > 1024) { err = k + ": bad number " + v; return false; }
            dst = int(n);
            return true;
        };
        bool ok = true;
        if (k == "--game") ok = val(a.game);
        else if (k == "--classes") ok = val(a.classes);
        else if (k == "--recipes") ok = val(a.recipes);
        else if (k == "--transform") ok = val(a.transform);
        else if (k == "--stage") ok = val(a.stage);
        else if (k == "--out") ok = val(a.out);
        else if (k == "--only") ok = val(a.only);
        else if (k == "--report") ok = val(a.report);
        else if (k == "--check-pack") ok = val(a.checkPack);
        else if (k == "--steps") { ok = num(a.steps); a.stepsGiven = true; }
        else if (k == "--days") ok = num(a.days);
        else if (k == "--day") ok = val(a.day);
        else if (k == "--config") ok = val(a.config);
        else if (k == "--state") ok = val(a.state);
        else if (k == "--packs") ok = val(a.packs);
        else if (k == "--stages-needed-file") { ok = val(a.neededFile); a.plan = true; }
        else if (k == "--plan") a.plan = true;
        else if (k == "--threads") ok = num(a.threads);
        else if (k == "--verify-inventory") a.verify = true;
        else if (k == "--quick") a.quick = true;
        else if (k == "--list-stages") a.listStages = true;
        else if (k == "--dry-run") a.dryRun = true;
        else if (k == "--quiet") a.quiet = true;
        else if (k == "--help" || k == "-h" || k == "/?") a.help = true;
        else { err = "unknown option " + k; return false; }
        if (!ok) return false;
    }
    return true;
}

// Log to stdout and (optionally) a log file.
FILE* g_log = nullptr;
void out(const char* fmt, ...) __attribute__((format(gnu_printf, 1, 2)));
void out(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char buf[4096];
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::fputs(buf, stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
    if (g_log) { std::fputs(buf, g_log); std::fputc('\n', g_log); std::fflush(g_log); }
}

std::string mb(uint64_t b) {
    char s[32];
    std::snprintf(s, sizeof(s), "%.1f MB", double(b) / 1048576.0);
    return s;
}

bool loadItems(const Args& a, gen::Inventory& inv) {
    std::vector<std::string> errs;
    bool ok = gen::loadInventory(a.classes, inv, errs);
    for (const auto& e : errs) out("classes: %s", e.c_str());
    if (!ok) return false;
    out("inventory: %s, %zu entries (version %d, %s)", a.classes.c_str(), inv.items.size(), inv.version, inv.game.c_str());
    return true;
}

int cmdListStages(const Args& a) {
    for (const gen::StageSpec& s : gen::allStages(a.steps))
        std::printf("%-7s file %-12s %s\n", s.id().c_str(), s.fileName().c_str(),
                    s.plateau() ? (s.from == 'U' ? "summer = vanilla, nothing to generate" : "plateau") : "transition");
    return 0;
}

int cmdVerify(const Args& a, gen::GameArchive& ar) {
    gen::Inventory inv;
    if (!loadItems(a, inv)) return 1;
    const auto t0 = std::chrono::steady_clock::now();
    auto last = t0;
    gen::VerifyReport r = gen::verifyInventory(ar, inv.items, !a.quick, [&](size_t done, size_t total) {
        auto now = std::chrono::steady_clock::now();
        if (a.quiet || (now - last < std::chrono::milliseconds(500) && done != total)) return;
        last = now;
        std::fprintf(stderr, "\r  verifying %zu/%zu", done, total);
        if (done == total) std::fprintf(stderr, "\n");
    });
    FILE* rep = a.report.empty() ? nullptr : pack::fopenUtf8(a.report, "wb");
    size_t shown = 0;
    for (const auto& i : r.issues) {
        if (rep) std::fprintf(rep, "%s\t%s\t%s\t%s\n", i.kind.c_str(), i.group.c_str(), i.path.c_str(), i.detail.c_str());
        if (shown++ < 60) out("  %-8s %s/%s: %s", i.kind.c_str(), i.group.c_str(), i.path.c_str(), i.detail.c_str());
    }
    if (shown > 60) out("  ... %zu more (use --report <file>)", shown - 60);
    if (rep) std::fclose(rep);
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    out("verify-inventory: %zu checked, %zu ok, %zu missing, %zu index mismatch, %zu decode errors, %zu header mismatch, "
        "%zu shadowed by DLC, %zu group errors; %s read in %.1f s (%s)",
        r.checked, r.ok, r.missing, r.indexMismatch, r.decodeErrors, r.headerMismatch, r.shadowed, r.groupErrors,
        mb(r.bytesRead).c_str(), secs, a.quick ? "index only" : "full decode");
    out(r.clean() ? "RESULT: inventory matches this game installation" : "RESULT: PROBLEMS FOUND (see above)");
    return r.clean() ? 0 : 3;
}

int cmdCheckPack(const Args& a) {
    pack::PackReader pr;
    std::string err;
    if (!pr.openFile(a.checkPack, &err)) { out("check-pack: %s", err.c_str()); return 3; }
    const pack::Header& h = pr.header();
    out("pack %s: stage %s, %zu records in %zu paz files, payload %s, index %zu bytes", a.checkPack.c_str(),
        h.stageId.c_str(), pr.records().size(), pr.paz().size(), mb(pr.payloadSize()).c_str(), pr.indexBytes());
    out("  generator \"%s\", recipe version %u, game build \"%s\"", h.generator.c_str(), h.recipeVersion, h.gameBuild.c_str());
    {
        size_t perRegion[season::kRegionCount] = {}, meta = 0;
        for (const pack::Record& r : pr.records()) {
            if (r.hasMeta) ++meta;
            if (r.region < season::kRegionCount) perRegion[r.region]++;
        }
        std::string regions;
        for (int r = 0; r < season::kRegionCount; ++r)
            if (perRegion[r]) regions += std::string(regions.empty() ? "" : ", ") + season::toString(season::Region(r)) + " " + std::to_string(perRegion[r]);
        out("  format v%u, transition steps %u, %s", h.formatVersion, h.transitionSteps,
            meta ? ("regions: " + regions).c_str() : "no region/seed data (v1: every texture follows the Temperate calendar)");
    }
    int rc = 0;
    if (!pr.verifyAll(&err)) { out("  PAYLOAD CHECK FAILED: %s", err.c_str()); rc = 3; }
    else out("  every payload hash verified");
    if (!a.game.empty()) {
        gen::GameArchive ar;
        if (!ar.open(a.game, &err)) { out("  %s", err.c_str()); return 1; }
        uint64_t es = 0;
        uint32_t ts = 0;
        std::string build = ar.exeBuild(es, ts) ? pack::gameBuildId(es, ts) : "unreadable";
        bool ok = build == h.gameBuild;
        out("  game build now \"%s\" -> %s", build.c_str(), ok ? "OK" : "STALE");
        for (const auto& g : h.pamtHashes) {
            const gen::Group* gg = ar.group(g.first, &err);
            uint32_t now = gg ? gg->pamt.storedSelfHash() : 0;
            out("  %s/0.pamt %08X (pack %08X) %s", g.first.c_str(), now, g.second, now == g.second ? "OK" : "STALE");
            ok = ok && now == g.second;
        }
        if (!ok) { out("  RESULT: pack does not match this game version: regenerate it"); rc = 3; }
    }
    if (rc == 0) out("RESULT: pack OK");
    return rc;
}

// [Seasons] settings: defaults, or the plugin's QuattroStagioni.ini (--config).
bool loadSeasonConfig(const Args& a, season::SeasonConfig& cfg) {
    cfg.setDefaults();
    if (a.config.empty()) return true;
    std::vector<uint8_t> raw;
    if (!pack::readWholeFile(a.config, raw)) { out("config: cannot read %s", a.config.c_str()); return false; }
    Ini ini;
    ini.parse(std::string(raw.begin(), raw.end()));
    std::vector<std::string> errs;
    cfg.load(ini, errs);
    for (const auto& e : errs) out("config: %s", e.c_str());
    return true;
}

int effectiveSteps(const Args& a, const season::SeasonConfig& cfg) {
    if (a.stepsGiven || a.config.empty()) return a.steps;
    return cfg.transitionSteps >= 1 ? cfg.transitionSteps : a.steps;
}

// "--stage A,W,A>W.1" or "--stage @file" (ids separated by commas, spaces or new lines; '#' comments).
bool parseStageList(const std::string& arg, int steps, std::vector<gen::StageSpec>& outStages, std::string& err) {
    std::string text = arg;
    if (!arg.empty() && arg[0] == '@') {
        std::vector<uint8_t> raw;
        if (!pack::readWholeFile(arg.substr(1), raw)) { err = "cannot read " + arg.substr(1); return false; }
        text.clear();
        bool comment = false;
        for (uint8_t c : raw) {
            if (c == '#') comment = true;
            if (c == '\n') comment = false;
            if (!comment) text += char(c);
        }
    }
    std::string cur;
    std::vector<std::string> ids;
    for (char c : text + ",") {
        if (c == ',' || c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            if (!cur.empty()) ids.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    for (const std::string& id : ids) {
        gen::StageSpec st;
        if (!gen::parseStage(id, steps, st, &err)) return false;
        if (st.plateau() && st.from == 'U') { out("stage U is summer = vanilla: nothing to generate, skipped"); continue; }
        bool dup = false;
        for (const gen::StageSpec& s : outStages) dup = dup || s.id() == st.id();
        if (!dup) outStages.push_back(st);
    }
    if (outStages.empty() && err.empty()) err = "no stage to generate";
    return !outStages.empty();
}

int cmdPlan(const Args& a) {
    season::SeasonConfig cfg;
    if (!loadSeasonConfig(a, cfg)) return 1;
    double day = cfg.startDay;
    if (!a.day.empty()) {
        char* end = nullptr;
        day = std::strtod(a.day.c_str(), &end);
        if (!end || *end || !std::isfinite(day)) { out("--day: bad number %s", a.day.c_str()); return 2; }
    } else if (!a.state.empty()) {
        std::vector<uint8_t> raw;
        Ini ini;
        if (pack::readWholeFile(a.state, raw)) ini.parse(std::string(raw.begin(), raw.end()));
        const std::string v = ini.get("State", "Day");
        if (v.empty()) out("state: no [State] Day= in %s, using the start day", a.state.c_str());
        else day = std::strtod(v.c_str(), nullptr);
    }
    // Packs present: every *.qsp of the packs folder whose index opens (stage id from the header).
    const std::string dir = !a.packs.empty() ? a.packs : a.out;
    std::vector<std::string> present;
    std::map<std::string, uint64_t> presentBytes;
    std::error_code ec;
    if (!dir.empty() && fs::is_directory(fs::u8path(dir), ec)) {
        for (const auto& e : fs::directory_iterator(fs::u8path(dir), ec)) {
            if (pack::stageIdFromFileName(e.path().filename().u8string()).empty()) continue;
            pack::PackReader pr;
            std::string err;
            if (!pr.openFile(e.path().u8string(), &err)) { out("packs: %s ignored: %s", e.path().u8string().c_str(), err.c_str()); continue; }
            present.push_back(pr.header().stageId);
            presentBytes[pr.header().stageId] = pr.payloadSize();
        }
    }
    // Regions and MB estimates from the inventory when it is available (no game files needed).
    gen::Inventory inv;
    std::vector<std::string> errs;
    const bool haveInv = gen::loadInventory(a.classes, inv, errs);
    uint32_t mask = 0;
    if (haveInv)
        for (const gen::Item& it : inv.items) mask |= 1u << int(gen::regionOf(it));
    else
        out("inventory %s not readable: all enabled regions, no MB estimate", a.classes.c_str());
    mask = mask ? (mask & pack::enabledRegionMask(cfg)) : pack::enabledRegionMask(cfg);
    const int steps = cfg.transitionSteps;
    pack::StagePlan plan = pack::planStages(cfg, day, double(a.days), mask, present, [&](const std::string& id) -> uint64_t {
        auto it = presentBytes.find(id);
        if (it != presentBytes.end()) return it->second;
        return haveInv ? gen::estimateStageBytes(inv.items, id, steps) : 0;
    });
    out("%s: plan for %d days from day %.2f, %d transition steps, jitter +-%.1f days, packs in %s", kVersion, a.days, day,
        steps, double(cfg.assetJitterDays), dir.empty() ? "(none given)" : dir.c_str());
    std::string text = plan.text(cfg);
    if (!text.empty() && text.back() == '\n') text.pop_back();
    out("%s", text.c_str());
    std::string open;
    for (const auto& id : pack::packsToOpen(cfg, plan, present)) open += (open.empty() ? "" : ", ") + id;
    out("the plugin would keep open now: %s", open.empty() ? "(none: vanilla)" : open.c_str());
    if (!a.neededFile.empty()) {
        std::string f = "# QuattroStagioni: stages missing for the next " + std::to_string(a.days) + " days (qs_gen --plan)\n";
        f += "# generate them with: qs_gen --game \"<dir>\" --stage @<this file> --out <packs dir>\n";
        for (const auto& id : plan.missingIds()) f += id + "\n";
        if (!pack::writeWholeFile(a.neededFile, f.data(), f.size())) { out("cannot write %s", a.neededFile.c_str()); return 1; }
        out("missing stages written to %s (%zu)", a.neededFile.c_str(), plan.missingCount);
    }
    return 0;
}

int cmdGenerate(const Args& a, gen::GameArchive& ar) {
    std::string err;
    season::SeasonConfig cfg;
    if (!loadSeasonConfig(a, cfg)) return 1;
    std::vector<gen::StageSpec> stages;
    if (!parseStageList(a.stage, effectiveSteps(a, cfg), stages, err)) {
        out("%s", err.empty() ? "nothing to generate" : err.c_str());
        return err.empty() ? 0 : 2;
    }
    std::unique_ptr<gen::TextureTransform> tr;
    if (a.transform == "tint") {
        tr = gen::makeTintTransform();
    } else if (a.transform == "recipes") {
        tr = gen::makeRecipeTransform(a.recipes, &err);
        if (!tr) { out("recipes: %s", err.c_str()); return 1; }
    } else {
        out("unknown transform %s (use recipes or tint)", a.transform.c_str());
        return 2;
    }
    gen::Inventory inv;
    if (!loadItems(a, inv)) return 1;

    gen::GenOptions o;
    o.threads = a.threads > 0 ? a.threads : int(std::max(1u, std::thread::hardware_concurrency()));
    o.only = a.only;
    o.dryRun = a.dryRun;
    o.generator = kVersion;
    std::vector<std::string> outPaths;
    std::string ids;
    for (const gen::StageSpec& st : stages) ids += (ids.empty() ? "" : ", ") + st.id();
    std::error_code ec;
    if (!a.dryRun) {
        fs::create_directories(fs::u8path(a.out), ec);
        for (const gen::StageSpec& st : stages) outPaths.push_back((fs::u8path(a.out) / st.fileName()).u8string());
        const std::string base = stages.size() == 1 ? stages[0].fileName().substr(0, stages[0].fileName().size() - 4) : "batch";
        std::string logPath = (fs::u8path(a.out) / ("qs_gen_" + base + ".log")).u8string();
        g_log = pack::fopenUtf8(logPath, "wb");
        if (g_log) out("log: %s", logPath.c_str());
    }
    if (stages.size() == 1)
        out("%s: stage %s (t %.3f), transform %s, %d threads%s%s", kVersion, stages[0].id().c_str(), stages[0].t,
            tr->name().c_str(), o.threads, a.only.empty() ? "" : (", only /" + a.only + "/").c_str(), a.dryRun ? ", DRY RUN" : "");
    else
        out("%s: %zu stages in one pass (%s), transform %s, %d threads%s%s", kVersion, stages.size(), ids.c_str(),
            tr->name().c_str(), o.threads, a.only.empty() ? "" : (", only /" + a.only + "/").c_str(), a.dryRun ? ", DRY RUN" : "");
    out("game: %s (%zu groups)", ar.root().c_str(), ar.groupNames().size());

    const auto t0 = std::chrono::steady_clock::now();
    auto last = t0;
    o.log = [&](const std::string& s) {
        if (!a.quiet) std::fprintf(stderr, "\r%*s\r", 100, "");
        out("  %s", s.c_str());
    };
    o.progress = [&](size_t done, size_t total, const gen::GenSummary& s) {
        if (a.quiet) return;
        auto now = std::chrono::steady_clock::now();
        if (now - last < std::chrono::milliseconds(400) && done != total) return;
        last = now;
        double el = std::chrono::duration<double>(now - t0).count();
        double eta = done ? el * double(total - done) / double(done) : 0;
        std::fprintf(stderr, "\r  [%5zu/%zu %3.0f%%] fit %zu (+bias %zu, mild %zu) vanilla %zu err %zu  ETA %.0f s   ",
                     done, total, total ? 100.0 * double(done) / double(total) : 100.0, s.fit, s.fitBias, s.fitMild,
                     s.vanilla, s.errors + s.indexMismatch, eta);
        if (done == total) std::fprintf(stderr, "\n");
    };
    std::vector<gen::GenSummary> sums;
    bool ok = gen::runGenerationMulti(ar, inv.items, *tr, stages, outPaths, o, sums, &err);
    if (!ok) {
        out("ERROR: %s", err.c_str());
        if (g_log) std::fclose(g_log);
        return 1;
    }
    int rc = 0;
    for (size_t i = 0; i < stages.size(); ++i) {
        const gen::GenSummary& s = sums[i];
        out("---- summary, stage %s ----", stages[i].id().c_str());
        out("selected %zu of %zu inventory textures; the stage changes %zu (%s stored)", s.selected, s.total, s.affected,
            mb(s.affectedBytes).c_str());
        if (a.dryRun) {
            out("dry run: %zu index mismatches, nothing written (%.1f s)", s.indexMismatch, s.seconds);
            if (s.indexMismatch) rc = 3;
            continue;
        }
        out("fit %zu at full quality, %zu with fitBias, %zu at reduced strength; %zu kept vanilla (no fit)", s.fit,
            s.fitBias, s.fitMild, s.vanilla);
        out("unaffected %zu, identical %zu, index mismatch %zu, errors %zu", s.unaffected, s.identical, s.indexMismatch,
            s.errors);
        out("FIT RATE %.2f%% (%zu of %zu attempted)", 100.0 * s.fitRate(), s.fit + s.fitBias + s.fitMild, s.attempted());
        out("pack %s: %zu records, %s payload, %.1f s", outPaths[i].c_str(), s.records, mb(s.packedBytes).c_str(), s.seconds);
        if (s.errors || s.indexMismatch) rc = 3;
    }
    if (g_log) std::fclose(g_log);
    return rc;
}

}  // namespace

int main(int argc, char** argv) {
    Args a;
    std::string err;
    if (!parseArgs(argc, argv, a, err)) {
        std::fprintf(stderr, "qs_gen: %s (see --help)\n", err.c_str());
        return 2;
    }
    if (a.help || argc < 2) { usage(); return argc < 2 ? 2 : 0; }
    if (a.listStages) return cmdListStages(a);
    if (!a.checkPack.empty()) return cmdCheckPack(a);
    if (a.plan) return cmdPlan(a);
    if (a.game.empty()) { std::fprintf(stderr, "qs_gen: --game is required (see --help)\n"); return 2; }
    gen::GameArchive ar;
    if (!ar.open(a.game, &err)) { std::fprintf(stderr, "qs_gen: %s\n", err.c_str()); return 1; }
    if (a.verify) return cmdVerify(a, ar);
    if (a.stage.empty()) { std::fprintf(stderr, "qs_gen: --stage is required (see --help)\n"); return 2; }
    if (a.out.empty() && !a.dryRun) { std::fprintf(stderr, "qs_gen: --out is required (see --help)\n"); return 2; }
    return cmdGenerate(a, ar);
}
