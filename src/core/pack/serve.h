// QuattroStagioni - serving game reads from a season pack (platform independent, unit tested).
//
// The plugin calls PackServer::apply() for every COMPLETED read of a .paz archive. For each pack
// record overlapping the read, the overlapping bytes are replaced with the record's payload, but only
// after the bytes the game got are proven to be the original ones:
//   * a 4 KiB chunk (absolute alignment, see qsp_io.h) fully inside the read is hashed and compared
//     with the stored original chunk hash; no original bytes are kept in memory;
//   * a chunk only partly inside the read (unaligned read edges) cannot be hashed from the buffer:
//     the server asks an OriginalReader (the plugin: a private handle on the same paz, unhooked
//     ReadFile) for that whole chunk, checks its hash and that the buffer piece equals it;
//     without a reader such a piece is "unverifiable" and the record is left untouched;
//   * a piece already equal to the payload counts as already patched (idempotent re-application).
// One record is all-or-nothing per read: if any piece is a mismatch, unverifiable or the payload
// cannot be loaded, none of that record's bytes in this read are changed (pass-through).
// Payloads are loaded whole on demand, checked against their dataHash, and kept in a bounded LRU
// cache; a payload that fails its hash disables that record for the rest of the session.
// Thread safe: any number of concurrent apply() calls.
#pragma once
#include <atomic>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "qsp_io.h"

namespace qs::pack {

class OriginalReader {
public:
    virtual ~OriginalReader() = default;
    // Reads original archive bytes [offset, offset + n) of the paz being served.
    virtual bool readOriginal(uint64_t offset, uint8_t* dst, size_t n) = 0;
};

struct ApplyReport {
    uint32_t records = 0;        // records overlapping the read
    uint32_t patched = 0;        // records whose bytes were replaced (fully or the original parts)
    uint32_t already = 0;        // records whose overlap already held the payload
    uint32_t mismatch = 0;       // left untouched: bytes are neither original nor payload
    uint32_t unverifiable = 0;   // left untouched: partial chunk and no OriginalReader (or it failed)
    uint32_t payloadErrors = 0;  // left untouched: payload unreadable or corrupt
    uint32_t partialChunks = 0;  // partial chunks verified through the OriginalReader
    uint64_t bytesPatched = 0;
    const Record* first = nullptr;  // first record touched (for logs)
};

// Outcome of one record in one read (applyRecord).
enum class RecordResult { None, Patched, Already, Mismatch, Unverifiable, PayloadError };

struct ServeStats {
    uint64_t applyCalls = 0, recordHits = 0, patched = 0, already = 0, mismatch = 0, unverifiable = 0,
             payloadErrors = 0, partialChunks = 0, bytesPatched = 0, cacheHits = 0, cacheMisses = 0,
             cacheBytes = 0, disabledRecords = 0;
};

class PackServer {
public:
    // Opens the pack (header + index only). cacheBytes bounds the payload cache.
    bool open(std::unique_ptr<ByteSource> src, size_t cacheBytes, std::string* error = nullptr);
    const PackReader& reader() const { return rd_; }

    int slotFor(const std::string& pazKey) const { return rd_.findPaz(pazKey); }
    // True if [offset, offset + length) of the paz in `slot` overlaps any record.
    bool touches(int slot, uint64_t offset, uint64_t length) const;
    ApplyReport apply(int slot, uint64_t offset, uint8_t* buffer, size_t length, OriginalReader* original);
    // Same rules for ONE record (index into reader().records()) of the paz the read belongs to; the
    // result is also added to `rep`. Returns None when the record does not overlap the read. Used by
    // the per-texture server (multi.h), which picks the serving pack texture by texture.
    RecordResult applyRecord(uint32_t rec, uint64_t offset, uint8_t* buffer, size_t length, OriginalReader* original,
                             ApplyReport& rep);

    // Payload cache bound; lowering it evicts immediately (the plugin splits one budget between the
    // packs it keeps open).
    void setCacheLimit(size_t bytes);
    size_t cacheLimit() const;

    ServeStats stats() const;

private:
    using Payload = std::shared_ptr<const std::vector<uint8_t>>;
    Payload payload(uint32_t rec, std::string* error);
    // Index range [first, last) of records in `slot` that overlap the read.
    void overlapping(int slot, uint64_t offset, uint64_t length, uint32_t& first, uint32_t& last) const;

    PackReader rd_;
    size_t cacheLimit_ = 0;
    mutable std::mutex m_;
    std::list<uint32_t> lru_;  // most recent first
    struct Slot {
        Payload data;
        std::list<uint32_t>::iterator it;
    };
    std::unordered_map<uint32_t, Slot> cache_;
    size_t cacheUsed_ = 0;
    std::unique_ptr<std::atomic<uint8_t>[]> disabled_;

    std::atomic<uint64_t> applyCalls_{0}, recordHits_{0}, patched_{0}, already_{0}, mismatch_{0}, unverifiable_{0},
        payloadErrors_{0}, partialChunks_{0}, bytesPatched_{0}, cacheHits_{0}, cacheMisses_{0}, disabledCount_{0};
};

}  // namespace qs::pack
