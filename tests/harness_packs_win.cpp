// QuattroStagioni - Windows harness for season pack serving through the REAL hooks (run under wine).
//
// Links the plugin's io_hooks/setup code into this exe (no ASI, so no change to main.cpp is needed):
// builds the synthetic archive (tests/synth_archive.h), generates packs A and W with the tint
// transform, installs the hooks, starts the packs and reads every .paz with synchronous ReadFile
// (random unaligned chunks), overlapped ReadFile + GetOverlappedResult and whole entries, checking
// byte-exactly that pack records come back patched and everything else untouched. Then switches the
// pack at runtime (none, W, A) and checks subsequent reads follow. Finally the per-texture season mode:
// io::setSeasonDay with packs A, A>W.1, A>W.3, W (A>W.2 missing -> fallback), reads compared with the
// core assignment (pack/multi.h), status API, override on top of the season mode and back.
// Not built by build.sh (it only picks tests/test_*.cpp); build manually:
//   x86_64-w64-mingw32-g++ -std=c++17 -O2 -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DWIN32_LEAN_AND_MEAN -DNOMINMAX
//     -municode -Ithird_party/minhook/include -Isrc/core -Ithird_party/lz4 -Isrc/win -Ithird_party/rgbcx -Ithird_party/stb tests/harness_packs_win.cpp
//     src/win/io_hooks.cpp src/win/setup.cpp src/win/log.cpp src/win/winutil.cpp src/win/qs_state.cpp
//     $(find src/core -name '*.cpp') build/w_*.o -o build/harness_packs_win.exe -static -static-libgcc -static-libstdc++
//   wine build/harness_packs_win.exe
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "../src/core/gen/pipeline.h"
#include "../src/core/ini.h"
#include "../src/core/pack/multi.h"
#include "../src/core/pack/qsp_io.h"
#include "io_hooks.h"
#include "log.h"
#include "setup.h"
#include "synth_archive.h"
#include "winutil.h"

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

struct PazCase {
    std::wstring path;
    std::vector<uint8_t> orig, expectA, expectW;
};

static bool generateMulti(const std::string& root, const std::string& classesJson, const std::vector<std::string>& ids,
                          const std::string& dir) {
    qs::gen::GameArchive ar;
    qs::gen::Inventory inv;
    std::vector<std::string> errs;
    if (!ar.open(root) || !qs::gen::parseInventory(classesJson, inv, errs)) return false;
    auto tr = qs::gen::makeTintTransform();
    std::vector<qs::gen::StageSpec> stages;
    std::vector<std::string> outs;
    for (const std::string& id : ids) {
        qs::gen::StageSpec st;
        qs::gen::parseStage(id, 3, st);
        stages.push_back(st);
        outs.push_back(dir + "\\" + st.fileName());
    }
    qs::gen::GenOptions o;
    o.threads = 2;
    std::vector<qs::gen::GenSummary> sums;
    std::string err;
    bool ok = qs::gen::runGenerationMulti(ar, inv.items, *tr, stages, outs, o, sums, &err);
    if (!ok) std::printf("multi generation failed: %s\n", err.c_str());
    return ok;
}

static bool generate(const std::string& root, const std::string& classesJson, const char* stage, const std::string& out) {
    qs::gen::GameArchive ar;
    qs::gen::Inventory inv;
    std::vector<std::string> errs;
    if (!ar.open(root) || !qs::gen::parseInventory(classesJson, inv, errs)) return false;
    auto tr = qs::gen::makeTintTransform();
    qs::gen::GenOptions o;
    qs::gen::parseStage(stage, 3, o.stage);
    o.threads = 2;
    o.outPath = out;
    qs::gen::GenSummary s;
    std::string err;
    bool ok = qs::gen::runGeneration(ar, inv.items, *tr, o, s, &err);
    if (!ok) std::printf("generation %s failed: %s\n", stage, err.c_str());
    return ok && s.records > 0;
}

// Expected paz bytes for the season mode at `day`: the core assignment over every pack in `files`.
static std::vector<uint8_t> expectedSeason(const std::vector<std::string>& files, const qs::season::SeasonConfig& cfg,
                                           double day, const std::string& key, const std::vector<uint8_t>& orig,
                                           size_t* served = nullptr) {
    std::vector<std::shared_ptr<qs::pack::LoadedPack>> packs;
    for (const std::string& f : files) {
        std::shared_ptr<qs::pack::LoadedPack> lp(new qs::pack::LoadedPack());
        lp->server.reset(new qs::pack::PackServer());
        if (!lp->server->open(qs::pack::openFileSource(f), 1 << 20)) continue;
        lp->stageId = lp->server->reader().header().stageId;
        packs.push_back(lp);
    }
    auto idx = qs::pack::TextureIndex::build(packs);
    auto snap = qs::pack::assignStages(idx, cfg, day);
    if (served) *served = snap->servedTextures;
    std::vector<uint8_t> e = orig;
    const int slot = idx->findPaz(key);
    if (slot < 0) return e;
    const qs::pack::TextureIndex::Paz& z = idx->paz()[size_t(slot)];
    for (uint32_t t = z.first; t < z.first + z.count; ++t) {
        if (snap->serve[t] < 0) continue;
        const qs::pack::PackReader& r = idx->packs()[size_t(snap->serve[t])]->server->reader();
        std::vector<uint8_t> pl;
        r.readPayload(r.records()[idx->record(t, size_t(snap->serve[t]))], pl);
        std::memcpy(e.data() + idx->textures()[t].offset, pl.data(), pl.size());
    }
    return e;
}

static std::vector<uint8_t> expected(const std::string& packPath, const std::string& key, const std::vector<uint8_t>& orig) {
    std::vector<uint8_t> e = orig;
    qs::pack::PackReader r;
    if (!r.openFile(packPath)) return e;
    int slot = r.findPaz(key);
    if (slot < 0) return e;
    const qs::pack::PazInfo& z = r.paz()[size_t(slot)];
    for (uint32_t k = z.firstRecord; k < z.firstRecord + z.recordCount; ++k) {
        std::vector<uint8_t> pl;
        r.readPayload(r.records()[k], pl);
        std::memcpy(e.data() + r.records()[k].offset, pl.data(), pl.size());
    }
    return e;
}

static std::vector<uint8_t> readSyncChunks(const std::wstring& p, size_t total, std::mt19937& rng, size_t maxChunk) {
    std::vector<uint8_t> out(total);
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    size_t pos = 0;
    while (pos < total) {
        DWORD want = DWORD(std::min<size_t>(1 + rng() % maxChunk, total - pos)), got = 0;
        if (!ReadFile(h, out.data() + pos, want, &got, nullptr) || got != want) break;
        pos += got;
    }
    CloseHandle(h);
    out.resize(pos);
    return out;
}

static std::vector<uint8_t> readOverlapped(const std::wstring& p, size_t total, size_t chunk) {
    std::vector<uint8_t> out(total);
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    for (size_t pos = 0; pos < total; pos += chunk) {
        OVERLAPPED ov = {};
        ov.Offset = DWORD(pos);
        ov.hEvent = ev;
        DWORD want = DWORD(std::min(chunk, total - pos)), got = 0;
        ResetEvent(ev);
        if (!ReadFile(h, out.data() + pos, want, nullptr, &ov) && GetLastError() != ERROR_IO_PENDING) { out.clear(); break; }
        if (!GetOverlappedResult(h, &ov, &got, TRUE) || got != want) { out.clear(); break; }
    }
    CloseHandle(ev);
    CloseHandle(h);
    return out;
}

int wmain() {
    wchar_t tmpw[MAX_PATH];
    GetTempPathW(MAX_PATH, tmpw);
    const std::string base = qs::narrow(tmpw) + "qs_harness_packs";
    const std::string root = base + "\\game";
    synth::Archive arc;
    if (!synth::build(root, arc)) { std::printf("cannot build synthetic archive\n"); return 1; }
    // meta/0.paver with the 2.04.00 bytes the version guard expects (version numbers only).
    CreateDirectoryW(qs::widen(root + "\\meta").c_str(), nullptr);
    std::vector<uint8_t> paver;
    qs::parseHex("0200040000000100000006000000646c635f30310000000047396a31", paver);
    qs::pack::writeWholeFile(root + "\\meta\\0.paver", paver.data(), paver.size());
    const std::string scripts = base + "\\scripts", packs = scripts + "\\QuattroStagioni\\packs";
    std::filesystem::create_directories(std::filesystem::u8path(packs));
    CHECK(generate(root, arc.classesJson, "A", packs + "\\A.qsp"));
    CHECK(generate(root, arc.classesJson, "W", packs + "\\W.qsp"));

    std::vector<PazCase> cases;
    for (const char* key : {"0001/0.paz", "0001/1.paz", "dlc/dlc_01/0015/0.paz"}) {
        PazCase c;
        std::string k = key;
        std::string rel = k;
        for (char& ch : rel) if (ch == '/') ch = '\\';
        c.path = qs::widen(root + "\\" + rel);
        qs::pack::readWholeFile(root + "\\" + rel, c.orig);
        c.expectA = expected(packs + "\\A.qsp", k, c.orig);
        c.expectW = expected(packs + "\\W.qsp", k, c.orig);
        CHECK(c.expectA != c.orig && c.expectW != c.orig && c.expectA != c.expectW);
        cases.push_back(c);
    }

    // Generated before the hooks are installed (qs_gen must read the original bytes); the folder is
    // rescanned when the override ends below. Kept aside until then so availablePacks() sees 2 files.
    const std::string later = base + "\\later";
    std::filesystem::create_directories(std::filesystem::u8path(later));
    CHECK(generateMulti(root, arc.classesJson, {"A>W.1", "A>W.3"}, later));

    qs::Config cfg;
    cfg.testMode = true;
    cfg.gameRootOverride = qs::widen(root);
    cfg.activePack = "A";
    cfg.packCacheMB = 1;
    cfg.logLevel = 1;
    qs::log::setLevel(cfg.logLevel);
    qs::log::open(qs::widen(scripts + "\\QuattroStagioni.log"));
    qs::SetupResult setup = qs::runSetup(cfg, qs::widen(scripts));
    CHECK(setup.versionOk);
    CHECK(qs::io::install(cfg, setup.gameRoot, std::move(setup.targets)));
    CHECK(qs::io::packsStart(cfg, setup.gameRoot, qs::widen(scripts)));
    CHECK(qs::io::activePackStage() == "A");
    CHECK(qs::io::availablePacks().size() == 2);

    std::mt19937 rng(4242);
    auto checkAll = [&](int which, const char* what) {
        for (const PazCase& c : cases) {
            const std::vector<uint8_t>& want = which == 0 ? c.orig : which == 1 ? c.expectA : c.expectW;
            std::vector<uint8_t> a = readSyncChunks(c.path, c.orig.size(), rng, 9000);
            std::vector<uint8_t> b = readSyncChunks(c.path, c.orig.size(), rng, 70);
            std::vector<uint8_t> o4 = readOverlapped(c.path, c.orig.size(), 4096);
            std::vector<uint8_t> o5 = readOverlapped(c.path, c.orig.size(), 5000);
            bool ok = a == want && b == want && o4 == want && o5 == want;
            if (!ok) std::printf("  mismatch (%s) in %s: sync9000 %d sync70 %d ov4096 %d ov5000 %d\n", what,
                                 qs::narrow(c.path).c_str(), a == want, b == want, o4 == want, o5 == want);
            CHECK(ok);
        }
    };
    checkAll(1, "pack A");
    std::string why;
    CHECK(qs::io::setActivePack("none", &why));
    checkAll(0, "no pack");
    CHECK(qs::io::setActivePack("W", &why) && qs::io::activePackStage() == "W");
    checkAll(2, "pack W");
    CHECK(!qs::io::setActivePack("S", &why));  // not generated: refused, W stays active
    CHECK(qs::io::activePackStage() == "W");
    CHECK(qs::io::setActivePack("A.qsp", &why));
    checkAll(1, "pack A again");

    // ---- per-texture season mode ----------------------------------------------------------------
    for (const char* f : {"A-W.1.qsp", "A-W.3.qsp"})
        MoveFileW(qs::widen(later + "\\" + f).c_str(), qs::widen(packs + "\\" + f).c_str());
    CHECK(qs::io::setActivePack("none", &why));   // end the override: no season day yet -> vanilla
    CHECK(qs::io::activePackStage().empty());
    checkAll(0, "override ended, no day");
    qs::season::SeasonConfig scfg;
    scfg.setDefaults();
    const double wStart = scfg.phaseStartDay(qs::season::Phase::Winter);
    const std::vector<std::string> files = {packs + "\\A.qsp", packs + "\\A-W.1.qsp", packs + "\\A-W.3.qsp",
                                            packs + "\\W.qsp"};
    auto checkSeason = [&](double day, const char* what) {
        for (const PazCase& c : cases) {
            std::string key = qs::narrow(c.path).substr(root.size() + 1);
            for (char& ch : key) if (ch == '\\') ch = '/';
            const std::vector<uint8_t> want = expectedSeason(files, scfg, day, key, c.orig);
            std::vector<uint8_t> a = readSyncChunks(c.path, c.orig.size(), rng, 9000);
            std::vector<uint8_t> o5 = readOverlapped(c.path, c.orig.size(), 5000);
            bool ok = a == want && o5 == want;
            if (!ok) std::printf("  mismatch (%s) in %s: sync9000 %d ov5000 %d\n", what, key.c_str(), a == want, o5 == want);
            CHECK(ok);
        }
    };
    double day = wStart - 0.8;
    qs::io::setSeasonDay(day, scfg);
    qs::io::PacksStatus ps = qs::io::packsStatus();
    CHECK(ps.started && ps.seasonDriven && !ps.forced);
    CHECK(ps.open.size() >= 3);
    CHECK(std::find(ps.missingNow.begin(), ps.missingNow.end(), "A>W.2") != ps.missingNow.end());
    CHECK(ps.texturesServed > 0 && ps.texturesFallback > 0 && ps.texturesIndexed >= ps.texturesServed);
    CHECK(ps.available.size() == 4 && ps.planDays == 7 && !ps.plan.stages.empty());
    CHECK(ps.cacheLimitBytes == (1u << 20) && ps.indexBytes > 0);
    size_t served = 0;
    expectedSeason(files, scfg, day, "0001/0.paz", cases[0].orig, &served);
    CHECK(ps.texturesServed == served);
    checkSeason(day, "season day 1");
    ps = qs::io::packsStatus();
    uint64_t patched = 0, fallbackPatched = 0;
    for (const auto& p : ps.open) { patched += p.patched; fallbackPatched += p.fallbackPatched; CHECK(p.formatVersion == 2); }
    CHECK(patched > 0 && fallbackPatched > 0);
    // Days pass: the assignment follows per texture (several snapshots, some packs closed/opened).
    for (double d2 : {wStart - 0.3, wStart + 0.6, wStart + 2.5}) {
        for (double x = day; x < d2; x += 0.01) qs::io::setSeasonDay(x, scfg);
        day = d2;
        qs::io::setSeasonDay(day, scfg);
        checkSeason(day, "season sweep");
    }
    CHECK(qs::io::packsStatus().snapshotSwaps > 3);
    qs::pack::StagePlan plan = qs::io::planStages(30);
    CHECK(plan.missingCount > 0 && plan.presentCount > 0);
    // Override on top of the season mode, then back.
    CHECK(qs::io::setActivePack("A", &why) && qs::io::activePackStage() == "A");
    CHECK(qs::io::packsStatus().forced && qs::io::openPackStages().size() == 1);
    checkAll(1, "override A in season mode");
    qs::io::setSeasonDay(day + 0.01, scfg);       // ignored while forced
    checkAll(1, "override A after a tick");
    CHECK(qs::io::setActivePack("U", &why) && qs::io::packsStatus().forced);
    checkAll(0, "override vanilla");
    CHECK(qs::io::setActivePack("none", &why) && qs::io::packsStatus().seasonDriven);
    checkSeason(day + 0.01, "season after override");
    // Late summer: L and U>L have no pack, the walk back reaches vanilla summer.
    qs::io::setSeasonDay(scfg.phaseStartDay(qs::season::Phase::LateSummer) + 1.0, scfg);
    checkAll(0, "late summer, no packs");

    qs::io::logSummary("harness");
    qs::io::packsStop();
    qs::io::uninstall();
    qs::log::close();
    std::error_code ec;
    std::filesystem::remove_all(std::filesystem::u8path(base), ec);
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
