// QuattroStagioni - pack serving. See serve.h.
#include "serve.h"

#include <algorithm>
#include <cstring>

#include "../lookup3.h"

namespace qs::pack {

bool PackServer::open(std::unique_ptr<ByteSource> src, size_t cacheBytes, std::string* error) {
    if (!rd_.open(std::move(src), error)) return false;
    cacheLimit_ = cacheBytes;
    const size_t n = rd_.records().size();
    disabled_.reset(new std::atomic<uint8_t>[n ? n : 1]);
    for (size_t i = 0; i < n; ++i) disabled_[i].store(0);
    return true;
}

void PackServer::overlapping(int slot, uint64_t off, uint64_t len, uint32_t& first, uint32_t& last) const {
    first = last = 0;
    if (slot < 0 || size_t(slot) >= rd_.paz().size() || len == 0) return;
    const PazInfo& z = rd_.paz()[size_t(slot)];
    const std::vector<Record>& recs = rd_.records();
    const uint32_t b = z.firstRecord, e = z.firstRecord + z.recordCount;
    // First record whose offset is > off, then step back once: that one may cover `off`.
    uint32_t lo = b, hi = e;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (recs[mid].offset <= off) lo = mid + 1;
        else hi = mid;
    }
    uint32_t i = lo;
    if (i > b && recs[i - 1].offset + recs[i - 1].storedSize > off) --i;
    first = i;
    const uint64_t end = off + len;
    while (i < e && recs[i].offset < end) ++i;
    last = i;
}

bool PackServer::touches(int slot, uint64_t off, uint64_t len) const {
    uint32_t a, b;
    overlapping(slot, off, len, a, b);
    return a < b;
}

PackServer::Payload PackServer::payload(uint32_t rec, std::string* error) {
    {
        std::lock_guard<std::mutex> lock(m_);
        auto it = cache_.find(rec);
        if (it != cache_.end()) {
            lru_.splice(lru_.begin(), lru_, it->second.it);
            cacheHits_++;
            return it->second.data;
        }
    }
    cacheMisses_++;
    std::shared_ptr<std::vector<uint8_t>> buf(new std::vector<uint8_t>());
    if (!rd_.readPayload(rd_.records()[rec], *buf, error)) return nullptr;
    Payload p = buf;
    {
        std::lock_guard<std::mutex> lock(m_);
        if (buf->size() <= cacheLimit_ && !cache_.count(rec)) {
            while (cacheUsed_ + buf->size() > cacheLimit_ && !lru_.empty()) {
                uint32_t victim = lru_.back();
                lru_.pop_back();
                auto v = cache_.find(victim);
                cacheUsed_ -= v->second.data->size();
                cache_.erase(v);
            }
            lru_.push_front(rec);
            cache_[rec] = Slot{p, lru_.begin()};
            cacheUsed_ += buf->size();
        }
    }
    return p;
}

ApplyReport PackServer::apply(int slot, uint64_t off, uint8_t* buf, size_t len, OriginalReader* original) {
    ApplyReport rep;
    applyCalls_++;
    uint32_t first, last;
    overlapping(slot, off, len, first, last);
    if (first >= last || !buf) return rep;
    for (uint32_t ri = first; ri < last; ++ri) applyRecord(ri, off, buf, len, original, rep);
    return rep;
}

RecordResult PackServer::applyRecord(uint32_t ri, uint64_t off, uint8_t* buf, size_t len, OriginalReader* original,
                                     ApplyReport& rep) {
    const std::vector<Record>& recs = rd_.records();
    if (!buf || ri >= recs.size() || len == 0) return RecordResult::None;
    const Record& r = recs[ri];
    const uint64_t lo = std::max<uint64_t>(off, r.offset), hi = std::min<uint64_t>(off + len, r.offset + r.storedSize);
    if (lo >= hi) return RecordResult::None;
    rep.records++;
    if (!rep.first) rep.first = &r;
    recordHits_++;
    if (disabled_[ri].load()) { rep.payloadErrors++; payloadErrors_++; return RecordResult::PayloadError; }
    Payload pl = payload(ri, nullptr);
    if (!pl) {
        if (disabled_[ri].exchange(1) == 0) disabledCount_++;
        rep.payloadErrors++;
        payloadErrors_++;
        return RecordResult::PayloadError;
    }
    enum : uint8_t { kOrig = 0, kRepl = 1, kBad = 2, kUnverifiable = 3 };
    const uint32_t j0 = chunkIndexOf(r.offset, lo), j1 = chunkIndexOf(r.offset, hi - 1);
    const uint32_t* hashes = rd_.chunkHashes(r);
    std::vector<uint8_t> state(j1 - j0 + 1, kOrig), disk;
    bool bad = false, unverifiable = false, anyOrig = false;
    uint32_t partial = 0;
    for (uint32_t j = j0; j <= j1 && !bad; ++j) {
        uint64_t cb, ce;
        chunkRange(r.offset, r.storedSize, j, cb, ce);
        const uint64_t ps = std::max(cb, lo), pe = std::min(ce, hi);
        const size_t n = size_t(pe - ps);
        const uint8_t* bp = buf + (ps - off);
        const uint8_t* pp = pl->data() + (ps - r.offset);
        uint8_t& st = state[j - j0];
        if (std::memcmp(bp, pp, n) == 0) {
            st = kRepl;
        } else if (ps == cb && pe == ce) {
            st = cdHash(bp, n) == hashes[j] ? kOrig : kBad;
        } else if (original) {
            disk.resize(size_t(ce - cb));
            if (original->readOriginal(cb, disk.data(), disk.size()) && cdHash(disk.data(), disk.size()) == hashes[j] &&
                std::memcmp(disk.data() + (ps - cb), bp, n) == 0) {
                st = kOrig;
                ++partial;
            } else {
                st = kBad;
            }
        } else {
            st = kUnverifiable;
            unverifiable = true;
        }
        if (st == kBad) bad = true;
        if (st == kOrig) anyOrig = true;
    }
    if (bad) { rep.mismatch++; mismatch_++; return RecordResult::Mismatch; }
    if (unverifiable) { rep.unverifiable++; unverifiable_++; return RecordResult::Unverifiable; }
    rep.partialChunks += partial;
    partialChunks_ += partial;
    if (!anyOrig) { rep.already++; already_++; return RecordResult::Already; }
    uint64_t bytes = 0;
    for (uint32_t j = j0; j <= j1; ++j) {
        if (state[j - j0] != kOrig) continue;
        uint64_t cb, ce;
        chunkRange(r.offset, r.storedSize, j, cb, ce);
        const uint64_t ps = std::max(cb, lo), pe = std::min(ce, hi);
        std::memcpy(buf + (ps - off), pl->data() + (ps - r.offset), size_t(pe - ps));
        bytes += pe - ps;
    }
    rep.patched++;
    rep.bytesPatched += bytes;
    patched_++;
    bytesPatched_ += bytes;
    return RecordResult::Patched;
}

void PackServer::setCacheLimit(size_t bytes) {
    std::lock_guard<std::mutex> lock(m_);
    cacheLimit_ = bytes;
    while (cacheUsed_ > cacheLimit_ && !lru_.empty()) {
        uint32_t victim = lru_.back();
        lru_.pop_back();
        auto v = cache_.find(victim);
        cacheUsed_ -= v->second.data->size();
        cache_.erase(v);
    }
}

size_t PackServer::cacheLimit() const {
    std::lock_guard<std::mutex> lock(m_);
    return cacheLimit_;
}

ServeStats PackServer::stats() const {
    ServeStats s;
    s.applyCalls = applyCalls_;
    s.recordHits = recordHits_;
    s.patched = patched_;
    s.already = already_;
    s.mismatch = mismatch_;
    s.unverifiable = unverifiable_;
    s.payloadErrors = payloadErrors_;
    s.partialChunks = partialChunks_;
    s.bytesPatched = bytesPatched_;
    s.cacheHits = cacheHits_;
    s.cacheMisses = cacheMisses_;
    s.disabledRecords = disabledCount_;
    {
        std::lock_guard<std::mutex> lock(m_);
        s.cacheBytes = cacheUsed_;
    }
    return s;
}

}  // namespace qs::pack
