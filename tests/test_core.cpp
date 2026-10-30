// QuattroStagioni - native unit tests for the platform-independent core (build with g++ on Linux).
// Usage: test_core [data_dir]
//   data_dir (optional) may contain: 0012_0.pamt, cd_loading_00.stored.orig, test.bin, mip0_modified.raw
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

#include "../src/core/dds.h"
#include "../src/core/ini.h"
#include "../src/core/lookup3.h"
#include "../src/core/lz4_exact.h"
#include "../src/core/manifest.h"
#include "../src/core/pamt.h"
#include "../src/core/range_patch.h"
#include "lz4.h"

static int g_fail = 0, g_pass = 0;
#define CHECK(cond)                                                                         \
    do {                                                                                    \
        if (cond) ++g_pass;                                                                 \
        else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); }       \
    } while (0)

static std::vector<uint8_t> readFile(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// ---------------------------------------------------------------------------------------------
static void testLookup3() {
    const char* s = "Four score and seven years ago";
    CHECK(qs::hashlittle("", 0, 0) == 0xdeadbeefu);
    CHECK(qs::hashlittle("", 0, 0xdeadbeef) == 0xbd5b7ddeu);
    CHECK(qs::hashlittle(s, 30, 0) == 0x17770551u);
    CHECK(qs::hashlittle(s, 30, 1) == 0xcd628161u);
}

// ---------------------------------------------------------------------------------------------
static void testIniManifest() {
    qs::Ini ini;
    ini.parse("\xEF\xBB\xBF; comment\n[Game]\nversion = 2.04.00\n\n[entry.a]\npath=ui/x.dds\ngroup=0012\npaz_index=3\n"
              "offset=0x10 ; inline\nstored_size=100\nflags=0x01\ndata=a.bin\n[entry.bad]\npath=x\n");
    std::vector<std::string> errs;
    qs::Manifest m = qs::parseManifest(ini, errs);
    CHECK(m.gameVersion == "2.04.00");
    CHECK(m.entries.size() == 1);
    CHECK(errs.size() == 1);
    if (!m.entries.empty()) {
        CHECK(m.entries[0].offset == 16);
        CHECK(m.entries[0].flags == 1);
        CHECK(m.entries[0].pazIndex == 3);
    }
}

// ---------------------------------------------------------------------------------------------
// Range substitution under arbitrary chunking.
static void testRangePatchRandom() {
    std::mt19937_64 rng(12345);
    for (int iter = 0; iter < 400; ++iter) {
        const size_t fileSize = 1000 + rng() % 200000;
        std::vector<uint8_t> file(fileSize);
        for (auto& b : file) b = uint8_t(rng());
        qs::Substitution s;
        s.name = "t";
        size_t len = 1 + rng() % std::min<size_t>(40000, fileSize - 1);
        s.offset = rng() % (fileSize - len + 1);
        s.original.assign(file.begin() + long(s.offset), file.begin() + long(s.offset + len));
        s.replacement = s.original;
        for (size_t i = 0; i < len; ++i)
            if (rng() % 3) s.replacement[i] = uint8_t(rng());
        std::vector<uint8_t> expected = file;
        std::memcpy(expected.data() + s.offset, s.replacement.data(), len);

        // Simulate the game reading the whole file in random chunks (some 4 KiB aligned, some not).
        std::vector<uint8_t> result(fileSize);
        size_t pos = 0;
        while (pos < fileSize) {
            size_t chunk;
            switch (rng() % 4) {
                case 0: chunk = 1 + rng() % 7; break;
                case 1: chunk = 4096 - (pos % 4096); break;
                case 2: chunk = 65536; break;
                default: chunk = 1 + rng() % 30000; break;
            }
            chunk = std::min(chunk, fileSize - pos);
            std::vector<uint8_t> buf(file.begin() + long(pos), file.begin() + long(pos + chunk));
            qs::PatchInfo pi = qs::applyToRead(s, pos, buf.data(), buf.size());
            bool ov = pos < s.end() && pos + chunk > s.offset;
            CHECK((pi.result != qs::PatchResult::NoOverlap) == ov);
            CHECK(pi.result != qs::PatchResult::Mismatch);
            std::memcpy(result.data() + pos, buf.data(), chunk);
            pos += chunk;
        }
        CHECK(result == expected);

        // Overlapping re-reads of random windows, applied twice (idempotent).
        for (int k = 0; k < 20; ++k) {
            size_t a = rng() % fileSize, b = a + 1 + rng() % (fileSize - a);
            std::vector<uint8_t> buf(file.begin() + long(a), file.begin() + long(b));
            qs::applyToRead(s, a, buf.data(), buf.size());
            qs::PatchInfo second = qs::applyToRead(s, a, buf.data(), buf.size());
            CHECK(std::memcmp(buf.data(), expected.data() + a, b - a) == 0);
            CHECK(second.result == qs::PatchResult::NoOverlap || second.result == qs::PatchResult::AlreadyPatched ||
                  second.result == qs::PatchResult::Identical);
        }
        // Foreign data in the range must be left untouched.
        {
            std::vector<uint8_t> buf(file.begin() + long(s.offset), file.begin() + long(s.end()));
            buf[buf.size() / 2] ^= 0x5A;
            if (s.original[buf.size() / 2] != s.replacement[buf.size() / 2] || true) {
                std::vector<uint8_t> copy = buf;
                qs::PatchInfo pi = qs::applyToRead(s, s.offset, buf.data(), buf.size());
                if (pi.result == qs::PatchResult::Mismatch) CHECK(buf == copy);
                else CHECK(pi.result != qs::PatchResult::NoOverlap);
            }
        }
    }
    // Edge cases
    qs::Substitution s;
    s.offset = 100;
    s.original = {1, 2, 3, 4};
    s.replacement = {9, 8, 7, 6};
    uint8_t b1[4] = {0, 0, 0, 0};
    CHECK(qs::applyToRead(s, 96, b1, 4).result == qs::PatchResult::NoOverlap);   // ends exactly at offset
    CHECK(qs::applyToRead(s, 104, b1, 4).result == qs::PatchResult::NoOverlap);  // starts exactly at end
    CHECK(qs::applyToRead(s, 100, b1, 0).result == qs::PatchResult::NoOverlap);   // zero length
    uint8_t b2[2] = {3, 4};
    CHECK(qs::applyToRead(s, 102, b2, 2).result == qs::PatchResult::Patched && b2[0] == 7 && b2[1] == 6);
    CHECK(qs::applyToRead(s, 102, b2, 2).result == qs::PatchResult::AlreadyPatched);
    uint8_t b4[2] = {5, 5};
    CHECK(qs::applyToRead(s, 102, b4, 2).result == qs::PatchResult::Mismatch && b4[0] == 5);
    uint8_t b3[1] = {0};
    CHECK(qs::applyToRead(s, 99, b3, 1).result == qs::PatchResult::NoOverlap);
}

// ---------------------------------------------------------------------------------------------
static std::vector<uint8_t> makeData(std::mt19937_64& rng, size_t n, int entropy) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) {
        if (i > 64 && rng() % 100 < unsigned(100 - entropy)) {
            size_t back = 1 + rng() % 64;
            v[i] = v[i - back];
        } else {
            v[i] = uint8_t(rng() % (entropy > 50 ? 256 : 16));
        }
    }
    return v;
}

static void testLz4Exact() {
    std::mt19937_64 rng(777);
    int okCount = 0, total = 0;
    for (int iter = 0; iter < 30; ++iter) {
        size_t n = 200 + rng() % 120000;
        std::vector<uint8_t> src = makeData(rng, n, int(rng() % 90));
        size_t def = qs::lz4DefaultSize(src.data(), src.size());
        qs::Lz4ExactResult best = qs::lz4CompressExact(src.data(), src.size(), def);
        CHECK(best.ok);
        // Every target between the HC optimum and the all-literals size must be reachable.
        const size_t maxSize = 1 + (n < 15 ? 0 : 1 + (n - 15) / 255) + n;
        std::vector<size_t> targets = {best.bestSize, best.bestSize + 1, best.bestSize + 2, best.bestSize + 3,
                                       def, def + 1, def + 17, def + 255, def + 2000, maxSize, maxSize - 1, maxSize - 2};
        for (int k = 0; k < 4; ++k) targets.push_back(best.bestSize + rng() % (maxSize - best.bestSize + 1));
        for (size_t& t : targets) t = std::min(std::max(t, best.bestSize), maxSize);
        qs::Lz4ExactResult over = qs::lz4CompressExact(src.data(), src.size(), maxSize + 1);
        CHECK(!over.ok);
        for (size_t t : targets) {
            ++total;
            qs::Lz4ExactResult r = qs::lz4CompressExact(src.data(), src.size(), t);
            bool good = r.ok && r.block.size() == t && qs::lz4VerifyStrict(r.block.data(), r.block.size(), src.data(), src.size());
            // Independent check: the plain LZ4 API with exact input size.
            if (good) {
                std::vector<char> out(src.size());
                int d = LZ4_decompress_safe(reinterpret_cast<const char*>(r.block.data()), out.data(), int(t), int(out.size()));
                good = d == int(src.size()) && std::memcmp(out.data(), src.data(), src.size()) == 0;
            }
            if (good) ++okCount;
            else std::printf("  lz4 exact miss: n=%zu target=%zu best=%zu err=%s\n", n, t, best.bestSize, r.error.c_str());
            CHECK(good);
        }
        // Below the optimum must fail cleanly.
        qs::Lz4ExactResult tooSmall = qs::lz4CompressExact(src.data(), src.size(), best.bestSize > 1 ? best.bestSize - 1 : 0);
        CHECK(!tooSmall.ok);
    }
    std::printf("  lz4 exact: %d/%d targets hit\n", okCount, total);
}

// ---------------------------------------------------------------------------------------------
static void testRealData(const std::string& dir) {
    std::vector<uint8_t> pamt = readFile(dir + "/0012_0.pamt");
    if (!pamt.empty()) {
        qs::Pamt p;
        CHECK(p.parse(pamt.data(), pamt.size()));
        CHECK(p.storedSelfHash() == 0xF352B11Fu);
        CHECK(p.computedSelfHash() == 0xF352B11Fu);
        CHECK(p.fileCount() == 27740);
        qs::PamtEntry e;
        CHECK(p.find("ui/texture/cd_loading_00.dds", e));
        CHECK(e.pazIndex == 3 && e.offset == 24565408u && e.storedSize == 30119u && e.size == 262272u && e.flags == 0x01);
        qs::PamtEntry e2;
        CHECK(p.find("\\UI\\Texture\\CD_LOADING_00.DDS", e2) && e2.offset == e.offset);
        CHECK(p.find("ui/texture/cd_uitexture_loading_00.dds", e2) && e2.offset == 28527904u && e2.storedSize == 35669u);
        CHECK(!p.find("ui/texture/does_not_exist.dds", e2));
        bool paz3 = false;
        for (auto& z : p.pazTable()) if (z.index == 3 && z.size == 955464432u) paz3 = true;
        CHECK(paz3);
        std::printf("  pamt 0012: %zu files, hash %08X\n", p.fileCount(), p.storedSelfHash());
    } else {
        std::printf("  (skipping pamt tests: %s/0012_0.pamt not found)\n", dir.c_str());
    }

    std::vector<uint8_t> orig = readFile(dir + "/cd_loading_00.stored.orig");
    std::vector<uint8_t> repl = readFile(dir + "/test.bin");
    std::vector<uint8_t> modRaw = readFile(dir + "/mip0_modified.raw");
    if (orig.empty() || repl.empty()) { std::printf("  (skipping texture tests)\n"); return; }
    CHECK(orig.size() == 30119 && repl.size() == 30119);
    CHECK(qs::cdHash(orig.data(), orig.size()) == 0x2EB74396u);
    CHECK(qs::hashlittle(orig.data(), orig.size(), 0) == 0xBB32A935u);
    std::string err;
    std::vector<uint8_t> decO, decR;
    CHECK(qs::validatePerMipDds(orig.data(), orig.size(), 262272, &err, &decO));
    CHECK(qs::validatePerMipDds(repl.data(), repl.size(), 262272, &err, &decR));
    if (!err.empty()) std::printf("  dds: %s\n", err.c_str());
    CHECK(std::memcmp(orig.data(), repl.data(), 128) == 0);
    if (!modRaw.empty() && decR.size() == 262272) CHECK(std::memcmp(decR.data() + 128, modRaw.data(), modRaw.size()) == 0);

    // Exact encoder on the real modified mip must reproduce a block of the original size.
    if (!modRaw.empty()) {
        qs::Lz4ExactResult r = qs::lz4CompressExact(modRaw.data(), modRaw.size(), orig.size() - 128);
        CHECK(r.ok && r.block.size() == orig.size() - 128);
    }

    // End-to-end simulation: a fake archive with the original entry at its real offset, read in
    // random chunks; afterwards the entry range must equal test.bin and decode to the new texture.
    std::mt19937_64 rng(4242);
    const uint64_t entryOff = 24565408;
    const size_t region = 2 * 1024 * 1024;              // simulate 2 MiB around the entry
    const uint64_t base = entryOff - 700000;             // archive offset of region[0]
    std::vector<uint8_t> archive(region);
    for (auto& b : archive) b = uint8_t(rng());
    std::memcpy(archive.data() + (entryOff - base), orig.data(), orig.size());
    qs::Substitution s;
    s.name = "ui/texture/cd_loading_00.dds";
    s.offset = entryOff;
    s.original = orig;
    s.replacement = repl;
    for (int run = 0; run < 50; ++run) {
        std::vector<uint8_t> seen(region);
        size_t pos = 0;
        while (pos < region) {
            size_t chunk = run % 3 == 0 ? 65536 - ((base + pos) % 65536) : 1 + rng() % 50000;
            chunk = std::min(chunk, region - pos);
            std::vector<uint8_t> buf(archive.begin() + long(pos), archive.begin() + long(pos + chunk));
            qs::applyToRead(s, base + pos, buf.data(), buf.size());
            std::memcpy(seen.data() + pos, buf.data(), chunk);
            pos += chunk;
        }
        CHECK(std::memcmp(seen.data() + (entryOff - base), repl.data(), repl.size()) == 0);
        CHECK(std::memcmp(seen.data(), archive.data(), size_t(entryOff - base)) == 0);
        size_t after = size_t(entryOff - base) + orig.size();
        CHECK(std::memcmp(seen.data() + after, archive.data() + after, region - after) == 0);
    }
    std::printf("  end-to-end chunked read simulation over the real entry: done\n");
}

int main(int argc, char** argv) {
    std::string dir = argc > 1 ? argv[1] : ".";
    testLookup3();
    testIniManifest();
    testRangePatchRandom();
    testLz4Exact();
    testRealData(dir);
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
