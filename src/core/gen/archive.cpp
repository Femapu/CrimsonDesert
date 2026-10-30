// QuattroStagioni - archive reader and stored-entry codec. See archive.h.
#include "archive.h"

#include <algorithm>
#include <cstring>
#include <filesystem>

#include "../crypto/chacha20.h"
#include "../lz4_exact.h"
#include "lz4.h"

namespace fs = std::filesystem;

namespace qs::gen {
namespace {

bool isGroupName(const std::string& s) {
    if (s.size() != 4) return false;
    for (char c : s)
        if (c < '0' || c > '9') return false;
    return true;
}

std::string u8(const fs::path& p) {
    auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

fs::path fromU8(const std::string& s) { return fs::u8path(s); }

std::string join(const std::string& a, const std::string& b) { return u8(fromU8(a) / fromU8(b)); }

bool fail(std::string* e, const std::string& m) {
    if (e) *e = m;
    return false;
}

}  // namespace

bool GameArchive::open(const std::string& gameDir, std::string* error) {
    root_ = gameDir;
    names_.clear();
    groups_.clear();
    paz_.clear();
    std::error_code ec;
    fs::path root = fromU8(gameDir);
    if (!fs::is_directory(root, ec)) return fail(error, "not a folder: " + gameDir);
    for (auto it = fs::directory_iterator(root, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        std::string n = u8(it->path().filename());
        if (isGroupName(n) && fs::exists(it->path() / "0.pamt", ec)) names_.push_back(n);
    }
    fs::path dlc = root / "dlc";
    if (fs::is_directory(dlc, ec)) {
        for (auto d = fs::directory_iterator(dlc, ec); !ec && d != fs::directory_iterator(); d.increment(ec)) {
            if (!d->is_directory(ec)) continue;
            std::string dn = u8(d->path().filename());
            std::error_code ec2;
            for (auto g = fs::directory_iterator(d->path(), ec2); !ec2 && g != fs::directory_iterator(); g.increment(ec2)) {
                std::string n = u8(g->path().filename());
                if (isGroupName(n) && fs::exists(g->path() / "0.pamt", ec2)) names_.push_back("dlc/" + dn + "/" + n);
            }
        }
    }
    std::sort(names_.begin(), names_.end());
    if (names_.empty()) return fail(error, "no NNNN/0.pamt group found under " + gameDir);
    return true;
}

bool GameArchive::hasGroup(const std::string& name) const {
    return std::binary_search(names_.begin(), names_.end(), name);
}

const Group* GameArchive::group(const std::string& name, std::string* error) {
    std::lock_guard<std::mutex> lock(m_);
    auto it = groups_.find(name);
    if (it != groups_.end()) return it->second.get();
    if (!hasGroup(name)) { fail(error, "group " + name + " not found"); return nullptr; }
    std::unique_ptr<Group> g(new Group());
    g->name = name;
    g->dir = join(root_, name);
    std::vector<uint8_t> data;
    if (!pack::readWholeFile(join(g->dir, "0.pamt"), data)) { fail(error, "cannot read " + name + "/0.pamt"); return nullptr; }
    if (!g->pamt.parse(data.data(), data.size())) {
        fail(error, name + "/0.pamt: " + g->pamt.error());
        return nullptr;
    }
    g->keyMaterial = crypto::keyMaterialFromTag(g->pamt.formatTag());
    for (PamtEntry& e : g->pamt.entries()) g->byPath.emplace(e.path, e);
    const Group* r = g.get();
    groups_[name] = std::move(g);
    return r;
}

std::string GameArchive::pazPath(const std::string& group, uint32_t pazIndex) const {
    return join(join(root_, group), std::to_string(pazIndex) + ".paz");
}

bool GameArchive::pazFileSize(const std::string& group, uint32_t pazIndex, uint64_t& size) {
    return pack::fileSize(pazPath(group, pazIndex), size);
}

bool GameArchive::readStored(const std::string& group, const PamtEntry& e, std::vector<uint8_t>& out, std::string* error) {
    const std::string key = group + "/" + std::to_string(e.pazIndex);
    pack::ByteSource* src = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_);
        auto it = paz_.find(key);
        if (it == paz_.end()) {
            std::string err;
            auto s = pack::openFileSource(pazPath(group, e.pazIndex), &err);
            if (!s) return fail(error, err);
            it = paz_.emplace(key, std::move(s)).first;
        }
        src = it->second.get();
    }
    out.resize(e.storedSize);
    if (!src->readAt(e.offset, out.data(), out.size()))
        return fail(error, "cannot read " + key + ".paz [" + std::to_string(e.offset) + ", +" + std::to_string(e.storedSize) + ")");
    return true;
}

bool GameArchive::exeBuild(uint64_t& size, uint32_t& ts) const {
    return readExeBuild(join(join(root_, "bin64"), "CrimsonDesert.exe"), size, ts);
}

bool readExeBuild(const std::string& exePath, uint64_t& size, uint32_t& ts) {
    auto src = pack::openFileSource(exePath);
    if (!src) return false;
    size = src->size();
    uint8_t h[4096] = {0};
    size_t n = size_t(std::min<uint64_t>(size, sizeof(h)));
    if (n < 0x40 || !src->readAt(0, h, n) || h[0] != 'M' || h[1] != 'Z') return false;
    uint32_t pe;
    std::memcpy(&pe, h + 0x3C, 4);
    if (uint64_t(pe) + 12 > n || std::memcmp(h + pe, "PE\0\0", 4) != 0) return false;
    std::memcpy(&ts, h + pe + 8, 4);
    return true;
}

// ------------------------------------------------------------------------------------------- codec
bool decodeStored(const PamtEntry& e, uint8_t keyMaterial, const uint8_t* stored, size_t storedSize,
                  std::vector<uint8_t>& plain, EntryLayout* layoutOut, std::string* error) {
    EntryLayout L;
    L.compression = e.compression();
    L.crypto = e.crypto();
    L.storedSize = e.storedSize;
    L.size = e.size;
    L.keyMaterial = keyMaterial;
    L.path = e.path;
    if (storedSize != e.storedSize) return fail(error, "stored size differs from the index");
    if (L.crypto != 0 && L.crypto != 3) return fail(error, "unsupported crypto " + std::to_string(L.crypto));
    std::vector<uint8_t> dec(stored, stored + storedSize);
    if (L.crypto == 3) crypto::cryptEntry(e.path, dec.data(), dec.size(), keyMaterial);
    std::string err;
    switch (L.compression) {
        case 0:
            if (e.size != e.storedSize) return fail(error, "uncompressed entry with size != stored size");
            plain = std::move(dec);
            break;
        case 1: {
            if (!validatePerMipDds(dec.data(), dec.size(), e.size, &err, &plain)) return fail(error, "per-mip DDS: " + err);
            if (!parseDdsHeader(dec.data(), dec.size(), L.dds, &err)) return fail(error, err);
            L.header.assign(dec.begin(), dec.begin() + L.dds.headerSize);
            uint64_t so = L.dds.headerSize, po = L.dds.headerSize;
            for (uint32_t i = 0; i < L.dds.mipCount; ++i) {
                uint32_t raw = L.dds.mipRawSize[i];
                uint32_t st = i < 4 ? L.dds.storedMip[i] : raw;
                L.mipStored.push_back(st);
                L.mipStoredOffset.push_back(so);
                L.mipPlainOffset.push_back(po);
                so += st;
                po += raw;
            }
            break;
        }
        case 2: {
            plain.assign(e.size, 0);
            int r = LZ4_decompress_safe(reinterpret_cast<const char*>(dec.data()), reinterpret_cast<char*>(plain.data()),
                                        int(dec.size()), int(plain.size()));
            if (r != int(e.size)) return fail(error, "LZ4 entry decode returned " + std::to_string(r));
            break;
        }
        default:
            return fail(error, "unsupported compression " + std::to_string(L.compression));
    }
    if (layoutOut) *layoutOut = std::move(L);
    return true;
}

MipEncode encodeMipStored(const EntryLayout& L, size_t mip, const uint8_t* raw, size_t rawSize) {
    MipEncode r;
    if (L.compression != 1 || mip >= L.mipStored.size() || rawSize != L.dds.mipRawSize[mip]) {
        r.error = "mip/layout mismatch";
        return r;
    }
    const size_t target = L.mipStored[mip];
    if (!L.mipIsLz4(mip)) {
        r.chunk.assign(raw, raw + rawSize);
        r.bestSize = rawSize;
        r.ok = true;
        return r;
    }
    Lz4ExactResult x = lz4CompressExact(raw, rawSize, target);
    r.bestSize = x.bestSize;
    if (!x.ok) {
        r.error = x.error;
        return r;
    }
    r.chunk = std::move(x.block);
    r.ok = true;
    return r;
}

static bool verifyAndEncrypt(const EntryLayout& L, std::vector<uint8_t>& blob, const std::vector<uint8_t>& expectedPlain,
                             std::string* error) {
    if (blob.size() != L.storedSize) return fail(error, "assembled size " + std::to_string(blob.size()) + " != stored size");
    if (L.crypto == 3) crypto::cryptEntry(L.path, blob.data(), blob.size(), L.keyMaterial);
    // Strict round trip through the same decoder used for originals.
    PamtEntry e;
    e.path = L.path;
    e.storedSize = L.storedSize;
    e.size = L.size;
    e.flags = uint8_t((L.crypto << 4) | L.compression);
    std::vector<uint8_t> check;
    std::string err;
    if (!decodeStored(e, L.keyMaterial, blob.data(), blob.size(), check, nullptr, &err)) return fail(error, "re-decode: " + err);
    if (check != expectedPlain) return fail(error, "re-decode differs from the intended texture");
    return true;
}

bool assembleStored(const EntryLayout& L, const std::vector<std::vector<uint8_t>>& chunks,
                    const std::vector<uint8_t>& expectedPlain, std::vector<uint8_t>& out, std::string* error) {
    if (L.compression != 1 || chunks.size() != L.mipStored.size()) return fail(error, "assemble: layout mismatch");
    out = L.header;
    for (size_t i = 0; i < chunks.size(); ++i) {
        if (chunks[i].size() != L.mipStored[i]) return fail(error, "assemble: mip " + std::to_string(i) + " chunk size");
        out.insert(out.end(), chunks[i].begin(), chunks[i].end());
    }
    return verifyAndEncrypt(L, out, expectedPlain, error);
}

bool encodeStoredLike(const EntryLayout& L, const std::vector<uint8_t>& plain, std::vector<uint8_t>& out,
                      std::string* error, int* failedMip) {
    if (failedMip) *failedMip = -1;
    if (plain.size() != L.size) return fail(error, "plain size differs from the entry size");
    switch (L.compression) {
        case 0:
            out = plain;
            return verifyAndEncrypt(L, out, plain, error);
        case 1: {
            if (std::memcmp(plain.data(), L.header.data(), L.header.size()) != 0)
                return fail(error, "DDS header differs from the original (stored sizes / pathc template)");
            std::vector<std::vector<uint8_t>> chunks;
            for (size_t i = 0; i < L.mipStored.size(); ++i) {
                MipEncode m = encodeMipStored(L, i, plain.data() + L.mipPlainOffset[i], L.dds.mipRawSize[i]);
                if (!m.ok) {
                    if (failedMip) *failedMip = int(i);
                    return fail(error, "mip " + std::to_string(i) + ": " + m.error);
                }
                chunks.push_back(std::move(m.chunk));
            }
            return assembleStored(L, chunks, plain, out, error);
        }
        case 2: {
            Lz4ExactResult x = lz4CompressExact(plain.data(), plain.size(), L.storedSize);
            if (!x.ok) {
                if (failedMip) *failedMip = 0;
                return fail(error, x.error);
            }
            out = std::move(x.block);
            return verifyAndEncrypt(L, out, plain, error);
        }
    }
    return fail(error, "unsupported compression");
}

}  // namespace qs::gen
