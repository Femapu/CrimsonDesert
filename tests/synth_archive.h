// QuattroStagioni - synthetic Crimson Desert style archive for tests (no game data involved).
//
// Writes a fake game folder that follows the documented 2.04 formats (docs/recon, src/core/pamt.h):
//   <root>/bin64/CrimsonDesert.exe   minimal MZ/PE header with a known TimeDateStamp
//   <root>/0001/0.pamt, 0.paz, 1.paz base group with per-mip DDS (LZ4 mips 0..3, raw tail mips),
//                                    a whole-LZ4 DDS, an all-raw DDS, ChaCha20 encrypted entries
//   <root>/dlc/dlc_01/0015/0.pamt    a DLC group
// Entries are placed at unaligned offsets with junk in between, so reads of neighbours and of chunk
// edges are exercised. Also returns the matching inventory (classes) JSON text.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "../src/core/crypto/chacha20.h"
#include "../src/core/lookup3.h"
#include "../src/core/pack/byte_source.h"
#include "lz4.h"

namespace synth {

constexpr uint32_t kExeTimestamp = 0x6B2F3AF2u;
constexpr uint32_t kPamtTag = 0x610E0232u;

struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed * 6364136223846793005ull + 1442695040888963407ull) {}
    uint32_t next() {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return uint32_t(s >> 33);
    }
};

inline void put32(std::vector<uint8_t>& b, uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i))); }
inline void set32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i)); }

// Plain DDS (DXT1 or DXT5) with a compressible but non-trivial pattern.
inline std::vector<uint8_t> makePlainDds(const char* fourcc, uint32_t w, uint32_t h, uint32_t mips, uint32_t seed) {
    std::vector<uint8_t> d(128, 0);
    std::memcpy(d.data(), "DDS ", 4);
    set32(&d[4], 124);
    set32(&d[8], 0x000A1007);
    set32(&d[12], h);
    set32(&d[16], w);
    set32(&d[28], mips);
    set32(&d[76], 32);
    set32(&d[80], 4);  // DDPF_FOURCC
    std::memcpy(&d[84], fourcc, 4);
    set32(&d[108], 0x401008);
    set32(&d[124], 0x0480);  // engine format id ("last4")
    const bool dxt5 = std::strcmp(fourcc, "DXT5") == 0;
    const uint32_t bb = dxt5 ? 16 : 8;
    Rng rng(seed);
    for (uint32_t m = 0; m < mips; ++m) {
        uint32_t mw = std::max(1u, w >> m), mh = std::max(1u, h >> m);
        uint32_t bw = (mw + 3) / 4, bh = (mh + 3) / 4;
        for (uint32_t by = 0; by < bh; ++by)
            for (uint32_t bx = 0; bx < bw; ++bx) {
                uint8_t blk[16] = {0};
                uint8_t* c = blk + (dxt5 ? 8 : 0);
                if (dxt5) {
                    blk[0] = 255; blk[1] = uint8_t(128 + (bx & 7));
                    for (int i = 2; i < 8; ++i) blk[i] = uint8_t(((bx + by) & 1) ? 0x49 : 0x92);
                }
                uint16_t c0 = uint16_t(((8 + (bx * 3 + by) % 16) << 11) | ((20 + (by * 5) % 30) << 5) | (4 + bx % 8));
                uint16_t c1 = uint16_t(c0 > 0x0841 ? c0 - 0x0841 : 0);
                if (c0 <= c1) c1 = c0 > 0 ? uint16_t(c0 - 1) : 0;
                c[0] = uint8_t(c0); c[1] = uint8_t(c0 >> 8); c[2] = uint8_t(c1); c[3] = uint8_t(c1 >> 8);
                uint32_t idx = ((bx + by) % 3 == 0) ? 0x55AA55AAu : 0x1B1B1B1Bu;
                if (rng.next() % 9 == 0) idx ^= rng.next();  // a little entropy
                set32(c + 4, idx);
                d.insert(d.end(), blk, blk + bb);
            }
    }
    return d;
}

// Stored per-mip form: mips 0..3 LZ4 when smaller, header +0x20 = stored sizes.
inline std::vector<uint8_t> perMipStore(std::vector<uint8_t>& plain, uint32_t w, uint32_t h, uint32_t mips, uint32_t bb,
                                        bool allRaw) {
    std::vector<uint8_t> out(plain.begin(), plain.begin() + 128);
    size_t p = 128;
    uint32_t stored[4] = {0, 0, 0, 0};
    std::vector<uint8_t> body;
    for (uint32_t m = 0; m < mips; ++m) {
        uint32_t mw = std::max(1u, w >> m), mh = std::max(1u, h >> m);
        uint32_t raw = ((mw + 3) / 4) * ((mh + 3) / 4) * bb;
        std::vector<char> tmp(static_cast<size_t>(LZ4_compressBound(int(raw))));
        int n = (m < 4 && !allRaw) ? LZ4_compress_default(reinterpret_cast<const char*>(&plain[p]), tmp.data(), int(raw),
                                                          int(tmp.size()))
                                   : 0;
        if (n > 0 && uint32_t(n) < raw) {
            body.insert(body.end(), tmp.begin(), tmp.begin() + n);
            if (m < 4) stored[m] = uint32_t(n);
        } else {
            body.insert(body.end(), plain.begin() + long(p), plain.begin() + long(p + raw));
            if (m < 4) stored[m] = raw;
        }
        p += raw;
    }
    for (int i = 0; i < 4; ++i) set32(&out[0x20 + 4 * i], stored[i]);
    for (int i = 0; i < 4; ++i) set32(&plain[0x20 + 4 * i], stored[i]);  // plain keeps the same header
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

struct Entry {
    std::string dir, name, ext;  // "texture/tree", "a", ".dds"
    std::string group;           // "0001"
    uint32_t paz = 0;
    uint8_t flags = 0;
    std::vector<uint8_t> plain, stored;
    uint64_t offset = 0;
    // inventory fields
    std::string fmt, cls, profile, looks;
    uint32_t w = 0, h = 0, mips = 0;
    std::string path() const { return dir + "/" + name + ext; }
};

struct Archive {
    std::string root;
    std::vector<Entry> entries;
    std::string classesJson;
};

inline Entry dds(const std::string& dir, const std::string& name, const char* fourcc, uint32_t w, uint32_t h,
                 uint32_t mips, uint8_t flags, const std::string& cls, const std::string& looks, uint32_t seed,
                 bool allRaw = false) {
    Entry e;
    e.dir = dir; e.name = name; e.ext = ".dds"; e.group = "0001"; e.flags = flags;
    e.fmt = fourcc; e.cls = cls; e.profile = "deciduous"; e.looks = looks; e.w = w; e.h = h; e.mips = mips;
    e.plain = makePlainDds(fourcc, w, h, mips, seed);
    const uint32_t bb = std::strcmp(fourcc, "DXT5") == 0 ? 16 : 8;
    const unsigned comp = flags & 15;
    if (comp == 1) {
        e.stored = perMipStore(e.plain, w, h, mips, bb, allRaw);
    } else if (comp == 2) {
        std::vector<char> tmp(static_cast<size_t>(LZ4_compressBound(int(e.plain.size()))));
        int n = LZ4_compress_default(reinterpret_cast<const char*>(e.plain.data()), tmp.data(), int(e.plain.size()), int(tmp.size()));
        e.stored.assign(tmp.begin(), tmp.begin() + n);
    } else {
        e.stored = e.plain;
    }
    if ((flags >> 4) == 3) qs::crypto::cryptEntry(e.path(), e.stored.data(), e.stored.size());
    return e;
}

inline uint32_t lookupSeed(const std::string& s) { return qs::cdHash(s.data(), s.size()); }

inline std::vector<uint8_t> trieNode(const std::string& s) {
    std::vector<uint8_t> v{0, uint8_t(s.size())};
    v.insert(v.end(), s.begin(), s.end());
    return v;
}

// Writes one group (pamt + paz files) for the entries of `group`.
inline bool writeGroup(const std::string& root, const std::string& group, std::vector<Entry>& all, uint32_t pazCount) {
    namespace fs = std::filesystem;
    fs::create_directories(fs::path(root) / group);
    std::vector<std::vector<uint8_t>> paz(pazCount);
    Rng junk(lookupSeed(group));
    for (auto& p : paz) for (int i = 0; i < 777; ++i) p.push_back(uint8_t(junk.next()));  // unaligned start
    std::vector<Entry*> es;
    for (Entry& e : all) if (e.group == group) es.push_back(&e);
    for (Entry* e : es) {
        auto& p = paz[e->paz];
        e->offset = p.size();
        p.insert(p.end(), e->stored.begin(), e->stored.end());
        size_t pad = 100 + junk.next() % 5000;
        for (size_t i = 0; i < pad; ++i) p.push_back(uint8_t(junk.next()));
    }
    // Directory and file tries, folders grouped by directory.
    std::vector<std::string> dirs;
    for (Entry* e : es) if (std::find(dirs.begin(), dirs.end(), e->dir) == dirs.end()) dirs.push_back(e->dir);
    std::vector<uint8_t> dirTab, fileTab, folders, files;
    std::vector<uint32_t> dirNode;
    for (const auto& d : dirs) { dirNode.push_back(uint32_t(dirTab.size())); auto n = trieNode(d); dirTab.insert(dirTab.end(), n.begin(), n.end()); }
    uint32_t fileIdx = 0;
    for (size_t di = 0; di < dirs.size(); ++di) {
        uint32_t first = fileIdx, count = 0;
        for (Entry* e : es) {
            if (e->dir != dirs[di]) continue;
            uint32_t node = uint32_t(fileTab.size());
            auto n = trieNode(e->name);
            fileTab.insert(fileTab.end(), n.begin(), n.end());
            put32(files, node);
            put32(files, uint32_t(e->offset));
            put32(files, uint32_t(e->stored.size()));
            put32(files, uint32_t(e->plain.size()));
            files.push_back(uint8_t(e->paz)); files.push_back(0);
            files.push_back(e->flags);
            files.push_back(e->ext == ".dds" ? 1 : 2);
            ++fileIdx; ++count;
        }
        put32(folders, qs::cdHash(dirs[di].data(), dirs[di].size()));
        put32(folders, dirNode[di]);
        put32(folders, first);
        put32(folders, count);
    }
    std::vector<uint8_t> pamt;
    put32(pamt, 0);
    put32(pamt, pazCount);
    put32(pamt, kPamtTag);
    for (uint32_t i = 0; i < pazCount; ++i) {
        put32(pamt, i);
        put32(pamt, qs::cdHash(paz[i].data(), paz[i].size()));
        put32(pamt, uint32_t(paz[i].size()));
    }
    put32(pamt, uint32_t(dirTab.size())); pamt.insert(pamt.end(), dirTab.begin(), dirTab.end());
    put32(pamt, uint32_t(fileTab.size())); pamt.insert(pamt.end(), fileTab.begin(), fileTab.end());
    put32(pamt, uint32_t(dirs.size())); pamt.insert(pamt.end(), folders.begin(), folders.end());
    put32(pamt, fileIdx); pamt.insert(pamt.end(), files.begin(), files.end());
    const char ext[] = "\0.dds\0.xml\0";
    put32(pamt, sizeof(ext) - 1); pamt.insert(pamt.end(), ext, ext + sizeof(ext) - 1);
    set32(pamt.data(), qs::cdHash(pamt.data() + 12, pamt.size() - 12));
    bool ok = qs::pack::writeWholeFile((fs::path(root) / group / "0.pamt").string(), pamt.data(), pamt.size());
    for (uint32_t i = 0; i < pazCount; ++i)
        ok = ok && qs::pack::writeWholeFile((fs::path(root) / group / (std::to_string(i) + ".paz")).string(), paz[i].data(), paz[i].size());
    return ok;
}

inline std::string jsonEntry(const Entry& e) {
    std::string ms;
    for (int i = 0; i < 4; ++i) {
        uint32_t v = 0;
        std::memcpy(&v, e.plain.data() + 0x20 + 4 * i, 4);
        ms += (i ? "," : "") + std::to_string(v);
    }
    return "{\"path\":\"" + e.path() + "\",\"group\":\"" + e.group + "\",\"paz\":" + std::to_string(e.paz) +
           ",\"offset\":" + std::to_string(e.offset) + ",\"stored\":" + std::to_string(e.stored.size()) +
           ",\"size\":" + std::to_string(e.plain.size()) + ",\"fmt\":\"" + e.fmt + "\",\"w\":" + std::to_string(e.w) +
           ",\"h\":" + std::to_string(e.h) + ",\"mips\":" + std::to_string(e.mips) + ",\"mip_stored\":[" + ms +
           "],\"role\":\"albedo\",\"cls\":\"" + e.cls + "\",\"profile\":\"" + e.profile + "\",\"looks\":\"" + e.looks +
           "\",\"tier\":1,\"region\":\"shared_temperate\",\"asset\":\"" + e.name + "\"}";
}

// Builds the whole synthetic game folder under `root`.
inline bool build(const std::string& root, Archive& a) {
    namespace fs = std::filesystem;
    a = Archive();
    a.root = root;
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(fs::path(root) / "bin64");
    // Minimal PE header: "MZ", e_lfanew = 0x80, "PE\0\0", machine, sections, TimeDateStamp.
    std::vector<uint8_t> exe(4096, 0);
    exe[0] = 'M'; exe[1] = 'Z';
    set32(&exe[0x3C], 0x80);
    std::memcpy(&exe[0x80], "PE\0\0", 4);
    exe[0x84] = 0x64; exe[0x85] = 0x86;
    set32(&exe[0x88], kExeTimestamp);
    bool ok = qs::pack::writeWholeFile((fs::path(root) / "bin64" / "CrimsonDesert.exe").string(), exe.data(), exe.size());

    auto& E = a.entries;
    E.push_back(dds("texture/tree", "oak_a", "DXT1", 128, 128, 8, 0x01, "fit", "SLAW", 11));
    E.push_back(dds("texture/tree", "oak_a_sss", "DXT5", 64, 128, 7, 0x01, "fit", "SLAW", 12));
    E.push_back(dds("texture/grass", "meadow_b", "DXT1", 128, 64, 6, 0x01, "needbias", "SLAW", 13));
    E.push_back(dds("texture/grass", "flowers_c", "DXT5", 64, 64, 7, 0x01, "mild", "SLAW", 14));
    E.push_back(dds("texture/grass", "nofit_d", "DXT1", 64, 64, 7, 0x01, "nofit", "SLAW", 15));
    E.push_back(dds("texture/tree", "pine_e", "DXT1", 64, 64, 7, 0x01, "fit", "W", 16));           // conifer: W only
    E.push_back(dds("texture/tree", "rawtail_f", "DXT1", 32, 32, 6, 0x01, "fit", "SLAW", 17, true)); // all mips raw
    E.push_back(dds("texture/ground", "lz4whole_g", "DXT1", 64, 64, 7, 0x02, "fit", "SLAW", 18));   // compression 2
    E.push_back(dds("texture/ground", "secret_h", "DXT1", 64, 64, 7, 0x31, "fit", "SLAW", 19));     // per-mip + ChaCha
    E.back().paz = 1;
    E.push_back(dds("texture/ground", "plain_i", "DXT1", 64, 32, 5, 0x00, "fit", "SLAW", 20));      // uncompressed
    E.back().paz = 1;
    {  // encrypted LZ4 xml (not a texture; checks the generic decoder)
        Entry x;
        x.dir = "ui/xml"; x.name = "season_config"; x.ext = ".xml"; x.group = "0001"; x.paz = 1; x.flags = 0x32;
        std::string text;
        for (int i = 0; i < 200; ++i) text += "<item id=\"" + std::to_string(i % 17) + "\" value=\"autumn\"/>\n";
        x.plain.assign(text.begin(), text.end());
        std::vector<char> tmp(static_cast<size_t>(LZ4_compressBound(int(x.plain.size()))));
        int n = LZ4_compress_default(text.data(), tmp.data(), int(text.size()), int(tmp.size()));
        x.stored.assign(tmp.begin(), tmp.begin() + n);
        qs::crypto::cryptEntry(x.path(), x.stored.data(), x.stored.size());
        E.push_back(x);
    }
    E.push_back(dds("texture/dlc", "palm_j", "DXT1", 64, 64, 7, 0x01, "fit", "SLAW", 21));
    E.back().group = "dlc/dlc_01/0015";
    ok = ok && writeGroup(root, "0001", E, 2);
    ok = ok && writeGroup(root, "dlc/dlc_01/0015", E, 1);

    std::string j = "{\"version\":1,\"game\":\"synthetic\",\"summary\":{},\"entries\":[\n";
    bool firstE = true;
    for (const Entry& e : E) {
        if (e.ext != ".dds") continue;
        j += (firstE ? "" : ",\n") + jsonEntry(e);
        firstE = false;
    }
    j += "\n]}\n";
    a.classesJson = j;
    return ok;
}

}  // namespace synth
