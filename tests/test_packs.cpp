// QuattroStagioni - tests for per-texture stage serving from several packs (pack/multi.h, pack/plan.h),
// pack format v2 (region + asset seed per record, v1 still readable) and multi-stage generation.
// Native, no game files: synthetic archive from tests/synth_archive.h, packs made with the tint transform.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "../src/core/gen/archive.h"
#include "../src/core/gen/inventory.h"
#include "../src/core/gen/pipeline.h"
#include "../src/core/gen/stage.h"
#include "../src/core/gen/transform.h"
#include "../src/core/lookup3.h"
#include "../src/core/pack/multi.h"
#include "../src/core/pack/plan.h"
#include "../src/core/pack/qsp_io.h"
#include "../src/core/season/season.h"
#include "synth_archive.h"

namespace fs = std::filesystem;
using namespace qs;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond)                                                                         \
    do {                                                                                    \
        if (cond) ++g_pass;                                                                 \
        else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); }       \
    } while (0)

static std::string g_tmp;
static synth::Archive g_arc;

static std::vector<uint8_t> readAll(const std::string& p) {
    std::vector<uint8_t> v;
    pack::readWholeFile(p, v);
    return v;
}

struct MemOriginal : pack::OriginalReader {
    const std::vector<uint8_t>* file = nullptr;
    bool readOriginal(uint64_t off, uint8_t* dst, size_t n) override {
        if (off + n > file->size()) return false;
        std::memcpy(dst, file->data() + off, n);
        return true;
    }
};

// Inventory of the synthetic archive with some variety: two regions and distinct assets.
static gen::Inventory inventory() {
    gen::Inventory inv;
    std::vector<std::string> errs;
    gen::parseInventory(g_arc.classesJson, inv, errs);
    for (gen::Item& it : inv.items) {
        if (it.path.find("meadow_b") != std::string::npos || it.path.find("secret_h") != std::string::npos)
            it.region = "north_cold";
        if (it.path.find("plain_i") != std::string::npos) it.region = "by_tile";
    }
    return inv;
}

static std::shared_ptr<pack::LoadedPack> load(const std::string& file) {
    std::shared_ptr<pack::LoadedPack> lp(new pack::LoadedPack());
    lp->server.reset(new pack::PackServer());
    std::string err;
    if (!lp->server->open(pack::openFileSource(file), 1 << 20, &err)) {
        std::printf("  cannot open %s: %s\n", file.c_str(), err.c_str());
        return nullptr;
    }
    lp->stageId = lp->server->reader().header().stageId;
    lp->file = fs::path(file).filename().string();
    return lp;
}

// ---------------------------------------------------------------------------------------------
static void testNamesAndRegions() {
    CHECK(pack::stageFileName("A>W.2") == "A-W.2.qsp" && pack::stageFileName("W") == "W.qsp");
    CHECK(pack::stageIdFromFileName("A-W.2.qsp") == "A>W.2");
    CHECK(pack::stageIdFromFileName("w.QSP") == "W");
    CHECK(pack::stageIdFromFileName("notes.txt") == "" && pack::stageIdFromFileName("A-S.1.qsp") == "");
    season::SeasonConfig cfg;
    cfg.setDefaults();
    for (int i = 0; i < cfg.stageCount(); ++i) CHECK(pack::stageCalendarIndex(cfg, pack::stageIdAt(cfg, i)) == i);
    CHECK(pack::stageCalendarIndex(cfg, "A>W.4") == -1);
    gen::Item it;
    it.region = "by_tile";
    CHECK(gen::regionOf(it) == season::Region::Temperate);
    it.region = "world";
    CHECK(gen::regionOf(it) == season::Region::Temperate);
    it.region = "north_cold";
    CHECK(gen::regionOf(it) == season::Region::NorthCold);
    it.region = "swamp_jungle";
    CHECK(gen::regionOf(it) == season::Region::SwampJungle);
    it.region = "aquatic";
    CHECK(gen::regionOf(it) == season::Region::Aquatic);
}

// ---------------------------------------------------------------------------------------------
// Multi-stage generation (one archive pass) == single-stage runs, byte for byte; v2 metadata.
static void testMultiGeneration(const gen::Inventory& inv) {
    gen::GameArchive ar;
    std::string err;
    CHECK(ar.open(g_arc.root, &err));
    auto tr = gen::makeTintTransform();
    std::vector<gen::StageSpec> stages;
    for (const char* id : {"A", "A>W.1", "A>W.3", "W", "L"}) {
        gen::StageSpec s;
        CHECK(gen::parseStage(id, 3, s));
        stages.push_back(s);
    }
    std::vector<std::string> outs;
    for (const gen::StageSpec& s : stages) outs.push_back(g_tmp + "/packs/" + s.fileName());
    fs::create_directories(g_tmp + "/packs");
    fs::create_directories(g_tmp + "/single");
    gen::GenOptions o;
    o.threads = 3;
    std::vector<gen::GenSummary> sums;
    CHECK(gen::runGenerationMulti(ar, inv.items, *tr, stages, outs, o, sums, &err));
    if (!err.empty()) std::printf("  multi: %s\n", err.c_str());
    CHECK(sums.size() == stages.size());
    for (size_t i = 0; i < sums.size(); ++i) CHECK(sums[i].records > 0 && sums[i].errors == 0);
    // Same bytes as one run per stage.
    for (size_t i : {size_t(0), size_t(3)}) {
        gen::GenOptions so;
        so.threads = 1;
        so.stage = stages[i];
        so.outPath = g_tmp + "/single/" + stages[i].fileName();
        gen::GenSummary s;
        CHECK(gen::runGeneration(ar, inv.items, *tr, so, s, &err));
        CHECK(readAll(so.outPath) == readAll(outs[i]));
        CHECK(s.records == sums[i].records);
    }
    // v2 records carry region and asset seed from the inventory.
    pack::PackReader pr;
    CHECK(pr.openFile(outs[0], &err));
    CHECK(pr.header().formatVersion == 2 && pr.header().transitionSteps == 3);
    size_t checked = 0;
    for (const pack::Record& r : pr.records()) {
        for (const gen::Item& it : inv.items)
            if (it.path == r.path) {
                CHECK(r.hasMeta && r.assetSeed == it.seed && r.region == uint8_t(gen::regionOf(it)));
                CHECK(it.seed == cdHash(it.asset.data(), it.asset.size()));
                ++checked;
            }
    }
    CHECK(checked == pr.records().size());
}

// After a hotfix an entry may only move inside its paz: the live offset is used, with a note.
static void testMovedEntry(const gen::Inventory& inv) {
    gen::GameArchive ar;
    ar.open(g_arc.root);
    auto tr = gen::makeTintTransform();
    gen::StageSpec st;
    gen::parseStage("W", 3, st);
    gen::Item moved = inv.items[0];
    const uint64_t live = moved.offset;
    moved.offset += 4096;
    gen::TextureResult r = gen::processTexture(ar, moved, st, *tr, gen::FitPolicy());
    CHECK(r.outcome == gen::Outcome::Fit && r.entry.offset == live && r.message.find("live pamt") != std::string::npos);
    moved.stored += 1;  // a real change: still skipped
    CHECK(gen::processTexture(ar, moved, st, *tr, gen::FitPolicy()).outcome == gen::Outcome::IndexMismatch);
}

// ---------------------------------------------------------------------------------------------
// v1 packs (no region/seed) stay readable and servable.
static void testV1Pack() {
    pack::PackWriter w;
    w.setFormatVersion(1);
    pack::Header h;
    h.stageId = "W";
    std::string err;
    const std::string path = g_tmp + "/v1.qsp";
    CHECK(w.open(path, h, &err));
    std::vector<uint8_t> a(5000), b(5000);
    for (size_t i = 0; i < a.size(); ++i) { a[i] = uint8_t(i * 7); b[i] = uint8_t(i * 13 + 1); }
    pack::Record r;
    r.group = "0001"; r.path = "texture/x.dds"; r.pazIndex = 0; r.offset = 1000; r.storedSize = 5000;
    r.region = 3; r.assetSeed = 1234;  // ignored by the v1 writer
    CHECK(w.add(r, a.data(), b.data(), &err));
    w.setPazSize("0001", 0, 10000);
    CHECK(w.finish(&err));
    pack::PackReader pr;
    CHECK(pr.openFile(path, &err));
    CHECK(pr.header().formatVersion == 1 && pr.header().transitionSteps == 0);
    CHECK(pr.records().size() == 1 && !pr.records()[0].hasMeta && pr.records()[0].assetSeed == 0);
    auto lp = load(path);
    CHECK(lp != nullptr);
    if (!lp) return;
    auto idx = pack::TextureIndex::build({lp});
    CHECK(idx->textures().size() == 1);
    const std::string p = "texture/x.dds";
    CHECK(idx->textures()[0].region == uint8_t(season::Region::Temperate) && !idx->textures()[0].hasMeta &&
          idx->textures()[0].seed == cdHash(p.data(), p.size()));
    auto snap = pack::assignForced(idx, "W");
    std::vector<uint8_t> file(10000, 0);
    std::memcpy(file.data() + 1000, a.data(), a.size());
    std::vector<uint8_t> buf = file;
    MemOriginal orig;
    orig.file = &file;
    pack::ApplyReport rep = snap->apply(idx->findPaz("0001/0.paz"), 0, buf.data(), buf.size(), &orig);
    CHECK(rep.patched == 1 && std::equal(b.begin(), b.end(), buf.begin() + 1000));
}

// ---------------------------------------------------------------------------------------------
struct World {
    season::SeasonConfig cfg;
    std::vector<std::shared_ptr<pack::LoadedPack>> packs;
    std::shared_ptr<const pack::TextureIndex> idx;
    std::map<std::string, std::vector<uint8_t>> files;  // paz key -> original bytes
};

static World makeWorld(const std::vector<std::string>& stageIds) {
    World w;
    w.cfg.setDefaults();
    for (const std::string& id : stageIds) {
        auto lp = load(g_tmp + "/packs/" + pack::stageFileName(id));
        if (lp) w.packs.push_back(lp);
    }
    w.idx = pack::TextureIndex::build(w.packs);
    gen::GameArchive ar;
    ar.open(g_arc.root);
    for (const pack::TextureIndex::Paz& z : w.idx->paz()) {
        const size_t slash = z.key.rfind('/');
        const std::string group = z.key.substr(0, slash);
        const uint32_t n = uint32_t(std::stoul(z.key.substr(slash + 1)));
        w.files[z.key] = readAll(ar.pazPath(group, n));
    }
    return w;
}

// Pack a texture should be served from at `day`, computed independently of multi.cpp.
static int expectedPack(const World& w, size_t t, double day, bool& fallback) {
    const pack::TextureIndex::Tex& x = w.idx->textures()[t];
    const season::VisualStage st = season::stageAt(w.cfg, season::Region(x.region), day, x.seed);
    fallback = false;
    if (st.isVanilla()) return -1;
    int p = w.idx->packOfStage(st.id());
    if (p < 0) {
        std::vector<std::string> present;
        for (auto& lp : w.packs) present.push_back(lp->stageId);
        const std::string fb = pack::fallbackStage(w.cfg, st.id(), present);
        if (fb.empty()) return -1;
        p = w.idx->packOfStage(fb);
        fallback = true;
    }
    if (w.idx->record(t, size_t(p)) == pack::TextureIndex::kNoRecord) { fallback = false; return -1; }
    return p;
}

static void testPerTextureResolution() {
    // A>W.2 deliberately missing: its textures fall back to A>W.1.
    World w = makeWorld({"A", "A>W.1", "A>W.3", "W"});
    CHECK(w.packs.size() == 4);
    CHECK(w.idx->conflicts() == 0);
    CHECK(w.idx->textures().size() >= 9);
    std::set<uint8_t> regions;
    for (const auto& t : w.idx->textures()) { regions.insert(t.region); CHECK(t.hasMeta); }
    CHECK(regions.size() == 2);
    // pine_e (looks "W") is in the transitions and W, never in A.
    const int pA = w.idx->packOfStage("A"), pW = w.idx->packOfStage("W");
    CHECK(pA >= 0 && pW >= 0);
    size_t onlyInW = 0;
    for (size_t t = 0; t < w.idx->textures().size(); ++t)
        if (w.idx->record(t, size_t(pA)) == pack::TextureIndex::kNoRecord && w.idx->record(t, size_t(pW)) != pack::TextureIndex::kNoRecord)
            ++onlyInW;
    CHECK(onlyInW >= 1);

    const double aStart = w.cfg.phaseStartDay(season::Phase::Autumn), wStart = w.cfg.phaseStartDay(season::Phase::Winter);
    std::shared_ptr<const pack::ServeSnapshot> prev;
    size_t mismatches = 0, maxDistinct = 0, fallbackSeen = 0, incrementalUpdates = 0, steps = 0;
    bool sawMissing = false;
    for (double day = aStart - 1.0; day < wStart + 9.0; day += 0.037) {
        auto inc = pack::assignStages(w.idx, w.cfg, day, prev.get());
        auto full = pack::assignStages(w.idx, w.cfg, day);
        CHECK(inc->serve == full->serve && inc->fallback == full->fallback);
        if (prev) incrementalUpdates += inc->updatedTextures;
        ++steps;
        std::set<int> distinct;
        for (size_t t = 0; t < w.idx->textures().size(); ++t) {
            bool fb;
            const int e = expectedPack(w, t, day, fb);
            if (e != inc->serve[t] || uint8_t(fb) != inc->fallback[t]) ++mismatches;
            if (inc->serve[t] >= 0) distinct.insert(inc->serve[t]);
            if (inc->fallback[t]) ++fallbackSeen;
        }
        maxDistinct = std::max(maxDistinct, distinct.size());
        if (std::find(inc->missingStages.begin(), inc->missingStages.end(), "A>W.2") != inc->missingStages.end()) {
            sawMissing = true;
            CHECK(inc->missingTextures > 0);
        }
        CHECK(day < inc->validUntil);
        prev = inc;
    }
    CHECK(mismatches == 0);
    CHECK(maxDistinct >= 3);        // jitter: several stages served at the same moment
    CHECK(fallbackSeen > 0 && sawMissing);
    // Incremental updates touch far fewer textures than a full re-evaluation every step.
    CHECK(incrementalUpdates < (steps - 1) * w.idx->textures().size() / 2);

    // Late summer (north disabled): L and U>L have no pack and the walk back reaches vanilla summer.
    season::SeasonConfig noNorth = w.cfg;
    noNorth.regions[int(season::Region::NorthCold)].enabled = false;
    auto lsnap = pack::assignStages(w.idx, noNorth, w.cfg.phaseStartDay(season::Phase::LateSummer) + 1.0);
    CHECK(lsnap->servedTextures == 0 && lsnap->empty());
    // Region matters: north_cold textures follow another calendar than temperate ones.
    bool regionDiff = false;
    for (double day = aStart - 6; day < wStart + 9 && !regionDiff; day += 0.25)
        regionDiff = season::stageAt(w.cfg, season::Region::NorthCold, day, 77) != season::stageAt(w.cfg, season::Region::Temperate, day, 77);
    CHECK(regionDiff);

    // Served bytes over whole paz files at one day equal the chosen pack payloads.
    const double day = wStart - 0.8;
    auto snap = pack::assignStages(w.idx, w.cfg, day);
    for (const auto& kv : w.files) {
        const int slot = snap->slotFor(kv.first);
        CHECK(slot >= 0);
        std::vector<uint8_t> want = kv.second;
        const pack::TextureIndex::Paz& z = w.idx->paz()[size_t(slot)];
        for (uint32_t t = z.first; t < z.first + z.count; ++t) {
            if (snap->serve[t] < 0) continue;
            const pack::LoadedPack& lp = *w.idx->packs()[size_t(snap->serve[t])];
            std::vector<uint8_t> pl;
            CHECK(lp.server->reader().readPayload(lp.server->reader().records()[w.idx->record(t, size_t(snap->serve[t]))], pl));
            std::memcpy(want.data() + w.idx->textures()[t].offset, pl.data(), pl.size());
        }
        MemOriginal orig;
        orig.file = &kv.second;
        std::mt19937 rng(9);
        std::vector<uint8_t> got;
        for (uint64_t pos = 0; pos < kv.second.size();) {
            size_t len = size_t(std::min<uint64_t>(1 + rng() % 9000, kv.second.size() - pos));
            std::vector<uint8_t> buf(kv.second.begin() + long(pos), kv.second.begin() + long(pos + len));
            snap->apply(slot, pos, buf.data(), buf.size(), &orig);
            got.insert(got.end(), buf.begin(), buf.end());
            pos += len;
        }
        CHECK(got == want);
    }
    uint64_t patched = 0;
    for (auto& lp : w.packs) patched += lp->patched;
    CHECK(patched > 0);

    // Forced stage: every texture W knows is served by W.
    auto forced = pack::assignForced(w.idx, "W");
    size_t inW = 0;
    for (size_t t = 0; t < w.idx->textures().size(); ++t) inW += w.idx->record(t, size_t(pW)) != pack::TextureIndex::kNoRecord;
    CHECK(forced->forced && forced->servedTextures == inW && forced->packAssigned[size_t(pW)] == inW);
    CHECK(pack::assignForced(w.idx, "S")->servedTextures == 0);
    // Transition packs made with other transition steps are not used.
    season::SeasonConfig c2 = w.cfg;
    c2.transitionSteps = 2;
    auto s2 = pack::assignStages(w.idx, c2, wStart - 0.8);
    for (size_t t = 0; t < w.idx->textures().size(); ++t)
        if (s2->serve[t] >= 0) {
            const std::string& id = w.idx->packs()[size_t(s2->serve[t])]->stageId;
            CHECK(id == "A" || id == "W");
        }
}

// ---------------------------------------------------------------------------------------------
// Readers keep serving while the tick swaps snapshots: each record is either original or one of the
// payloads of the packs, never half patched, and nothing crashes or leaks.
static void testConcurrentSwap() {
    World w = makeWorld({"A", "A>W.1", "A>W.3", "W"});
    pack::SnapshotSlot slot;
    const double aStart = w.cfg.phaseStartDay(season::Phase::Autumn);
    slot.set(pack::assignStages(w.idx, w.cfg, aStart));
    std::map<std::string, std::vector<std::vector<uint8_t>>> payloads;  // per texture: valid contents
    std::vector<std::vector<std::vector<uint8_t>>> valid(w.idx->textures().size());
    for (size_t t = 0; t < w.idx->textures().size(); ++t) {
        const auto& x = w.idx->textures()[t];
        const auto& f = w.files.at(w.idx->paz()[x.paz].key);
        valid[t].emplace_back(f.begin() + long(x.offset), f.begin() + long(x.offset + x.storedSize));
        for (size_t p = 0; p < w.packs.size(); ++p) {
            const uint32_t r = w.idx->record(t, p);
            if (r == pack::TextureIndex::kNoRecord) continue;
            std::vector<uint8_t> pl;
            w.packs[p]->server->reader().readPayload(w.packs[p]->server->reader().records()[r], pl);
            valid[t].push_back(pl);
        }
    }
    std::atomic<bool> stop{false};
    std::atomic<size_t> bad{0}, reads{0};
    std::vector<std::thread> readers;
    for (int k = 0; k < 4; ++k)
        readers.emplace_back([&, k]() {
            std::mt19937 rng(100 + k);
            while (!stop.load()) {
                std::shared_ptr<const pack::ServeSnapshot> s = slot.get();
                const pack::TextureIndex::Paz& z = w.idx->paz()[rng() % w.idx->paz().size()];
                const std::vector<uint8_t>& file = w.files.at(z.key);
                const uint64_t pos = rng() % file.size();
                const size_t len = size_t(std::min<uint64_t>(1 + rng() % 30000, file.size() - pos));
                std::vector<uint8_t> buf(file.begin() + long(pos), file.begin() + long(pos + len));
                MemOriginal orig;
                orig.file = &file;
                const int sl = s->slotFor(z.key);
                s->apply(sl, pos, buf.data(), buf.size(), (rng() & 1) ? &orig : nullptr);
                uint32_t a, b;
                w.idx->overlapping(sl, pos, len, a, b);
                for (uint32_t t = a; t < b; ++t) {
                    const auto& x = w.idx->textures()[t];
                    const uint64_t lo = std::max<uint64_t>(pos, x.offset), hi = std::min<uint64_t>(pos + len, x.offset + x.storedSize);
                    bool ok = false;
                    for (const auto& v : valid[t])
                        ok = ok || std::equal(buf.begin() + long(lo - pos), buf.begin() + long(hi - pos), v.begin() + long(lo - x.offset));
                    if (!ok) bad++;
                }
                reads++;
            }
        });
    std::shared_ptr<const pack::ServeSnapshot> prev = slot.get();
    size_t swaps = 0;
    for (double day = aStart; day < aStart + 16; day += 0.01) {
        auto s = pack::assignStages(w.idx, w.cfg, day, prev.get());
        slot.set(s);
        prev = s;
        ++swaps;
        if (swaps % 200 == 0) slot.set(pack::assignForced(w.idx, swaps % 400 ? "W" : ""));
    }
    while (reads.load() < 2000) std::this_thread::yield();
    stop = true;
    for (auto& t : readers) t.join();
    CHECK(bad.load() == 0);
    CHECK(reads.load() >= 2000 && swaps > 1000);
    std::weak_ptr<const pack::ServeSnapshot> weak = prev;
    prev.reset();
    slot.set(nullptr);
    CHECK(weak.expired());  // the last snapshot is freed once nobody holds it
}

// ---------------------------------------------------------------------------------------------
static void testPlan(const gen::Inventory& inv) {
    season::SeasonConfig cfg;
    cfg.setDefaults();
    const uint32_t mask = pack::enabledRegionMask(cfg);
    CHECK((mask & (1u << int(season::Region::Temperate))) && !(mask & (1u << int(season::Region::DesertDry))));
    const double day = cfg.phaseStartDay(season::Phase::Autumn) + 1.3;
    const std::vector<std::string> present = {"A", "A>W.1", "W"};
    auto est = [](const std::string& id) -> uint64_t { return id.size() * 1000000ull; };
    pack::StagePlan plan = pack::planStages(cfg, day, 10, mask, present, est);
    CHECK(!plan.stages.empty());
    std::set<std::string> ids;
    for (const auto& s : plan.stages) {
        ids.insert(s.stageId);
        CHECK(s.stageId != "U");
        CHECK(s.present == (std::find(present.begin(), present.end(), s.stageId) != present.end()));
        CHECK(s.firstDay >= day - 1e-9 && s.lastDay <= day + 10 + 1e-9 && s.firstDay <= s.lastDay);
        CHECK(s.estimatedBytes == s.stageId.size() * 1000000ull);
    }
    CHECK(plan.presentCount + plan.missingCount == plan.stages.size());
    CHECK(plan.missingCount >= 1);
    // Superset of what any asset of any enabled region shows at any moment of the window.
    std::mt19937 rng(5);
    size_t outside = 0;
    for (int k = 0; k < 3000; ++k) {
        const uint32_t seed = rng();
        const int r = int(rng() % season::kRegionCount);
        const double x = day + 10.0 * double(rng() % 10001) / 10000.0;
        season::VisualStage s = season::stageAt(cfg, season::Region(r), x, seed);
        if (s.isVanilla()) continue;
        if (!ids.count(s.id())) { if (++outside < 5) std::printf("  outside: %s region %d day %.3f seed %u (window %.3f)\n", s.id().c_str(), r, x, seed, day); }
    }
    CHECK(outside == 0);
    // Packs to open: the present needed ones plus fallbacks of the missing ones.
    const std::vector<std::string> open = pack::packsToOpen(cfg, plan, present);
    for (const auto& id : open) CHECK(std::find(present.begin(), present.end(), id) != present.end());
    CHECK(pack::fallbackStage(cfg, "A>W.2", present) == "A>W.1");
    CHECK(pack::fallbackStage(cfg, "L", present) == "");
    CHECK(pack::fallbackStage(cfg, "S>U.1", present) == "W");
    // Text output lists every stage.
    const std::string text = plan.text(cfg);
    for (const auto& s : plan.stages) CHECK(text.find(s.stageId) != std::string::npos);
    CHECK(text.find("MISSING") != std::string::npos);
    // A zero-day window at a plateau far from boundaries needs one stage per region calendar.
    pack::StagePlan p0 = pack::planStages(cfg, cfg.phaseStartDay(season::Phase::Winter) + 3.0, 0, 1u << int(season::Region::Temperate), present);
    CHECK(p0.stages.size() >= 1 && p0.stages.size() <= 3);
    // Estimate helper from the inventory (generator side).
    const uint64_t a = gen::estimateStageBytes(inv.items, "A", cfg.transitionSteps);
    const uint64_t l = gen::estimateStageBytes(inv.items, "U", cfg.transitionSteps);
    CHECK(a > 0 && l == 0);
}

int main(int argc, char** argv) {
    g_tmp = (fs::temp_directory_path() / ("qs_test_packs_" + std::to_string(std::random_device()()))).string();
    fs::create_directories(g_tmp);
    testNamesAndRegions();
    if (!synth::build(g_tmp + "/game", g_arc)) {
        std::printf("FAIL: cannot build the synthetic archive in %s\n", g_tmp.c_str());
        ++g_fail;
    } else {
        const gen::Inventory inv = inventory();
        testMultiGeneration(inv);
        testV1Pack();
        testMovedEntry(inv);
        testPerTextureResolution();
        testConcurrentSwap();
        testPlan(inv);
    }
    std::error_code ec;
    fs::remove_all(g_tmp, ec);
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
