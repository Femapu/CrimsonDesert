// QuattroStagioni - qs_gen, the season pack generator (runs on the user's PC, reads only).
//
// Reads the user's own Crimson Desert archives, applies the seasonal recipes to the inventory textures
// for one visual stage and writes QuattroStagioni\packs\<stage>.qsp, which the plugin serves.
// Nothing in the game folder is ever written. See docs/GENERATORE.md (Italian) for usage.
//
//   qs_gen --game "<Crimson Desert dir>" --stage A>W.2 --out <dir> [--classes f] [--recipes d]
//          [--transform recipes|tint] [--steps N] [--threads N] [--only REGEX] [--dry-run]
//   qs_gen --game "<dir>" --verify-inventory [--quick] [--report file]
//   qs_gen --list-stages [--steps N]
//   qs_gen --check-pack <file.qsp> [--game "<dir>"]
#include <cstdarg>
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
#include "pack/qsp_io.h"

namespace fs = std::filesystem;
using namespace qs;

namespace {

const char* const kVersion = "qs_gen 0.3.0";

struct Args {
    std::string game, classes = "data/vegetation_classes_v1.json", recipes = "data/recipes", transform = "recipes";
    std::string stage, out, only, report, checkPack;
    int steps = 3, threads = 0;
    bool verify = false, quick = false, listStages = false, dryRun = false, quiet = false, help = false;
};

void usage() {
    std::printf(
        "%s - QuattroStagioni season pack generator (reads the game files, never writes them)\n\n"
        "Generate one stage:\n"
        "  qs_gen --game \"<Crimson Desert dir>\" --stage <id> --out <dir> [options]\n"
        "    --classes <file>     inventory (default data/vegetation_classes_v1.json)\n"
        "    --recipes <dir>      recipe folder (default data/recipes)\n"
        "    --transform <name>   recipes (default) or tint (pipeline test without recipes)\n"
        "    --steps <N>          transition stages per phase change (default 3)\n"
        "    --threads <N>        worker threads (default: all cores); output is identical for any N\n"
        "    --only <regex>       only archive paths matching the regex (case-insensitive)\n"
        "    --dry-run            select and check only, write nothing\n"
        "    --quiet              no progress line\n"
        "Other commands:\n"
        "  qs_gen --game \"<dir>\" --verify-inventory [--quick] [--report <file>]\n"
        "        re-reads every inventory entry from the live pamt (--quick: index only, no decode)\n"
        "  qs_gen --list-stages [--steps N]\n"
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
        else if (k == "--steps") ok = num(a.steps);
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

int cmdGenerate(const Args& a, gen::GameArchive& ar) {
    gen::StageSpec st;
    std::string err;
    if (!gen::parseStage(a.stage, a.steps, st, &err)) { out("%s", err.c_str()); return 2; }
    if (st.plateau() && st.from == 'U') { out("stage U is summer = vanilla: nothing to generate"); return 0; }
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
    o.stage = st;
    o.threads = a.threads > 0 ? a.threads : int(std::max(1u, std::thread::hardware_concurrency()));
    o.only = a.only;
    o.dryRun = a.dryRun;
    o.generator = kVersion;
    std::error_code ec;
    if (!a.dryRun) {
        fs::create_directories(fs::u8path(a.out), ec);
        o.outPath = (fs::u8path(a.out) / st.fileName()).u8string();
        std::string logPath = (fs::u8path(a.out) / ("qs_gen_" + st.fileName().substr(0, st.fileName().size() - 4) + ".log")).u8string();
        g_log = pack::fopenUtf8(logPath, "wb");
        if (g_log) out("log: %s", logPath.c_str());
    }
    out("%s: stage %s (t %.3f), transform %s, %d threads%s%s", kVersion, st.id().c_str(), st.t, tr->name().c_str(),
        o.threads, a.only.empty() ? "" : (", only /" + a.only + "/").c_str(), a.dryRun ? ", DRY RUN" : "");
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
    gen::GenSummary s;
    bool ok = gen::runGeneration(ar, inv.items, *tr, o, s, &err);
    if (!ok) {
        out("ERROR: %s", err.c_str());
        if (g_log) std::fclose(g_log);
        return 1;
    }
    out("---- summary, stage %s ----", st.id().c_str());
    out("selected %zu of %zu inventory textures; the stage changes %zu (%s stored)", s.selected, s.total, s.affected,
        mb(s.affectedBytes).c_str());
    if (a.dryRun) {
        out("dry run: %zu index mismatches, nothing written (%.1f s)", s.indexMismatch, s.seconds);
        return s.indexMismatch ? 3 : 0;
    }
    out("fit %zu at full quality, %zu with fitBias, %zu at reduced strength; %zu kept vanilla (no fit)", s.fit, s.fitBias,
        s.fitMild, s.vanilla);
    out("unaffected %zu, identical %zu, index mismatch %zu, errors %zu", s.unaffected, s.identical, s.indexMismatch, s.errors);
    out("FIT RATE %.2f%% (%zu of %zu attempted)", 100.0 * s.fitRate(), s.fit + s.fitBias + s.fitMild, s.attempted());
    out("pack %s: %zu records, %s payload, %.1f s", o.outPath.c_str(), s.records, mb(s.packedBytes).c_str(), s.seconds);
    if (g_log) std::fclose(g_log);
    return (s.errors || s.indexMismatch) ? 3 : 0;
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
    if (a.game.empty()) { std::fprintf(stderr, "qs_gen: --game is required (see --help)\n"); return 2; }
    gen::GameArchive ar;
    if (!ar.open(a.game, &err)) { std::fprintf(stderr, "qs_gen: %s\n", err.c_str()); return 1; }
    if (a.verify) return cmdVerify(a, ar);
    if (a.stage.empty()) { std::fprintf(stderr, "qs_gen: --stage is required (see --help)\n"); return 2; }
    if (a.out.empty() && !a.dryRun) { std::fprintf(stderr, "qs_gen: --out is required (see --help)\n"); return 2; }
    return cmdGenerate(a, ar);
}
