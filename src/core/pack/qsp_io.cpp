// QuattroStagioni - .qsp writer and reader. See qsp_io.h for the format.
#include "qsp_io.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>

#include "../lookup3.h"

namespace qs::pack {
namespace {

const uint8_t kMagic[8] = {'Q', 'S', 'P', 'A', 'C', 'K', 0x1A, 0x01};
const uint32_t kIndexMagic = 0x49505351u;  // "QSPI"

bool fail(std::string* e, const std::string& m) {
    if (e) *e = m;
    return false;
}

struct Out {
    std::vector<uint8_t> b;
    void u8(uint8_t v) { b.push_back(v); }
    void u16(uint16_t v) { u8(uint8_t(v)); u8(uint8_t(v >> 8)); }
    void u32(uint32_t v) { for (int i = 0; i < 4; ++i) u8(uint8_t(v >> (8 * i))); }
    void u64(uint64_t v) { for (int i = 0; i < 8; ++i) u8(uint8_t(v >> (8 * i))); }
    void str(const std::string& s) {
        u16(uint16_t(std::min<size_t>(s.size(), 0xFFFF)));
        b.insert(b.end(), s.begin(), s.begin() + long(std::min<size_t>(s.size(), 0xFFFF)));
    }
};

struct In {
    const uint8_t* d;
    size_t n, p = 0;
    bool ok = true;
    bool need(size_t k) {
        if (!ok || k > n - p) { ok = false; return false; }
        return true;
    }
    uint32_t u32() {
        if (!need(4)) return 0;
        uint32_t v = uint32_t(d[p]) | (uint32_t(d[p + 1]) << 8) | (uint32_t(d[p + 2]) << 16) | (uint32_t(d[p + 3]) << 24);
        p += 4;
        return v;
    }
    uint64_t u64() {
        uint64_t lo = u32();
        uint64_t hi = u32();
        return lo | (hi << 32);
    }
    std::string str() {
        if (!need(2)) return {};
        size_t l = size_t(d[p]) | (size_t(d[p + 1]) << 8);
        p += 2;
        if (!need(l)) return {};
        std::string s(reinterpret_cast<const char*>(d + p), l);
        p += l;
        return s;
    }
};

inline uint64_t rd64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}
inline uint32_t rd32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

void buildHeader(uint8_t h[kHeaderSize], uint32_t version, uint64_t indexOffset, uint64_t indexSize, uint64_t payloadSize, uint32_t nRec,
                 uint32_t indexHash) {
    Out o;
    o.b.insert(o.b.end(), kMagic, kMagic + 8);
    o.u32(version);
    o.u32(kHeaderSize);
    o.u64(indexOffset);
    o.u64(indexSize);
    o.u64(kHeaderSize);
    o.u64(payloadSize);
    o.u32(nRec);
    o.u32(indexHash);
    o.u32(0);
    o.u32(cdHash(o.b.data(), o.b.size()));
    std::memcpy(h, o.b.data(), kHeaderSize);
}

}  // namespace

std::string pazKey(const std::string& group, uint32_t pazIndex) { return group + "/" + std::to_string(pazIndex) + ".paz"; }
std::string PazInfo::key() const { return pazKey(group, pazIndex); }

std::string gameBuildId(uint64_t exeSize, uint32_t ts) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "exe %llu %08X", (unsigned long long)exeSize, ts);
    return buf;
}

uint32_t chunkCount(uint64_t offset, uint32_t size) {
    if (size == 0) return 0;
    return uint32_t(((offset + size - 1) >> kChunkShift) - (offset >> kChunkShift) + 1);
}

void chunkRange(uint64_t offset, uint32_t size, uint32_t j, uint64_t& begin, uint64_t& end) {
    const uint64_t base = (offset >> kChunkShift) + j;
    begin = std::max<uint64_t>(offset, base << kChunkShift);
    end = std::min<uint64_t>(offset + size, (base + 1) << kChunkShift);
}

uint32_t chunkIndexOf(uint64_t recordOffset, uint64_t fileOffset) {
    return uint32_t((fileOffset >> kChunkShift) - (recordOffset >> kChunkShift));
}

// ------------------------------------------------------------------------------------------ writer
PackWriter::~PackWriter() { abort(); }

bool PackWriter::open(const std::string& path, const Header& header, std::string* error) {
    abort();
    path_ = path;
    tmp_ = path + ".tmp";
    header_ = header;
    payloadSize_ = 0;
    recs_.clear();
    pazSizes_.clear();
    f_ = fopenUtf8(tmp_, "wb");
    if (!f_) return fail(error, "cannot create " + tmp_);
    uint8_t zero[kHeaderSize] = {0};
    if (std::fwrite(zero, 1, kHeaderSize, f_) != kHeaderSize) return fail(error, "write error on " + tmp_);
    return true;
}

void PackWriter::setPazSize(const std::string& group, uint32_t pazIndex, uint64_t size) {
    for (PazInfo& p : pazSizes_)
        if (p.group == group && p.pazIndex == pazIndex) { p.pazSize = size; return; }
    PazInfo p;
    p.group = group;
    p.pazIndex = pazIndex;
    p.pazSize = size;
    pazSizes_.push_back(p);
}

bool PackWriter::add(const Record& rIn, const uint8_t* original, const uint8_t* replacement, std::string* error) {
    if (!f_) return fail(error, "pack not open");
    if (rIn.storedSize == 0 || rIn.storedSize > kMaxRecordSize) return fail(error, "bad record size for " + rIn.path);
    Pending p;
    p.r = rIn;
    p.r.origHash = cdHash(original, rIn.storedSize);
    p.r.dataHash = cdHash(replacement, rIn.storedSize);
    p.r.dataOffset = kHeaderSize + payloadSize_;
    const uint32_t nc = chunkCount(rIn.offset, rIn.storedSize);
    p.chunks.resize(nc);
    for (uint32_t j = 0; j < nc; ++j) {
        uint64_t b, e;
        chunkRange(rIn.offset, rIn.storedSize, j, b, e);
        p.chunks[j] = cdHash(original + (b - rIn.offset), size_t(e - b));
    }
    if (std::fwrite(replacement, 1, rIn.storedSize, f_) != rIn.storedSize) return fail(error, "write error on " + tmp_);
    payloadSize_ += rIn.storedSize;
    recs_.push_back(std::move(p));
    return true;
}

bool PackWriter::finish(std::string* error) {
    if (!f_) return fail(error, "pack not open");
    // Order records by (paz key, offset) and build the paz table.
    std::vector<size_t> order(recs_.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::vector<std::string> keys(recs_.size());
    for (size_t i = 0; i < recs_.size(); ++i) keys[i] = pazKey(recs_[i].r.group, recs_[i].r.pazIndex);
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        if (keys[a] != keys[b]) return keys[a] < keys[b];
        return recs_[a].r.offset < recs_[b].r.offset;
    });
    std::vector<PazInfo> paz;
    std::vector<uint32_t> slotOf(recs_.size());
    for (size_t k = 0; k < order.size(); ++k) {
        const Pending& p = recs_[order[k]];
        if (paz.empty() || paz.back().key() != keys[order[k]]) {
            PazInfo z;
            z.group = p.r.group;
            z.pazIndex = p.r.pazIndex;
            z.firstRecord = uint32_t(k);
            bool found = false;
            for (const PazInfo& s : pazSizes_)
                if (s.group == z.group && s.pazIndex == z.pazIndex) { z.pazSize = s.pazSize; found = true; }
            if (!found) { abort(); return fail(error, "no paz size given for " + z.key()); }
            paz.push_back(z);
        } else {
            const Pending& prev = recs_[order[k - 1]];
            if (prev.r.offset + prev.r.storedSize > p.r.offset) {
                abort();
                return fail(error, "overlapping records " + prev.r.path + " and " + p.r.path);
            }
        }
        paz.back().recordCount++;
        slotOf[order[k]] = uint32_t(paz.size() - 1);
        if (p.r.offset + p.r.storedSize > paz.back().pazSize) {
            abort();
            return fail(error, p.r.path + " lies outside " + paz.back().key());
        }
    }

    Out o;
    o.u32(kIndexMagic);
    o.str(header_.stageId);
    o.str(header_.gameBuild);
    o.str(header_.generator);
    o.u32(header_.recipeVersion);
    o.u32(kChunkShift);
    if (version_ >= 2) o.u32(header_.transitionSteps);
    std::vector<std::pair<std::string, uint32_t>> groups = header_.pamtHashes;
    std::sort(groups.begin(), groups.end());
    groups.erase(std::unique(groups.begin(), groups.end()), groups.end());
    o.u32(uint32_t(groups.size()));
    for (const auto& g : groups) { o.str(g.first); o.u32(g.second); }
    o.u32(uint32_t(paz.size()));
    for (const PazInfo& z : paz) { o.str(z.group); o.u32(z.pazIndex); o.u64(z.pazSize); o.u32(z.firstRecord); o.u32(z.recordCount); }
    o.u32(uint32_t(order.size()));
    uint32_t chunkFirst = 0;
    for (size_t k : order) {
        const Record& r = recs_[k].r;
        o.u32(slotOf[k]);
        o.u64(r.offset);
        o.u32(r.storedSize);
        o.u32(r.size);
        o.u32(r.flags);
        o.u32(r.origHash);
        o.u32(r.dataHash);
        o.u64(r.dataOffset);
        o.u32(chunkFirst);
        if (version_ >= 2) {
            o.u32(r.assetSeed);
            o.u32(r.region);
        }
        o.str(r.path);
        chunkFirst += uint32_t(recs_[k].chunks.size());
    }
    o.u32(chunkFirst);
    for (size_t k : order)
        for (uint32_t h : recs_[k].chunks) o.u32(h);

    const uint64_t indexOffset = kHeaderSize + payloadSize_;
    bool ok = std::fwrite(o.b.data(), 1, o.b.size(), f_) == o.b.size();
    uint8_t hdr[kHeaderSize];
    buildHeader(hdr, version_, indexOffset, o.b.size(), payloadSize_, uint32_t(order.size()), cdHash(o.b.data(), o.b.size()));
    ok = ok && seek64(f_, 0) && std::fwrite(hdr, 1, kHeaderSize, f_) == kHeaderSize;
    ok = (std::fflush(f_) == 0) && ok;
    ok = (std::fclose(f_) == 0) && ok;
    f_ = nullptr;
    std::error_code ec;
    if (!ok) {
        std::filesystem::remove(std::filesystem::u8path(tmp_), ec);
        return fail(error, "write error on " + tmp_);
    }
    const auto dst = std::filesystem::u8path(path_), src = std::filesystem::u8path(tmp_);
    std::filesystem::remove(dst, ec);
    std::filesystem::rename(src, dst, ec);
    if (ec) return fail(error, "cannot rename " + tmp_ + ": " + ec.message());
    return true;
}

void PackWriter::abort() {
    if (f_) {
        std::fclose(f_);
        f_ = nullptr;
        std::error_code ec;
        std::filesystem::remove(std::filesystem::u8path(tmp_), ec);
    }
}

// ------------------------------------------------------------------------------------------ reader
bool PackReader::openFile(const std::string& path, std::string* error) {
    auto s = openFileSource(path, error);
    if (!s) return false;
    return open(std::move(s), error);
}

bool PackReader::open(std::unique_ptr<ByteSource> src, std::string* error) {
    *this = PackReader();
    if (!src) return fail(error, "no source");
    uint8_t h[kHeaderSize];
    const uint64_t fileSize = src->size();
    if (fileSize < kHeaderSize || !src->readAt(0, h, kHeaderSize)) return fail(error, "file too small for a pack header");
    if (std::memcmp(h, kMagic, 8) != 0) return fail(error, "not a QuattroStagioni pack (bad magic)");
    if (rd32(h + 60) != cdHash(h, 60)) return fail(error, "pack header hash mismatch (corrupt file)");
    const uint32_t version = rd32(h + 8);
    if (version < kMinFormatVersion || version > kFormatVersion)
        return fail(error, "unsupported pack format version " + std::to_string(version) +
                               (version > kFormatVersion ? " (made by a newer qs_gen: update the plugin)" : ""));
    if (rd32(h + 12) != kHeaderSize) return fail(error, "bad header size");
    const uint64_t indexOffset = rd64(h + 16), indexSize = rd64(h + 24);
    payloadOffset_ = rd64(h + 32);
    payloadSize_ = rd64(h + 40);
    const uint32_t nRec = rd32(h + 48), indexHash = rd32(h + 52);
    if (payloadOffset_ != kHeaderSize || payloadOffset_ + payloadSize_ != indexOffset || indexSize > (512u << 20) ||
        indexOffset + indexSize != fileSize)
        return fail(error, "pack layout does not match the file size (truncated or corrupt)");
    std::vector<uint8_t> idx(static_cast<size_t>(indexSize));
    if (!src->readAt(indexOffset, idx.data(), idx.size())) return fail(error, "cannot read pack index");
    if (cdHash(idx.data(), idx.size()) != indexHash) return fail(error, "pack index hash mismatch (corrupt file)");

    In in{idx.data(), idx.size()};
    if (in.u32() != kIndexMagic) return fail(error, "bad index magic");
    header_.stageId = in.str();
    header_.gameBuild = in.str();
    header_.generator = in.str();
    header_.recipeVersion = in.u32();
    if (in.u32() != kChunkShift) return fail(error, "unsupported chunk size");
    header_.formatVersion = version;
    if (version >= 2) header_.transitionSteps = in.u32();
    uint32_t n = in.u32();
    if (!in.ok || n > 100000) return fail(error, "bad group table");
    for (uint32_t i = 0; i < n && in.ok; ++i) {
        std::string g = in.str();
        uint32_t hsh = in.u32();
        header_.pamtHashes.emplace_back(g, hsh);
    }
    n = in.u32();
    if (!in.ok || n > 100000) return fail(error, "bad paz table");
    for (uint32_t i = 0; i < n && in.ok; ++i) {
        PazInfo z;
        z.group = in.str();
        z.pazIndex = in.u32();
        z.pazSize = in.u64();
        z.firstRecord = in.u32();
        z.recordCount = in.u32();
        paz_.push_back(z);
    }
    n = in.u32();
    if (!in.ok || n != nRec || n > 10000000) return fail(error, "bad record table");
    recs_.reserve(n);
    for (uint32_t i = 0; i < n && in.ok; ++i) {
        Record r;
        r.pazSlot = in.u32();
        r.offset = in.u64();
        r.storedSize = in.u32();
        r.size = in.u32();
        r.flags = uint8_t(in.u32());
        r.origHash = in.u32();
        r.dataHash = in.u32();
        r.dataOffset = in.u64();
        r.chunkFirst = in.u32();
        if (version >= 2) {
            r.assetSeed = in.u32();
            const uint32_t region = in.u32();
            if (region > 255) return fail(error, "bad record region");
            r.region = uint8_t(region);
            r.hasMeta = true;
        }
        r.path = in.str();
        recs_.push_back(std::move(r));
    }
    n = in.u32();
    if (!in.ok || !in.need(size_t(n) * 4)) return fail(error, "bad chunk table");
    chunks_.resize(n);
    for (uint32_t i = 0; i < n; ++i) chunks_[i] = in.u32();
    if (!in.ok || in.p != in.n) return fail(error, "index has trailing or missing bytes");

    // Structural validation: everything the server relies on.
    for (size_t s = 1; s < paz_.size(); ++s)
        if (!(paz_[s - 1].key() < paz_[s].key())) return fail(error, "paz table not sorted");
    uint32_t expectFirst = 0;
    for (size_t s = 0; s < paz_.size(); ++s) {
        const PazInfo& z = paz_[s];
        if (z.firstRecord != expectFirst || uint64_t(z.firstRecord) + z.recordCount > recs_.size())
            return fail(error, "paz record ranges inconsistent");
        expectFirst += z.recordCount;
        for (uint32_t k = z.firstRecord; k < z.firstRecord + z.recordCount; ++k) {
            Record& r = recs_[k];
            if (r.pazSlot != s) return fail(error, "record slot mismatch");
            r.group = z.group;
            r.pazIndex = z.pazIndex;
            if (r.storedSize == 0 || r.storedSize > kMaxRecordSize || r.offset + r.storedSize > z.pazSize)
                return fail(error, "record outside its paz: " + r.path);
            if (k > z.firstRecord && recs_[k - 1].offset + recs_[k - 1].storedSize > r.offset)
                return fail(error, "records overlap or are unsorted: " + r.path);
            if (r.dataOffset < payloadOffset_ || r.dataOffset + r.storedSize > payloadOffset_ + payloadSize_)
                return fail(error, "payload outside the payload area: " + r.path);
            if (uint64_t(r.chunkFirst) + chunkCount(r.offset, r.storedSize) > chunks_.size())
                return fail(error, "chunk hashes out of range: " + r.path);
        }
    }
    if (expectFirst != recs_.size()) return fail(error, "records not covered by the paz table");
    indexSize_ = size_t(indexSize);
    src_ = std::move(src);
    return true;
}

int PackReader::findPaz(const std::string& key) const {
    size_t lo = 0, hi = paz_.size();
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        const std::string k = paz_[mid].key();
        if (k == key) return int(mid);
        if (k < key) lo = mid + 1;
        else hi = mid;
    }
    return -1;
}

bool PackReader::readPayload(const Record& r, std::vector<uint8_t>& out, std::string* error) const {
    if (!src_) return fail(error, "pack not open");
    out.resize(r.storedSize);
    if (!src_->readAt(r.dataOffset, out.data(), out.size())) return fail(error, "cannot read payload of " + r.path);
    if (cdHash(out.data(), out.size()) != r.dataHash) return fail(error, "payload hash mismatch for " + r.path);
    return true;
}

bool PackReader::verifyAll(std::string* error) const {
    std::vector<uint8_t> buf;
    for (const Record& r : recs_)
        if (!readPayload(r, buf, error)) return false;
    return true;
}

}  // namespace qs::pack
