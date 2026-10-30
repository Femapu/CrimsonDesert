// QuattroStagioni - Crimson Desert 2.04 pamt parser (read-only). See pamt.h for the format.
#include "pamt.h"

#include <cstring>

#include "lookup3.h"

namespace qs {
namespace {

struct Reader {
    const uint8_t* d;
    size_t n;
    size_t p = 0;
    bool ok = true;
    uint32_t u32() {
        if (p + 4 > n) { ok = false; return 0; }
        uint32_t v = uint32_t(d[p]) | (uint32_t(d[p + 1]) << 8) | (uint32_t(d[p + 2]) << 16) | (uint32_t(d[p + 3]) << 24);
        p += 4;
        return v;
    }
    const uint8_t* take(size_t len) {
        if (len > n || p + len > n) { ok = false; return nullptr; }
        const uint8_t* r = d + p;
        p += len;
        return r;
    }
};

inline uint32_t rd32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

}  // namespace

std::string normalizePath(const std::string& p) {
    std::string s;
    s.reserve(p.size());
    for (char c : p) {
        if (c == '\\') c = '/';
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
        s.push_back(c);
    }
    size_t start = 0;
    while (start < s.size() && s[start] == '/') ++start;
    return s.substr(start);
}

bool Pamt::buildTrie(const uint8_t* tab, size_t len, std::unordered_map<uint32_t, std::string>& out) {
    size_t p = 0;
    while (p < len) {
        size_t start = p;
        uint32_t back = 0;
        int shift = 0;
        for (;;) {
            if (p >= len || shift > 28) { error_ = "trie: bad varint"; return false; }
            uint8_t c = tab[p++];
            back |= uint32_t(c & 0x7F) << shift;
            shift += 7;
            if (!(c & 0x80)) break;
        }
        if (p >= len) { error_ = "trie: truncated length"; return false; }
        uint8_t l = tab[p++];
        if (p + l > len) { error_ = "trie: truncated chars"; return false; }
        std::string name;
        if (back) {
            if (back > start) { error_ = "trie: parent out of range"; return false; }
            auto it = out.find(uint32_t(start - back));
            if (it == out.end()) { error_ = "trie: parent not a node start"; return false; }
            name = it->second;
        }
        name.append(reinterpret_cast<const char*>(tab + p), l);
        p += l;
        out.emplace(uint32_t(start), std::move(name));
    }
    return true;
}

bool Pamt::parse(const uint8_t* data, size_t size) {
    *this = Pamt();
    if (size < 12) { error_ = "file too small"; return false; }
    Reader r{data, size};
    selfHash_ = r.u32();
    uint32_t pazCount = r.u32();
    formatTag_ = r.u32();
    computedHash_ = cdHash(data + 12, size - 12);
    if (pazCount > 4096) { error_ = "implausible paz count"; return false; }
    for (uint32_t i = 0; i < pazCount; ++i) {
        PamtPaz z;
        z.index = r.u32();
        z.hash = r.u32();
        z.size = r.u32();
        paz_.push_back(z);
    }
    uint32_t dirLen = r.u32();
    const uint8_t* dirTab = r.take(dirLen);
    uint32_t fileLen = r.u32();
    const uint8_t* fileTab = r.take(fileLen);
    uint32_t nFolders = r.u32();
    const uint8_t* fol = r.take(size_t(nFolders) * 16);
    uint32_t nFiles = r.u32();
    const uint8_t* fil = r.take(size_t(nFiles) * 20);
    uint32_t extLen = r.u32();
    const uint8_t* ext = r.take(extLen);
    if (!r.ok) { error_ = "truncated tables"; return false; }

    if (!buildTrie(dirTab, dirLen, dirNames_) || !buildTrie(fileTab, fileLen, fileNames_)) return false;

    folders_.resize(nFolders);
    for (uint32_t i = 0; i < nFolders; ++i) {
        const uint8_t* q = fol + size_t(i) * 16;
        folders_[i] = {rd32(q), rd32(q + 4), rd32(q + 8), rd32(q + 12)};
        if (uint64_t(folders_[i].first) + folders_[i].count > nFiles) { error_ = "folder range out of bounds"; return false; }
    }
    files_.resize(nFiles);
    for (uint32_t i = 0; i < nFiles; ++i) {
        const uint8_t* q = fil + size_t(i) * 20;
        FileRec& f = files_[i];
        f.nameNode = rd32(q);
        f.offset = rd32(q + 4);
        f.stored = rd32(q + 8);
        f.size = rd32(q + 12);
        f.pazIndex = uint16_t(q[16] | (q[17] << 8));
        f.flags = q[18];
        f.ext = q[19];
    }
    // Extension table: "\0.dds\0.xml\0" -> index 0 = "", 1 = ".dds", ...
    exts_.clear();
    std::string cur;
    for (uint32_t i = 0; i < extLen; ++i) {
        if (ext[i] == 0) { exts_.push_back(cur); cur.clear(); }
        else cur.push_back(char(ext[i]));
    }
    if (exts_.empty()) exts_.push_back("");
    return true;
}

PamtEntry Pamt::makeEntry(const std::string& dir, uint32_t rec) const {
    const FileRec& f = files_[rec];
    PamtEntry e;
    auto it = fileNames_.find(f.nameNode);
    std::string name = it != fileNames_.end() ? it->second : std::string("?");
    if (f.ext < exts_.size()) name += exts_[f.ext];
    e.path = normalizePath(dir.empty() ? name : dir + "/" + name);
    e.pazIndex = f.pazIndex;
    e.offset = f.offset;
    e.storedSize = f.stored;
    e.size = f.size;
    e.flags = f.flags;
    e.record = rec;
    return e;
}

bool Pamt::find(const std::string& path, PamtEntry& out) const {
    const std::string want = normalizePath(path);
    const size_t slash = want.rfind('/');
    const std::string wantDir = slash == std::string::npos ? std::string() : want.substr(0, slash);
    for (const Folder& fo : folders_) {
        std::string dir;
        if (fo.dirNode != 0xFFFFFFFFu) {
            auto it = dirNames_.find(fo.dirNode);
            if (it == dirNames_.end()) continue;
            dir = normalizePath(it->second);
        }
        if (dir != wantDir) continue;
        for (uint32_t i = fo.first; i < fo.first + fo.count; ++i) {
            PamtEntry e = makeEntry(dir, i);
            if (e.path == want) { out = e; return true; }
        }
    }
    return false;
}

std::vector<PamtEntry> Pamt::entries() const {
    std::vector<PamtEntry> v;
    v.reserve(files_.size());
    for (const Folder& fo : folders_) {
        std::string dir;
        if (fo.dirNode != 0xFFFFFFFFu) {
            auto it = dirNames_.find(fo.dirNode);
            if (it != dirNames_.end()) dir = it->second;
        }
        for (uint32_t i = fo.first; i < fo.first + fo.count; ++i) v.push_back(makeEntry(dir, i));
    }
    return v;
}

}  // namespace qs
