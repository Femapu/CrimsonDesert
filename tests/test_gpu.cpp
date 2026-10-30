// QuattroStagioni - tests for the portable part of the GPU live swap (src/core/gpu/).
// Usage: test_gpu [data_dir]   (data_dir unused; synthetic data only, temp files in the system temp dir)
//
// Covers layouts and D3D12 footprint math, mip prints (packed == footprint, sampling properties),
// prints of stored per-mip DDS blobs, the fingerprint index (slots, ambiguity), the resident tracker,
// the budget scheduler, and the stage library end to end with synthetic packs, including a simulated
// "VRAM" where planned swap jobs are executed and checked byte for byte.
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "../src/core/gpu/fingerprint.h"
#include "../src/core/gpu/library.h"
#include "../src/core/gpu/swap_model.h"
#include "../src/core/lookup3.h"
#include "gpu_synth.h"
#include "lz4.h"

namespace fs = std::filesystem;
using namespace qs;
using namespace qs::gpu;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond)                                                                         \
    do {                                                                                    \
        if (cond) ++g_pass;                                                                 \
        else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); }       \
    } while (0)

static std::vector<uint8_t> randomBytes(size_t n, uint32_t seed) {
    std::vector<uint8_t> v(n);
    uint32_t x = seed * 747796405u + 1;
    for (auto& b : v) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        b = uint8_t(x >> 9);
    }
    return v;
}

// ---------------------------------------------------------------------------------------------
static void testLayouts() {
    CHECK(familyFromDxgi(71) == Family::BC1 && familyFromDxgi(72) == Family::BC1 && familyFromDxgi(70) == Family::BC1);
    CHECK(familyFromDxgi(77) == Family::BC3 && familyFromDxgi(78) == Family::BC3);
    CHECK(familyFromDxgi(80) == Family::BC4 && familyFromDxgi(83) == Family::BC5 && familyFromDxgi(98) == Family::BC7);
    CHECK(familyFromDxgi(2) == Family::Unknown);
    CHECK(familyFromDds("DXT1") == Family::BC1 && familyFromDds("DXT5") == Family::BC3);
    CHECK(familyFromDds("ATI1") == Family::BC4 && familyFromDds("ATI2") == Family::BC5 && familyFromDds("DXGI_99") == Family::BC7);
    CHECK(familyFromDds("RGB32") == Family::RGBA8);

    MipLayout l = mipLayout(Family::BC1, 256, 128, 0);
    CHECK(l.width == 256 && l.height == 128 && l.rowBytes == 512 && l.rows == 32 && l.size() == 16384);
    l = mipLayout(Family::BC3, 256, 128, 7);  // 2x1
    CHECK(l.width == 2 && l.height == 1 && l.rowBytes == 16 && l.rows == 1);
    l = mipLayout(Family::BC3, 256, 128, 12);  // clamps at 1x1
    CHECK(l.width == 1 && l.height == 1 && l.size() == 16);
    l = mipLayout(Family::RGBA8, 10, 6, 1);
    CHECK(l.width == 5 && l.height == 3 && l.rowBytes == 20 && l.rows == 3);
    l = mipLayout(Family::BC1, 20, 20, 0);  // non multiple of 4: rounded up to whole blocks
    CHECK(l.rowBytes == 40 && l.rows == 5);

    std::vector<Footprint> fps;
    uint64_t used = layoutFootprints(Family::BC1, 256, 128, 0, 9, 100, fps);
    CHECK(fps.size() == 9);
    bool aligned = true, ordered = true;
    for (size_t i = 0; i < fps.size(); ++i) {
        aligned = aligned && fps[i].offset % kPlacementAlign == 0 && fps[i].rowPitch % kPitchAlign == 0 &&
                  fps[i].rowPitch >= fps[i].layout.rowBytes;
        if (i) ordered = ordered && fps[i].offset >= fps[i - 1].offset + fps[i - 1].bytes();
    }
    CHECK(aligned && ordered);
    CHECK(fps[0].offset == 512 && fps[0].rowPitch == 512);
    CHECK(fps[1].rowPitch == 256);  // 128x64 BC1: 256 bytes per row
    CHECK(fps[5].rowPitch == 256 && fps[5].layout.rowBytes == 16);  // 8x4: 2 blocks, padded to 256
    CHECK(used == fps.back().offset + fps.back().bytes() - 100);
    CHECK(fps[0].bytes() == 512u * 31 + 512);

    // pack -> footprint -> pack round trip
    MipLayout ml = mipLayout(Family::BC3, 52, 36, 0);  // 13 x 9 blocks, 208-byte rows
    std::vector<uint8_t> packed = randomBytes(ml.size(), 5), fp(256 * ml.rows, 0xCD), back(ml.size());
    packedToFootprint(packed.data(), ml, fp.data(), 256);
    footprintToPacked(fp.data(), 256, ml, back.data());
    CHECK(back == packed);
    CHECK(fp[208] == 0xCD);  // padding untouched
}

static void testPrints() {
    // packed and footprint prints agree for many shapes (narrow, wide, tiny, non-square)
    struct S { Family f; uint32_t w, h; } shapes[] = {
        {Family::BC1, 1024, 1024}, {Family::BC1, 4, 4}, {Family::BC1, 8, 512}, {Family::BC3, 512, 8},
        {Family::BC3, 64, 64}, {Family::BC4, 2, 2}, {Family::BC5, 128, 32}, {Family::RGBA8, 33, 17},
        {Family::BC7, 16, 2048}, {Family::BC1, 32, 32}};
    bool agree = true, nonzero = true;
    uint32_t seed = 1;
    for (const S& s : shapes) {
        for (uint32_t m = 0; m < 4; ++m) {
            MipLayout l = mipLayout(s.f, s.w, s.h, m);
            std::vector<uint8_t> packed = randomBytes(l.size(), seed++);
            uint32_t pitch = (l.rowBytes + 255) / 256 * 256 + 256;
            std::vector<uint8_t> fp(size_t(pitch) * l.rows, 0xEE);
            packedToFootprint(packed.data(), l, fp.data(), pitch);
            uint64_t a = mipPrintPacked(s.f, l, packed.data()), b = mipPrint(s.f, l, fp.data(), pitch);
            agree = agree && a == b;
            nonzero = nonzero && a != 0;
        }
    }
    CHECK(agree);
    CHECK(nonzero);

    // sensitivity: a byte inside the sample changes the print; a byte outside does not (documented);
    // same bytes with another shape or family give another print
    MipLayout l = mipLayout(Family::BC1, 512, 512, 0);  // 1024-byte rows, 128 rows
    std::vector<uint8_t> d = randomBytes(l.size(), 77);
    uint64_t p0 = mipPrintPacked(Family::BC1, l, d.data());
    std::vector<uint8_t> e = d;
    e[(l.rowBytes - kSegmentBytes) / 2] ^= 1;  // first segment: row 0, centred
    CHECK(mipPrintPacked(Family::BC1, l, e.data()) != p0);
    e = d;
    e[size_t(l.rows - 1) * l.rowBytes + (l.rowBytes - kSegmentBytes) / 2 + 10] ^= 1;  // last segment
    CHECK(mipPrintPacked(Family::BC1, l, e.data()) != p0);
    e = d;
    e[5 * l.rowBytes + 3] ^= 1;  // not sampled
    CHECK(mipPrintPacked(Family::BC1, l, e.data()) == p0);
    MipLayout l2 = mipLayout(Family::BC4, 512, 512, 0);  // same byte layout, other family
    CHECK(l2.size() == l.size() && mipPrintPacked(Family::BC4, l2, d.data()) != p0);
    MipLayout l3 = mipLayout(Family::BC1, 1024, 256, 0);  // same size, other dims
    CHECK(l3.size() == l.size() && mipPrintPacked(Family::BC1, l3, d.data()) != p0);
    // transparent top rows (zeros) do not make two textures collide: other rows are sampled
    std::vector<uint8_t> a = randomBytes(l.size(), 1), b = randomBytes(l.size(), 2);
    std::memset(a.data(), 0, l.rowBytes * 8);
    std::memset(b.data(), 0, l.rowBytes * 8);
    CHECK(mipPrintPacked(Family::BC1, l, a.data()) != mipPrintPacked(Family::BC1, l, b.data()));

    // computePrints on a stored DDS (raw mips) and the same with mip 0 LZ4 compressed
    std::vector<uint8_t> st = gpusynth::makeStoredDds("DXT5", 64, 32, 7, 9);
    TexturePrints tp;
    std::string err;
    CHECK(computePrintsStored(st.data(), st.size(), tp, &err));
    CHECK(tp.family == Family::BC3 && tp.dxgiFormat == 77 && tp.width == 64 && tp.height == 32 && tp.mipCount == 7);
    CHECK(tp.mips.size() == 7 && tp.mipOffset.size() == 7 && tp.headerSize == 128);
    CHECK(tp.mipOffset[0] == 0 && tp.mipOffset[1] == 2048 && tp.mipOffset[2] == 2048 + 512);
    {
        MipLayout m1 = mipLayout(Family::BC3, 64, 32, 1);
        CHECK(tp.mips[1] == mipPrintPacked(Family::BC3, m1, st.data() + 128 + tp.mipOffset[1]));
    }
    TexturePrints plain;
    CHECK(computePrints(st.data(), st.size(), plain) && plain.mips == tp.mips);
    // LZ4 mip 0: make a compressible mip 0 and store it compressed
    std::vector<uint8_t> comp = gpusynth::makeStoredDds("DXT1", 64, 64, 4, 3);
    std::fill(comp.begin() + 128, comp.begin() + 128 + 2048, uint8_t(0x11));
    TexturePrints rawPrints;
    CHECK(computePrints(comp.data(), comp.size(), rawPrints));
    std::vector<char> lz(LZ4_compressBound(2048));
    int n = LZ4_compress_default(reinterpret_cast<const char*>(comp.data() + 128), lz.data(), 2048, int(lz.size()));
    CHECK(n > 0 && n < 2048);
    std::vector<uint8_t> stored(comp.begin(), comp.begin() + 128);
    gpusynth::put32(stored, 0x20, uint32_t(n));
    stored.insert(stored.end(), lz.begin(), lz.begin() + n);
    stored.insert(stored.end(), comp.begin() + 128 + 2048, comp.end());
    TexturePrints lzPrints;
    std::vector<uint8_t> decoded;
    CHECK(computePrintsStored(stored.data(), stored.size(), lzPrints, &err, &decoded));
    CHECK(lzPrints.mips == rawPrints.mips);
    CHECK(decoded.size() == comp.size() && std::memcmp(decoded.data() + 128, comp.data() + 128, comp.size() - 128) == 0);
    // corrupt stored blob is rejected
    std::vector<uint8_t> bad = stored;
    bad.resize(bad.size() - 3);
    CHECK(!computePrintsStored(bad.data(), bad.size(), lzPrints, &err));
}

static TexturePrints shapeOf(Family f, uint32_t dxgi, uint32_t w, uint32_t h, uint32_t mips) {
    TexturePrints t;
    t.family = f;
    t.dxgiFormat = dxgi;
    t.width = w;
    t.height = h;
    t.mipCount = mips;
    return t;
}

static void testIndex() {
    FingerprintIndex ix;
    CHECK(ix.slotOf("") == 0 && ix.slotOf("U") == 0 && ix.slotOf("none") == 0 && ix.slotOf("A") == -1);
    int a = ix.addStage("A"), w = ix.addStage("A>W.2");
    CHECK(a == 1 && w == 2 && ix.addStage("A") == 1 && ix.slotOf("A>W.2") == 2 && ix.stageOf(2) == "A>W.2");
    CHECK(ix.stageOf(0) == "U");

    TexturePrints s1 = shapeOf(Family::BC1, 71, 64, 64, 3), s2 = shapeOf(Family::BC3, 77, 32, 32, 2);
    uint32_t t1 = ix.addTexture("0001/0.paz@0", s1), t2 = ix.addTexture("0001/0.paz@4096", s2);
    CHECK(t1 == 0 && t2 == 1 && ix.addTexture("0001/0.paz@0", s1) == t1);
    CHECK(ix.addTexture("0001/0.paz@0", s2) == kNoTexture);  // same key, other shape
    CHECK(ix.hasShape(Family::BC1, 64, 64) && ix.hasShape(Family::BC1, 16, 16) && !ix.hasShape(Family::BC1, 8, 8));
    CHECK(ix.hasShape(Family::BC3, 16, 16) && !ix.hasShape(Family::BC4, 64, 64));

    CHECK(ix.addPrints(t1, 0, {101, 102, 103}));
    CHECK(ix.addPrints(t1, a, {201, 202, 103}));  // stage A leaves mip 2 unchanged
    CHECK(ix.addPrints(t1, w, {301, 302, 303}));
    CHECK(!ix.addPrints(t1, 0, {1, 2}));  // wrong mip count
    CHECK(ix.addPrints(t2, 0, {401, 999}));
    CHECK(ix.addPrints(t2, a, {501, 999}));
    Match m;
    CHECK(ix.lookup(202, m) && m.texture == t1 && m.mip == 1 && m.slots == slotBit(a));
    CHECK(ix.lookup(103, m) && m.texture == t1 && m.mip == 2 && m.slots == (slotBit(0) | slotBit(a)));
    CHECK(ix.lookup(999, m) && m.texture == t2 && m.mip == 1 && m.slots == (slotBit(0) | slotBit(a)));
    CHECK(!ix.lookup(12345, m) && m.texture == kNoTexture);
    // a third texture sharing a print makes it ambiguous
    uint32_t t3 = ix.addTexture("0001/0.paz@8192", shapeOf(Family::BC1, 71, 8, 8, 1));
    CHECK(ix.addPrints(t3, 0, {303}));
    CHECK(!ix.lookup(303, m) && m.texture == kAmbiguous);
    CHECK(ix.ambiguousCount() == 1);
    TextureDesc td;
    CHECK(ix.texture(t1, td) && td.slots == (slotBit(0) | slotBit(a) | slotBit(w)) && td.mipCount == 3);
    CHECK(ix.textureCount() == 3);
}

static void testTracker() {
    ResidentTracker tr;
    ResourceShape sh{71, 64, 64, 3, 1};
    Match m;
    m.texture = 7;
    m.mip = 0;
    m.slots = slotBit(0);
    CHECK(tr.onUpload(0xA0, sh, 0, m, 10) == ResidentTracker::Upload::New);
    m.mip = 1;
    CHECK(tr.onUpload(0xA0, sh, 1, m, 11) == ResidentTracker::Upload::Updated);
    Resident r;
    CHECK(tr.get(0xA0, r) && r.texture == 7 && r.mipOffset == 0 && r.content.size() == 3);
    CHECK(r.content[0] == slotBit(0) && r.content[1] == slotBit(0) && r.content[2] == 0 && r.lastUpload == 11);
    CHECK(!r.stateSeen && r.state == 0);

    // a lower quality resource: subresource 0 holds DDS mip 1
    ResourceShape low{71, 32, 32, 2, 1};
    m.mip = 1;
    CHECK(tr.onUpload(0xB0, low, 0, m, 12) == ResidentTracker::Upload::New);
    CHECK(tr.get(0xB0, r) && r.mipOffset == 1);
    // impossible match (DDS mip 0 into subresource 1) and array textures are ignored
    m.mip = 0;
    CHECK(tr.onUpload(0xC0, sh, 1, m, 12) == ResidentTracker::Upload::Ignored);
    ResourceShape arr{71, 64, 64, 3, 6};
    CHECK(tr.onUpload(0xC0, arr, 0, m, 12) == ResidentTracker::Upload::Ignored);
    Match amb;
    amb.texture = kAmbiguous;
    CHECK(tr.onUpload(0xC0, sh, 0, amb, 12) == ResidentTracker::Upload::Ignored);
    CHECK(!tr.tracked(0xC0) && tr.size() == 2);

    tr.onState(0xA0, 0xC0);  // PIXEL | NON_PIXEL shader resource
    CHECK(tr.get(0xA0, r) && r.stateSeen && r.state == 0xC0);
    tr.onState(0xDEAD, 0x40);  // untracked: ignored
    CHECK(!tr.tracked(0xDEAD));

    // plan: stage 2 for texture 7 -> jobs for the uploaded subresources only
    auto bytes = [](uint32_t, uint32_t mip) -> uint64_t { return uint64_t(2048) >> (2 * mip); };
    std::vector<SwapJob> jobs;
    tr.plan([](uint32_t) { return 2; }, bytes, jobs, 5);
    CHECK(jobs.size() == 3);  // A0 sub 0, A0 sub 1, B0 sub 0
    bool okJobs = jobs.size() == 3 && jobs[0].resource == 0xA0 && jobs[0].sub == 0 && jobs[0].ddsMip == 0 &&
                  jobs[1].sub == 1 && jobs[1].ddsMip == 1 && jobs[2].resource == 0xB0 && jobs[2].ddsMip == 1 &&
                  jobs[2].bytes == 512 && jobs[0].slot == 2 && jobs[0].notBefore == 5;
    CHECK(okJobs);
    tr.onSwapped(0xA0, 0, 2);
    tr.onSwapped(0xA0, 1, 2);
    jobs.clear();
    tr.plan([](uint32_t) { return 2; }, bytes, jobs);
    CHECK(jobs.size() == 1 && jobs[0].resource == 0xB0);
    jobs.clear();
    tr.plan([](uint32_t) { return -1; }, bytes, jobs);
    CHECK(jobs.empty());
    jobs.clear();
    tr.planResource(0xA0, [](uint32_t) { return 0; }, bytes, jobs);
    CHECK(jobs.size() == 2);

    // copies propagate tracking; CopyResource from an unknown resource drops the destination
    CHECK(tr.onCopy(0xD0, 0xA0, sh));
    CHECK(tr.get(0xD0, r) && r.texture == 7 && r.content[0] == slotBit(2) && !r.stateSeen);
    CHECK(tr.onCopy(0xE0, 0xA0, ResourceShape{71, 32, 32, 2, 1}, 0, 1));  // sub 1 of A0 -> sub 0 of E0
    CHECK(tr.get(0xE0, r) && r.mipOffset == 1 && r.content[0] == slotBit(2) && r.content[1] == 0);
    CHECK(!tr.onCopy(0xD0, 0x1234, sh));
    CHECK(!tr.tracked(0xD0));
    CHECK(!tr.onCopy(0xF0, 0xA0, ResourceShape{71, 128, 128, 3, 1}));  // CopyResource needs equal shapes

    // retarget: the same resource now holds another texture
    Match other;
    other.texture = 9;
    other.mip = 0;
    other.slots = slotBit(1);
    CHECK(tr.onUpload(0xA0, sh, 0, other, 20) == ResidentTracker::Upload::Retargeted);
    CHECK(tr.get(0xA0, r) && r.texture == 9 && r.content[0] == slotBit(1) && r.content[1] == 0 && r.stateSeen);
    CHECK(tr.onUnknownUpload(0xA0, 0) && !tr.tracked(0xA0));
    CHECK(!tr.onUnknownUpload(0xA0, 0));
    int calls = 0;
    CHECK(!tr.withTracked(0xA0, [&](const Resident&) { ++calls; }));
    CHECK(tr.withTracked(0xB0, [&](const Resident& rr) { calls += rr.mipOffset == 1 ? 10 : 1; }) && calls == 10);
    CHECK(tr.onDestroyed(0xB0) && !tr.onDestroyed(0xB0));
    CHECK(tr.size() == 1 && tr.resources().size() == 1 && tr.resources()[0] == 0xE0);
}

static void testScheduler() {
    SwapScheduler s;
    auto job = [](uint64_t res, uint32_t sub, uint64_t bytes, uint64_t nb = 0, int slot = 1) {
        SwapJob j;
        j.resource = res;
        j.sub = sub;
        j.bytes = bytes;
        j.notBefore = nb;
        j.slot = slot;
        return j;
    };
    s.push(job(1, 0, 1000));
    s.push(job(1, 1, 250));
    s.push(job(2, 0, 4000));
    s.push(job(3, 0, 100, 50));  // not before tick 50
    s.push(job(1, 1, 250, 0, 3));  // replaces the queued (1,1), keeps its position
    CHECK(s.size() == 4 && s.queuedBytes() == 5350);
    auto t = s.take(1500, 0);
    CHECK(t.size() == 2 && t[0].resource == 1 && t[0].sub == 0 && t[1].sub == 1 && t[1].slot == 3);
    t = s.take(1500, 0);  // 4000 > budget but first eligible: taken alone
    CHECK(t.size() == 1 && t[0].resource == 2);
    t = s.take(1500, 0);
    CHECK(t.empty() && s.size() == 1);  // job 3 waits
    t = s.take(1500, 50);
    CHECK(t.size() == 1 && t[0].resource == 3 && s.size() == 0 && s.queuedBytes() == 0);

    for (uint32_t i = 0; i < 10; ++i) s.push(job(10 + i % 2, i, 100));
    s.dropResource(10);
    CHECK(s.size() == 5 && s.queuedBytes() == 500);
    t = s.take(250, 0);
    CHECK(t.size() == 2 && t[0].resource == 11);
    s.clear();
    CHECK(s.size() == 0 && s.take(1000, 0).empty());

    // many jobs: replacement keeps O(1) behaviour and counts stay exact
    for (uint32_t i = 0; i < 20000; ++i) s.push(job(i % 1000, i % 13, 10));
    CHECK(s.size() == 13000);  // distinct (i % 1000, i % 13) pairs
    uint64_t total = 0;
    size_t n = 0;
    for (;;) {
        auto v = s.take(1 << 20, 0);
        if (v.empty()) break;
        for (auto& j : v) total += j.bytes;
        n += v.size();
    }
    CHECK(n * 10 == total && s.queuedBytes() == 0);
}

// ---------------------------------------------------------------------------------------------
// Stage library end to end with synthetic packs and a simulated VRAM.
class MemSource : public pack::ByteSource {
public:
    explicit MemSource(const std::vector<uint8_t>* d) : d_(d) {}
    bool readAt(uint64_t off, void* dst, size_t n) override {
        if (off + n > d_->size()) return false;
        std::memcpy(dst, d_->data() + off, n);
        return true;
    }
    uint64_t size() const override { return d_->size(); }

private:
    const std::vector<uint8_t>* d_;
};

static void testLibrary() {
    using namespace gpusynth;
    std::vector<SynthTexture> tex = {
        {"DXT1", 256, 128, 9, 11, "leaf_a.dds", 0, {}},
        {"DXT5", 128, 128, 8, 12, "leaf_b.dds", 0, {}},
        {"ATI1", 64, 64, 7, 13, "mask_c.dds", 0, {}},
    };
    std::vector<uint8_t> paz = buildPaz(tex);
    const fs::path dir = fs::temp_directory_path() / "qs_test_gpu";
    fs::create_directories(dir);
    std::string err;
    // stage A changes every texture (texture 0 keeps mips >= 6), stage W only textures 0 and 1
    std::vector<std::vector<uint8_t>> replA = {makeStageVariant(tex[0].original, "DXT1", 256, 128, 9, 101, 6),
                                               makeStageVariant(tex[1].original, "DXT5", 128, 128, 8, 102),
                                               makeStageVariant(tex[2].original, "ATI1", 64, 64, 7, 103)};
    std::vector<std::vector<uint8_t>> replW = {makeStageVariant(tex[0].original, "DXT1", 256, 128, 9, 201),
                                               makeStageVariant(tex[1].original, "DXT5", 128, 128, 8, 202), {}};
    const std::string pa = (dir / "A.qsp").string(), pw = (dir / "W.qsp").string();
    CHECK(writePack(pa, "A", "0001", 0, paz.size(), tex, replA, &err));
    CHECK(writePack(pw, "W", "0001", 0, paz.size(), tex, replW, &err));

    std::vector<uint8_t> pazCopy = paz;
    int opened = 0;
    StageLibrary lib([&](const std::string& group, uint32_t idx) -> std::unique_ptr<pack::ByteSource> {
        ++opened;
        if (group != "0001" || idx != 0) return nullptr;
        return std::make_unique<MemSource>(&pazCopy);
    });
    PackLoadStats st;
    int sa = lib.addPack(pack::openFileSource(pa), &err, &st);
    CHECK(sa == 1 && st.records == 3 && st.indexed == 3 && st.originalErrors == 0 && st.payloadErrors == 0);
    CHECK(!st.printsFromIndex);
    int sw = lib.addPack(pack::openFileSource(pw), &err, &st);
    CHECK(sw == 2 && st.records == 2 && st.indexed == 2);
    CHECK(lib.addPack(pack::openFileSource(pa), &err) == sa);  // same stage again
    CHECK(opened == 1);
    CHECK(lib.slotOf("W") == sw && lib.slotOf("U") == 0 && lib.stageOf(sa) == "A");
    CHECK(lib.index().textureCount() == 3);

    // every mip of every slot is identified: original, A, W
    auto texId = [&](size_t i) {
        Match m;
        TexturePrints tp;
        computePrintsStored(tex[i].original.data(), tex[i].original.size(), tp);
        lib.index().lookup(tp.mips[0], m);
        return m.texture;
    };
    uint32_t id0 = texId(0), id1 = texId(1), id2 = texId(2);
    CHECK(id0 != kNoTexture && id1 != kNoTexture && id2 != kNoTexture && id0 != id1 && id1 != id2);
    bool allFound = true, slotsOk = true;
    const std::vector<uint8_t>* data[3][3] = {{&tex[0].original, &replA[0], &replW[0]},
                                              {&tex[1].original, &replA[1], &replW[1]},
                                              {&tex[2].original, &replA[2], nullptr}};
    uint32_t ids[3] = {id0, id1, id2};
    for (int t = 0; t < 3; ++t)
        for (int s = 0; s < 3; ++s) {
            if (!data[t][s]) continue;
            TexturePrints tp;
            computePrintsStored(data[t][s]->data(), data[t][s]->size(), tp);
            for (uint32_t m = 0; m < tp.mipCount; ++m) {
                Match mt;
                if (!lib.index().lookup(tp.mips[m], mt) || mt.texture != ids[t] || mt.mip != m) {
                    // tiny mips of random data can only collide by chance (64-bit prints): treat as failure
                    allFound = false;
                    continue;
                }
                slotsOk = slotsOk && (mt.slots & slotBit(s));
            }
        }
    CHECK(allFound);
    CHECK(slotsOk);
    {  // texture 0, mip 7: stage A kept the original -> slots {0, A}
        TexturePrints tp;
        computePrintsStored(tex[0].original.data(), tex[0].original.size(), tp);
        Match mt;
        CHECK(lib.index().lookup(tp.mips[7], mt) && mt.slots == (slotBit(0) | slotBit(sa)));
    }
    CHECK(lib.targetFor(id0, sw) == sw && lib.targetFor(id2, sw) == 0 && lib.targetFor(id2, sa) == sa);
    CHECK(lib.targetFor(id1, 0) == 0);
    CHECK(lib.mipBytes(id0, 0) == 16384 && lib.mipBytes(id1, 1) == 4096 && lib.mipBytes(id0, 99) == 0);

    // mip data equals the stored mip bytes of the right source
    StageLibrary::MipData md;
    CHECK(lib.mipData(id1, sw, 2, md, &err));
    CHECK(md.size == mipBytes("DXT5", 128, 128, 2) &&
          std::memcmp(md.dds->data() + md.offset, replW[1].data() + 128 + 16384 + 4096, md.size) == 0);
    CHECK(lib.mipData(id2, 0, 0, md, &err) && std::memcmp(md.dds->data() + md.offset, tex[2].original.data() + 128, md.size) == 0);
    CHECK(!lib.mipData(id2, sw, 0, md, &err));  // W has no data for texture 2
    CHECK(lib.cacheBytes() > 0);

    // simulated VRAM: resources upload original / stage A data, then a stage change to W is planned and
    // executed; every subresource must end up holding exactly the target data
    ResidentTracker tr;
    SwapScheduler sched;
    std::map<uint64_t, std::vector<std::vector<uint8_t>>> vram;  // resource -> subresources (packed)
    auto upload = [&](uint64_t res, size_t t, const std::vector<uint8_t>& stored, uint32_t firstMip) {
        TexturePrints tp;
        std::vector<uint8_t> dec;
        computePrintsStored(stored.data(), stored.size(), tp, nullptr, &dec);
        ResourceShape sh{tp.dxgiFormat, tp.width >> firstMip, tp.height >> firstMip, uint16_t(tp.mipCount - firstMip), 1};
        auto& subs = vram[res];
        subs.assign(sh.mipLevels, {});
        for (uint32_t s = 0; s < sh.mipLevels; ++s) {
            uint32_t m = s + firstMip;
            MipLayout l = mipLayout(tp.family, tp.width, tp.height, m);
            const uint8_t* p = dec.data() + 128 + tp.mipOffset[m];
            subs[s].assign(p, p + l.size());
            // what the hook does: print of the footprint, lookup, tracker
            Match mt;
            if (lib.index().hasShape(tp.family, l.width, l.height) && lib.index().lookup(mipPrintPacked(tp.family, l, p), mt))
                tr.onUpload(res, sh, s, mt, 1);
        }
        (void)t;
    };
    upload(0x100, 0, tex[0].original, 0);
    upload(0x200, 1, replA[1], 1);  // loaded while A was active, low quality (top mip skipped)
    upload(0x300, 2, replA[2], 0);
    CHECK(tr.size() == 3);
    auto runStage = [&](int active) {
        std::vector<SwapJob> jobs;
        tr.plan([&](uint32_t t) { return lib.targetFor(t, active); },
                [&](uint32_t t, uint32_t m) { return lib.mipBytes(t, m); }, jobs);
        for (auto& j : jobs) sched.push(j);
        size_t ticks = 0;
        for (;;) {
            auto batch = sched.take(8192, ticks++);
            if (batch.empty()) break;
            for (auto& j : batch) {
                StageLibrary::MipData d;
                if (!lib.mipData(j.texture, j.slot, j.ddsMip, d)) continue;
                vram[j.resource][j.sub].assign(d.dds->data() + d.offset, d.dds->data() + d.offset + d.size);
                tr.onSwapped(j.resource, j.sub, j.slot);
            }
        }
        return jobs.size();
    };
    auto vramIs = [&](uint64_t res, const std::vector<uint8_t>& stored, uint32_t firstMip) {
        std::vector<uint8_t> dec;
        if (!decodeStored(stored.data(), stored.size(), dec)) return false;
        TexturePrints tp;
        computePrints(dec.data(), dec.size(), tp);
        auto& subs = vram[res];
        for (uint32_t s = 0; s < subs.size(); ++s) {
            const uint8_t* p = dec.data() + 128 + tp.mipOffset[s + firstMip];
            if (subs[s].size() != mipLayout(tp.family, tp.width, tp.height, s + firstMip).size() ||
                std::memcmp(subs[s].data(), p, subs[s].size()) != 0)
                return false;
        }
        return true;
    };
    size_t n = runStage(sw);
    CHECK(n == 9 + 7 + 7);  // tex0: all 9 mips; tex1: 7 subresources; tex2: back to original, 7 mips
    CHECK(vramIs(0x100, replW[0], 0));
    CHECK(vramIs(0x200, replW[1], 1));
    CHECK(vramIs(0x300, tex[2].original, 0));
    CHECK(runStage(sw) == 0);  // idempotent
    n = runStage(sa);
    CHECK(n == 9 + 7 + 7);  // content is tracked as W only after our swaps, so every subresource moves
    CHECK(vramIs(0x100, replA[0], 0) && vramIs(0x200, replA[1], 1) && vramIs(0x300, replA[2], 0));
    runStage(0);
    CHECK(vramIs(0x100, tex[0].original, 0) && vramIs(0x200, tex[1].original, 1) && vramIs(0x300, tex[2].original, 0));

    // a changed game file (patch) is detected: the original is not served
    StageLibrary lib2([&](const std::string&, uint32_t) -> std::unique_ptr<pack::ByteSource> {
        return std::make_unique<MemSource>(&pazCopy);
    });
    pazCopy[tex[1].offset + 200] ^= 0xFF;
    CHECK(lib2.addPack(pack::openFileSource(pa), &err, &st) == 1);
    CHECK(st.originalErrors == 1 && st.indexed == 3);
    Match mt;
    TexturePrints tp1;
    computePrintsStored(replA[1].data(), replA[1].size(), tp1);
    CHECK(lib2.index().lookup(tp1.mips[0], mt));
    CHECK(!lib2.mipData(mt.texture, 0, 0, md, &err));
    CHECK(lib2.mipData(mt.texture, 1, 0, md, &err));
    pazCopy[tex[1].offset + 200] ^= 0xFF;

    // stored prints callback (future pack field): used instead of reading the data
    int calls = 0;
    StageLibrary lib3(nullptr, [&](const pack::PackReader&, size_t, TexturePrints& o, TexturePrints& r) {
        ++calls;
        (void)o;
        (void)r;
        return false;  // not present: falls back to data (original unreadable: no opener)
    });
    CHECK(lib3.addPack(pack::openFileSource(pa), &err, &st) == 1 && calls == 3 && st.originalErrors == 3);
    fs::remove_all(dir);
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    testLayouts();
    testPrints();
    testIndex();
    testTracker();
    testScheduler();
    testLibrary();
    std::printf("test_gpu: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
