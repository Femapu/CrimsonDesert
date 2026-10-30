// QuattroStagioni - tests for the generator and pack stream (native, no game files needed).
// Usage: test_gen [data_dir]            run all tests (data_dir unused)
//        test_gen --write-synth <dir>   only write the synthetic game folder + classes.json (wine check)
//
// Builds a synthetic archive (tests/synth_archive.h) in a temp folder, runs the generator pipeline with
// a test-only transform (no texture codec involved), writes a pack, serves simulated game reads
// through PackServer and checks the served bytes decode to the transformed textures.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include "../src/core/crypto/chacha20.h"
#include "../src/core/gen/archive.h"
#include "../src/core/gen/inventory.h"
#include "../src/core/gen/json.h"
#include "../src/core/gen/pipeline.h"
#include "../src/core/gen/stage.h"
#include "../src/core/gen/transform.h"
#include "../src/core/gen/verify.h"
#include "../src/core/lookup3.h"
#include "../src/core/pack/qsp_io.h"
#include "../src/core/pack/serve.h"
#include "synth_archive.h"

namespace fs = std::filesystem;
using namespace qs;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond)                                                                         \
    do {                                                                                    \
        if (cond) ++g_pass;                                                                 \
        else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); }       \
    } while (0)

static std::string hex(const uint8_t* p, size_t n) {
    static const char* h = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) { s += h[p[i] >> 4]; s += h[p[i] & 15]; }
    return s;
}

// ---------------------------------------------------------------------------------------------
// ChaCha20: RFC 7539 2.4.2 and vectors produced by tools/cdformats.py (chacha_key_iv / chacha_xor).
static void testChaCha() {
    uint8_t key[32], iv[16] = {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x4a, 0, 0, 0, 0};
    for (int i = 0; i < 32; ++i) key[i] = uint8_t(i);
    std::string m = "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, "
                    "sunscreen would be it.";
    std::vector<uint8_t> buf(m.begin(), m.end());
    crypto::chacha20Xor(key, iv, buf.data(), buf.size());
    CHECK(hex(buf.data(), buf.size()) ==
          "6e2e359a2568f98041ba0728dd0d6981e97e7aec1d4360c20a27afccfd9fae0bf91b65c5524733ab8f593dabcd62b3571639d624e65152ab"
          "8f530c359f0861d807ca0dbf500d6a6156a38e088a22b65e52bc514d16ccf806818ce91ab77937365af90bbf74a35be6b40b8eedf2785e42874d");
    crypto::chacha20Xor(key, iv, buf.data(), buf.size());
    CHECK(std::string(buf.begin(), buf.end()) == m);

    struct V { const char* name; const char* key; const char* iv; };
    const V vs[] = {
        {"cd_loading_00.xml", "75c575db7fcf7fd179c979d773c373dd7bcb7bd57fcf7fd173c373dd77c777d9", "16a714bb16a714bb16a714bb16a714bb"},
        {"UI/Data/Some_Config.XML", "1dca076417c00d6e11c60b681bcc016213c4096a17c00d6e1bcc01621fc80566", "7ea866047ea866047ea866047ea86604"},
        {"a", "d641d324dc4bd92eda4ddf28d047d522d84fdd2adc4bd92ed047d522d443d126", "b523b244b523b244b523b244b523b244"},
        {"gamedata/x/stringtable_eng.paloc", "6ad8407c60d24a7666d44c706cde467a64d64e7260d24a766cde467a68da427e", "09ba211c09ba211c09ba211c09ba211c"},
    };
    for (const V& v : vs) {
        crypto::ChaChaKeyIv k = crypto::deriveKeyIv(v.name);
        CHECK(hex(k.key, 32) == v.key);
        CHECK(hex(k.iv, 16) == v.iv);
    }
    CHECK(crypto::keyMaterialFromTag(0x610E0232u) == crypto::kDefaultKeyMaterial);
    // Different key material changes the key (pycrimson general form).
    CHECK(hex(crypto::deriveKeyIv("a", 0x11).key, 32) != vs[2].key);

    std::vector<uint8_t> pt(300);
    for (int i = 0; i < 300; ++i) pt[size_t(i)] = uint8_t((i * 7 + 3) & 255);
    crypto::cryptEntry("some/dir/cd_loading_00.xml", pt.data(), pt.size());
    CHECK(hex(pt.data(), pt.size()) ==
          "9717372585254914bd01a55b52eb47b9e8234797ec80792a572be2004c50a82c896ed43cec43b6a72b0765a04663ad9d11847594c5c3d1af7ff4"
          "94270d5c6cc68ab127cddd74f8b83b436f093e63ad5862eec81feec88234aa3d34087cc4f1069d642f684bf1d092f3f595a7343ba5401f20e7"
          "8a77e7a199bb0e852e521289852c33ee4951000c5ec34362d090ed5a554d1b282787e00b706a5505a9c9d78bf8591c8e1c6712cf802dcbb2d0"
          "5ec9fe83e54b4e99ba2a8d248c33e7ddf78eb1e9ca19b4e4a97ea4fb80900155533004866518ac43c8ebd90f494fe3c2fe405aeec57370696a"
          "0e425a4c4a30c20d146285932502a5ad038e93a3372324336266483203579d8fdce49c6ccd0c3747ef544325f2f12940a8140821a1d2a7ba7b"
          "7994b9ef7119e3d5220cd7d5137b");
    // Counter carry (OpenSSL style): block at counter 0xFFFFFFFF followed by counter 0 with nonce word+1.
    uint8_t iv2[16] = {0xFF, 0xFF, 0xFF, 0xFF, 5, 0, 0, 0, 9, 9, 9, 9, 7, 7, 7, 7};
    uint8_t iv3[16] = {0, 0, 0, 0, 6, 0, 0, 0, 9, 9, 9, 9, 7, 7, 7, 7};
    std::vector<uint8_t> a(128, 0), b(64, 0);
    crypto::chacha20Xor(key, iv2, a.data(), a.size());
    crypto::chacha20Xor(key, iv3, b.data(), b.size());
    CHECK(std::memcmp(a.data() + 64, b.data(), 64) == 0);
}

// ---------------------------------------------------------------------------------------------
static void testJson() {
    json::Value v;
    std::string err;
    CHECK(json::parse("{\"a\":[1,2.5,-3,true,null,\"x\\u00e9\\ud83d\\ude00\\n\"],\"b\":{}}", v, &err));
    CHECK(v["a"].size() == 6);
    CHECK(v["a"][0].asInt() == 1);
    CHECK(v["a"][1].asDouble() == 2.5);
    CHECK(v["a"][2].asInt() == -3);
    CHECK(v["a"][3].asBool());
    CHECK(v["a"][4].isNull());
    CHECK(v["a"][5].asString() == "x\xC3\xA9\xF0\x9F\x98\x80\n");
    CHECK(v["b"].isObject() && v["missing"].isNull());
    CHECK(!json::parse("{\"a\":1,}", v, &err));
    CHECK(!json::parse("[1 2]", v, &err));
    CHECK(!json::parse("\"abc", v, &err));
    CHECK(!json::parse("01", v, &err));
    CHECK(json::parse("  [ ]  ", v, &err) && v.isArray() && v.size() == 0);
    CHECK(json::parse("1234567890123", v, &err) && v.asInt() == 1234567890123ll);
    // The real classes file of the repo (paths and numbers only) must parse.
    gen::Inventory inv;
    std::vector<std::string> errs;
    if (fs::exists("data/vegetation_classes_v1.json")) {
        CHECK(gen::loadInventory("data/vegetation_classes_v1.json", inv, errs));
        CHECK(errs.empty());
        CHECK(inv.items.size() == 3688);
    }
}

static void testStages() {
    gen::StageSpec s;
    CHECK(gen::parseStage("A>W.2", 3, s) && s.from == 'A' && s.to == 'W' && s.step == 2 && s.t == 0.5f);
    CHECK(s.id() == "A>W.2" && s.fileName() == "A-W.2.qsp");
    CHECK(gen::parseStage("A-W.2.qsp", 3, s) && s.id() == "A>W.2");
    CHECK(gen::parseStage("w", 3, s) && s.plateau() && s.id() == "W");
    CHECK(!gen::parseStage("A>S.1", 3, s));
    CHECK(!gen::parseStage("A>W.4", 3, s));
    CHECK(!gen::parseStage("X", 3, s));
    CHECK(gen::allStages(3).size() == 20);
    gen::Item conifer;
    conifer.looks = "W";
    gen::parseStage("A>W.1", 3, s);
    gen::EffectiveStage e = gen::effectiveStage(conifer, s);
    CHECK(e.from == 'U' && e.to == 'W' && e.t == 0.25f);
    gen::parseStage("A", 3, s);
    CHECK(gen::effectiveStage(conifer, s).vanilla());
}

// ---------------------------------------------------------------------------------------------
// Test-only transform: byte-level changes whose LZ4 behaviour is controlled by the item class.
//   fit      : XOR every byte with a constant (LZ4 parse unchanged, always fits at bias 0)
//   needbias : 90% random blocks unless fitBias >= 6
//   mild     : 90% random blocks unless strength <= 0.6
//   nofit    : random blocks always (keeps vanilla)
class TestWork : public gen::TextureWork {
public:
    TestWork(std::vector<uint8_t> dds, DdsInfo info, std::string cls, float strength, uint32_t seed, char look)
        : dds_(std::move(dds)), info_(std::move(info)), cls_(std::move(cls)), s_(strength), seed_(seed), look_(look) {}
    size_t mipCount() const override { return info_.mipCount; }
    bool encodeMip(size_t mip, int bias, std::vector<uint8_t>& out, std::string*) override {
        size_t off = info_.headerSize;
        for (size_t i = 0; i < mip; ++i) off += info_.mipRawSize[i];
        out.assign(dds_.begin() + long(off), dds_.begin() + long(off + info_.mipRawSize[mip]));
        const uint8_t k = uint8_t(look_ * 3 + int(s_ * 100));
        for (uint8_t& b : out) b ^= k;
        double noise = 0;
        if (cls_ == "needbias") noise = bias >= 6 ? 0 : 0.9;
        else if (cls_ == "mild") noise = s_ > 0.6f ? 0.9 : 0;
        else if (cls_ == "nofit") noise = 1.0;
        const size_t bb = info_.format == "DXT1" ? 8 : 16;
        synth::Rng rng(seed_ * 31 + mip);
        for (size_t p = 0; p + bb <= out.size(); p += bb)
            if (rng.next() % 1000 < uint32_t(noise * 1000))
                for (size_t i = 0; i < bb; ++i) out[p + i] = uint8_t(rng.next());
        return true;
    }

private:
    std::vector<uint8_t> dds_;
    DdsInfo info_;
    std::string cls_;
    float s_;
    uint32_t seed_;
    char look_;
};

class TestTransform : public gen::TextureTransform {
public:
    std::string name() const override { return "test"; }
    bool affects(const gen::Item& it, const gen::EffectiveStage& st) const override { return !st.vanilla(); }
    std::unique_ptr<gen::TextureWork> begin(const gen::Item& it, const gen::EffectiveStage& st, float strength,
                                            const uint8_t* dds, size_t n, std::string* err) const override {
        DdsInfo info;
        if (!parseDdsHeader(dds, n, info, err)) return nullptr;
        return std::unique_ptr<gen::TextureWork>(
            new TestWork(std::vector<uint8_t>(dds, dds + n), info, it.cls, strength, it.seed, st.to));
    }
};

// ---------------------------------------------------------------------------------------------
static std::string g_tmp;
static synth::Archive g_arc;

static void testArchive() {
    gen::GameArchive ar;
    std::string err;
    CHECK(ar.open(g_arc.root, &err));
    CHECK(ar.groupNames().size() == 2);
    CHECK(ar.hasGroup("0001") && ar.hasGroup("dlc/dlc_01/0015"));
    uint64_t es = 0;
    uint32_t ts = 0;
    CHECK(ar.exeBuild(es, ts) && ts == synth::kExeTimestamp && es == 4096);
    const gen::Group* g = ar.group("0001", &err);
    CHECK(g != nullptr);
    if (!g) return;
    CHECK(g->pamt.storedSelfHash() == g->pamt.computedSelfHash());
    CHECK(g->keyMaterial == crypto::kDefaultKeyMaterial);
    for (const synth::Entry& se : g_arc.entries) {
        const gen::Group* gg = ar.group(se.group, &err);
        auto it = gg->byPath.find(se.path());
        CHECK(it != gg->byPath.end());
        if (it == gg->byPath.end()) continue;
        std::vector<uint8_t> stored, plain;
        gen::EntryLayout L;
        CHECK(ar.readStored(se.group, it->second, stored, &err) && stored == se.stored);
        bool dec = gen::decodeStored(it->second, gg->keyMaterial, stored.data(), stored.size(), plain, &L, &err);
        CHECK(dec);
        if (!dec) { std::printf("  decode %s: %s\n", se.path().c_str(), err.c_str()); continue; }
        CHECK(plain == se.plain);
        // Identity re-encode: exact stored size, strict round trip, re-encrypted when needed.
        std::vector<uint8_t> again;
        int failed = -2;
        bool enc = gen::encodeStoredLike(L, plain, again, &err, &failed);
        CHECK(enc && again.size() == stored.size());
        if (!enc) std::printf("  re-encode %s: %s\n", se.path().c_str(), err.c_str());
        if (L.compression == 1) CHECK(std::memcmp(again.data(), stored.data(), L.dds.headerSize) == 0 || L.crypto != 0);
    }
}

static void testVerifyInventory() {
    gen::GameArchive ar;
    ar.open(g_arc.root);
    gen::Inventory inv;
    std::vector<std::string> errs;
    CHECK(gen::parseInventory(g_arc.classesJson, inv, errs));
    CHECK(inv.items.size() == 11);
    gen::VerifyReport r = gen::verifyInventory(ar, inv.items, true);
    CHECK(r.clean() && r.ok == inv.items.size());
    if (!r.clean())
        for (auto& i : r.issues) std::printf("  %s %s: %s\n", i.kind.c_str(), i.path.c_str(), i.detail.c_str());
    // A stale inventory: wrong offset and a missing path must be reported.
    inv.items[0].offset += 16;
    inv.items[1].path = "texture/tree/gone.dds";
    r = gen::verifyInventory(ar, inv.items, true);
    CHECK(!r.clean() && r.indexMismatch == 1 && r.missing == 1);
}

static std::vector<uint8_t> readAll(const std::string& p) {
    std::vector<uint8_t> v;
    pack::readWholeFile(p, v);
    return v;
}

struct MemOriginal : pack::OriginalReader {
    const std::vector<uint8_t>* file = nullptr;
    int calls = 0;
    bool readOriginal(uint64_t off, uint8_t* dst, size_t n) override {
        ++calls;
        if (off + n > file->size()) return false;
        std::memcpy(dst, file->data() + off, n);
        return true;
    }
};

static void testPipelineAndServe() {
    gen::GameArchive ar;
    std::string err;
    CHECK(ar.open(g_arc.root, &err));
    gen::Inventory inv;
    std::vector<std::string> errs;
    gen::parseInventory(g_arc.classesJson, inv, errs);
    TestTransform tr;
    gen::StageSpec st;
    gen::parseStage("A", 3, st);

    // ---- fitter outcomes per class ----------------------------------------------------------
    gen::FitPolicy pol;
    auto find = [&](const std::string& name) -> const gen::Item& {
        for (const gen::Item& it : inv.items)
            if (it.path.find(name) != std::string::npos) return it;
        return inv.items[0];
    };
    gen::TextureResult r = gen::processTexture(ar, find("oak_a.dds"), st, tr, pol);
    CHECK(r.outcome == gen::Outcome::Fit && r.maxBias == 0 && r.strength == 1.0f);
    r = gen::processTexture(ar, find("meadow_b"), st, tr, pol);
    CHECK(r.outcome == gen::Outcome::FitBias && r.maxBias == 6);
    r = gen::processTexture(ar, find("flowers_c"), st, tr, pol);
    CHECK(r.outcome == gen::Outcome::FitMild && r.strength == 0.5f);
    r = gen::processTexture(ar, find("nofit_d"), st, tr, pol);
    CHECK(r.outcome == gen::Outcome::Vanilla && r.replacement.empty());
    r = gen::processTexture(ar, find("pine_e"), st, tr, pol);
    CHECK(r.outcome == gen::Outcome::Unaffected);
    for (const char* n : {"rawtail_f", "lz4whole_g", "secret_h", "plain_i", "palm_j", "oak_a_sss"}) {
        r = gen::processTexture(ar, find(n), st, tr, pol);
        CHECK(r.outcome == gen::Outcome::Fit);
        if (r.outcome != gen::Outcome::Fit) std::printf("  %s: %s %s\n", n, gen::toString(r.outcome), r.message.c_str());
    }
    gen::Item stale = find("oak_a.dds");
    stale.stored += 1;
    CHECK(gen::processTexture(ar, stale, st, tr, pol).outcome == gen::Outcome::IndexMismatch);

    // ---- full run, deterministic across thread counts ----------------------------------------
    gen::GenOptions o;
    o.stage = st;
    o.threads = 1;
    o.outPath = g_tmp + "/A1.qsp";
    gen::GenSummary s1, s4;
    CHECK(gen::runGeneration(ar, inv.items, tr, o, s1, &err));
    o.threads = 4;
    o.outPath = g_tmp + "/A4.qsp";
    CHECK(gen::runGeneration(ar, inv.items, tr, o, s4, &err));
    CHECK(s1.fit == 7 && s1.fitBias == 1 && s1.fitMild == 1 && s1.vanilla == 1 && s1.unaffected == 1);
    CHECK(s1.records == 9 && s1.errors == 0 && s1.indexMismatch == 0);
    CHECK(s1.fitRate() > 0.89 && s1.fitRate() < 0.91);
    std::vector<uint8_t> p1 = readAll(g_tmp + "/A1.qsp"), p4 = readAll(g_tmp + "/A4.qsp");
    CHECK(!p1.empty() && p1 == p4);
    CHECK(!fs::exists(g_tmp + "/A1.qsp.tmp"));
    o.dryRun = true;
    o.only = "grass";
    gen::GenSummary sd;
    CHECK(gen::runGeneration(ar, inv.items, tr, o, sd, &err) && sd.selected == 3 && sd.affected == 3 && sd.records == 0);

    // ---- pack reader ---------------------------------------------------------------------------
    pack::PackReader pr;
    CHECK(pr.openFile(g_tmp + "/A1.qsp", &err));
    CHECK(pr.header().stageId == "A");
    CHECK(pr.header().gameBuild == pack::gameBuildId(4096, synth::kExeTimestamp));
    CHECK(pr.header().pamtHashes.size() == 2);
    CHECK(pr.records().size() == 9 && pr.paz().size() == 3);
    CHECK(pr.verifyAll(&err));

    // ---- serving: simulated game reads over every paz of the synthetic archive -----------------
    pack::PackServer srv;
    CHECK(srv.open(pack::openFileSource(g_tmp + "/A1.qsp"), 1 << 20, &err));
    std::mt19937_64 rng(777);
    size_t decodedOk = 0;
    for (size_t slot = 0; slot < pr.paz().size(); ++slot) {
        const pack::PazInfo& z = pr.paz()[slot];
        const std::vector<uint8_t> file = readAll(ar.pazPath(z.group, z.pazIndex));
        CHECK(file.size() == z.pazSize);
        CHECK(srv.slotFor(z.key()) == int(slot));
        std::vector<uint8_t> expected = file;
        for (uint32_t k = z.firstRecord; k < z.firstRecord + z.recordCount; ++k) {
            const pack::Record& rec = pr.records()[k];
            std::vector<uint8_t> pl;
            CHECK(pr.readPayload(rec, pl));
            std::memcpy(expected.data() + rec.offset, pl.data(), pl.size());
        }
        MemOriginal orig;
        orig.file = &file;
        // (1) random unaligned chunking with an original reader: exact result.
        for (int pass = 0; pass < 6; ++pass) {
            std::vector<uint8_t> got;
            uint64_t pos = 0;
            while (pos < file.size()) {
                size_t len = pass == 0 ? 1 + rng() % 64 : 1 + rng() % (pass * 9000);
                len = size_t(std::min<uint64_t>(len, file.size() - pos));
                std::vector<uint8_t> buf(file.begin() + long(pos), file.begin() + long(pos + len));
                srv.apply(int(slot), pos, buf.data(), buf.size(), &orig);
                got.insert(got.end(), buf.begin(), buf.end());
                pos += len;
            }
            CHECK(got == expected);
        }
        // (2) 4 KiB aligned chunks (any multiple) without an original reader: exact result.
        {
            std::vector<uint8_t> got;
            uint64_t pos = 0;
            while (pos < file.size()) {
                size_t len = size_t(std::min<uint64_t>(4096 * (1 + rng() % 5), file.size() - pos));
                std::vector<uint8_t> buf(file.begin() + long(pos), file.begin() + long(pos + len));
                pack::ApplyReport ar2 = srv.apply(int(slot), pos, buf.data(), buf.size(), nullptr);
                CHECK(ar2.unverifiable == 0 && ar2.mismatch == 0);
                got.insert(got.end(), buf.begin(), buf.end());
                pos += len;
            }
            CHECK(got == expected);
        }
        // (3) whole-entry reads without reader, idempotent second application, then decode.
        for (uint32_t k = z.firstRecord; k < z.firstRecord + z.recordCount; ++k) {
            const pack::Record& rec = pr.records()[k];
            std::vector<uint8_t> buf(file.begin() + long(rec.offset), file.begin() + long(rec.offset + rec.storedSize));
            pack::ApplyReport a1 = srv.apply(int(slot), rec.offset, buf.data(), buf.size(), nullptr);
            CHECK(a1.patched == 1 && a1.records == 1);
            pack::ApplyReport a2 = srv.apply(int(slot), rec.offset, buf.data(), buf.size(), nullptr);
            CHECK(a2.already == 1 && a2.patched == 0);
            CHECK(std::equal(buf.begin(), buf.end(), expected.begin() + long(rec.offset)));
            // Served bytes decode strictly; for "fit" items they equal the test transform output.
            const gen::Group* g = ar.group(z.group);
            gen::EntryLayout L;
            std::vector<uint8_t> plain;
            bool dec = gen::decodeStored(g->byPath.at(rec.path), g->keyMaterial, buf.data(), buf.size(), plain, &L, &err);
            CHECK(dec);
            const gen::Item& it = find(rec.path);
            if (dec && it.cls == "fit") {
                DdsInfo info;
                parseDdsHeader(plain.data(), plain.size(), info);
                const synth::Entry* se = nullptr;
                for (const auto& e : g_arc.entries) if (e.path() == rec.path) se = &e;
                gen::EffectiveStage eff = gen::effectiveStage(it, st);
                auto work = tr.begin(it, eff, 1.0f, se->plain.data(), se->plain.size(), nullptr);
                std::vector<uint8_t> want(se->plain.begin(), se->plain.begin() + info.headerSize), mip;
                for (size_t m = 0; m < work->mipCount(); ++m) { work->encodeMip(m, 0, mip, nullptr); want.insert(want.end(), mip.begin(), mip.end()); }
                CHECK(plain == want);
                if (plain == want) ++decodedOk;
            }
        }
        // (4) unaligned reads without reader: a record is never half patched within one read.
        for (int i = 0; i < 300; ++i) {
            uint64_t pos = rng() % file.size();
            size_t len = size_t(std::min<uint64_t>(1 + rng() % 20000, file.size() - pos));
            std::vector<uint8_t> buf(file.begin() + long(pos), file.begin() + long(pos + len));
            srv.apply(int(slot), pos, buf.data(), buf.size(), nullptr);
            for (uint32_t k = z.firstRecord; k < z.firstRecord + z.recordCount; ++k) {
                const pack::Record& rec = pr.records()[k];
                uint64_t lo = std::max<uint64_t>(pos, rec.offset), hi = std::min<uint64_t>(pos + len, rec.offset + rec.storedSize);
                if (lo >= hi) continue;
                bool allOrig = std::equal(buf.begin() + long(lo - pos), buf.begin() + long(hi - pos), file.begin() + long(lo));
                bool allRepl = std::equal(buf.begin() + long(lo - pos), buf.begin() + long(hi - pos), expected.begin() + long(lo));
                CHECK(allOrig || allRepl);
            }
        }
        // (5) unexpected data (another mod, a patch): untouched.
        if (z.recordCount) {
            const pack::Record& rec = pr.records()[z.firstRecord];
            std::vector<uint8_t> buf(file.begin() + long(rec.offset), file.begin() + long(rec.offset + rec.storedSize));
            buf[buf.size() / 2] ^= 0x40;
            std::vector<uint8_t> before = buf;
            pack::ApplyReport a = srv.apply(int(slot), rec.offset, buf.data(), buf.size(), &orig);
            CHECK(a.mismatch == 1 && buf == before);
        }
    }
    CHECK(decodedOk == 7);  // oak_a, oak_a_sss, rawtail_f, lz4whole_g, secret_h, plain_i, palm_j
    CHECK(srv.slotFor("0001/9.paz") == -1);
    CHECK(!srv.touches(0, 0, 777));  // junk before the first entry
    pack::ServeStats ss = srv.stats();
    CHECK(ss.patched > 0 && ss.partialChunks > 0 && ss.cacheHits > 0 && ss.cacheBytes <= (1u << 20));

    // ---- corruption detection --------------------------------------------------------------------
    {
        std::vector<uint8_t> bad = p1;
        bad[20] ^= 1;  // header
        pack::PackReader x;
        CHECK(!x.open(pack::memorySource(bad), &err));
        bad = p1;
        bad[bad.size() - 3] ^= 1;  // index
        CHECK(!x.open(pack::memorySource(bad), &err));
        bad = p1;
        bad.resize(bad.size() - 1);  // truncated
        CHECK(!x.open(pack::memorySource(bad), &err));
        bad = p1;
        bad[100] ^= 0xFF;  // payload of the first record
        pack::PackServer bs;
        CHECK(bs.open(pack::memorySource(bad), 1 << 20, &err));
        const pack::Record& rec = bs.reader().records()[0];
        const pack::PazInfo& z = bs.reader().paz()[rec.pazSlot];
        const std::vector<uint8_t> file = readAll(ar.pazPath(z.group, z.pazIndex));
        std::vector<uint8_t> buf(file.begin() + long(rec.offset), file.begin() + long(rec.offset + rec.storedSize));
        pack::ApplyReport a = bs.apply(int(rec.pazSlot), rec.offset, buf.data(), buf.size(), nullptr);
        CHECK(a.payloadErrors == 1 && std::equal(buf.begin(), buf.end(), file.begin() + long(rec.offset)));
        CHECK(bs.stats().disabledRecords == 1);
        CHECK(!bs.reader().verifyAll(&err));
    }
}

static void testWriterRejects() {
    pack::PackWriter w;
    pack::Header h;
    h.stageId = "W";
    std::string err;
    CHECK(w.open(g_tmp + "/bad.qsp", h, &err));
    std::vector<uint8_t> a(100, 1), b(100, 2);
    pack::Record r;
    r.group = "0001"; r.path = "x"; r.pazIndex = 0; r.offset = 0; r.storedSize = 100;
    CHECK(w.add(r, a.data(), b.data(), &err));
    r.path = "y"; r.offset = 50;  // overlaps x
    CHECK(w.add(r, a.data(), b.data(), &err));
    w.setPazSize("0001", 0, 1000);
    CHECK(!w.finish(&err));
    CHECK(!fs::exists(g_tmp + "/bad.qsp") && !fs::exists(g_tmp + "/bad.qsp.tmp"));
    pack::PackWriter w2;
    CHECK(w2.open(g_tmp + "/bad2.qsp", h, &err));
    r.offset = 950;  // outside the paz
    CHECK(w2.add(r, a.data(), b.data(), &err));
    w2.setPazSize("0001", 0, 1000);
    CHECK(!w2.finish(&err));
    CHECK(pack::chunkCount(4095, 2) == 2 && pack::chunkCount(4096, 4096) == 1 && pack::chunkCount(10, 0) == 0);
    uint64_t b0, e0;
    pack::chunkRange(4000, 5000, 1, b0, e0);
    CHECK(b0 == 4096 && e0 == 8192);
    pack::chunkRange(4000, 5000, 2, b0, e0);
    CHECK(b0 == 8192 && e0 == 9000);
}

int main(int argc, char** argv) {
    if (argc >= 3 && std::string(argv[1]) == "--write-synth") {
        synth::Archive a;
        std::string root = std::string(argv[2]) + "/game";
        if (!synth::build(root, a)) { std::printf("cannot write %s\n", root.c_str()); return 1; }
        pack::writeWholeFile(std::string(argv[2]) + "/classes.json", a.classesJson.data(), a.classesJson.size());
        std::printf("synthetic game folder in %s, inventory %s/classes.json\n", root.c_str(), argv[2]);
        return 0;
    }
    g_tmp = (fs::temp_directory_path() / ("qs_test_gen_" + std::to_string(std::random_device()()))).string();
    fs::create_directories(g_tmp);
    testChaCha();
    testJson();
    testStages();
    if (!synth::build(g_tmp + "/game", g_arc)) {
        std::printf("FAIL: cannot build the synthetic archive in %s\n", g_tmp.c_str());
        ++g_fail;
    } else {
        testArchive();
        testVerifyInventory();
        testPipelineAndServe();
        testWriterRejects();
    }
    std::error_code ec;
    fs::remove_all(g_tmp, ec);
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
