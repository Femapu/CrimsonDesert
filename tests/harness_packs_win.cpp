// QuattroStagioni - Windows harness for season pack serving through the REAL hooks (run under wine).
//
// Links the plugin's io_hooks/setup code into this exe (no ASI, so no change to main.cpp is needed):
// builds the synthetic archive (tests/synth_archive.h), generates packs A and W with the tint
// transform, installs the hooks, starts the packs and reads every .paz with synchronous ReadFile
// (random unaligned chunks), overlapped ReadFile + GetOverlappedResult and whole entries, checking
// byte-exactly that pack records come back patched and everything else untouched. Then switches the
// pack at runtime (none, W, A) and checks subsequent reads follow.
// Not built by build.sh (it only picks tests/test_*.cpp); build manually:
//   x86_64-w64-mingw32-g++ -std=c++17 -O2 -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DWIN32_LEAN_AND_MEAN -DNOMINMAX
//     -municode -Ithird_party/minhook/include -Isrc/core -Ithird_party/lz4 -Isrc/win tests/harness_packs_win.cpp
//     src/win/io_hooks.cpp src/win/setup.cpp src/win/log.cpp src/win/winutil.cpp src/win/qs_state.cpp
//     $(find src/core -name '*.cpp') build/w_*.o -o build/harness_packs_win.exe -static -static-libgcc -static-libstdc++
//   wine build/harness_packs_win.exe
#include <windows.h>

#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "../src/core/gen/pipeline.h"
#include "../src/core/ini.h"
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

    qs::io::logSummary("harness");
    qs::io::packsStop();
    qs::io::uninstall();
    qs::log::close();
    std::error_code ec;
    std::filesystem::remove_all(std::filesystem::u8path(base), ec);
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
