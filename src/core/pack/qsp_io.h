// QuattroStagioni - .qsp season pack file format: writer (qs_gen) and reader (qs_gen, plugin, tests).
//
// A pack holds, for one visual stage, the replacement stored bytes of every texture that changes.
// Designed for ~1.6 GB of payload (3688 textures) with a small index (a few MB) that the plugin keeps
// in RAM; payloads stay on disk and are read on demand.
//
// File layout (little endian):
//   [0, 64)        fixed header
//       0  char[8] magic "QSPACK\x1A\x01"
//       8  u32 formatVersion (2; 1 is still read)   12 u32 headerSize (64)
//      16  u64 indexOffset             24 u64 indexSize
//      32  u64 payloadOffset (64)      40 u64 payloadSize
//      48  u32 recordCount             52 u32 indexHash  = lookup3(index bytes, 0xC5EDE)
//      56  u32 reserved (0)            60 u32 headerHash = lookup3(header[0, 60), 0xC5EDE)
//   [64, ...)      payloads: replacement stored bytes, concatenated in record order
//   [indexOffset]  index (written last, so payloads stream to disk with bounded memory):
//       u32 'QSPI'
//       str stageId, str gameBuild, str generator, u32 recipeVersion, u32 chunkShift (12)
//       v2 only: u32 transitionSteps
//       u32 nGroups  x { str group, u32 pamtSelfHash }
//       u32 nPaz     x { str group, u32 pazIndex, u64 pazSize, u32 firstRecord, u32 recordCount }
//       u32 nRecords x { u32 pazSlot, u64 offset, u32 storedSize, u32 size, u32 flags, u32 origHash,
//                        u32 dataHash, u64 dataOffset, u32 chunkFirst,
//                        v2 only: u32 assetSeed, u32 region,
//                        str path }
//       u32 nChunks  x u32 original chunk hash
//     str = u16 length + bytes (UTF-8). Paz slots are sorted by key ("<group>/<n>.paz"); records are
//     sorted by (slot, offset) and never overlap, so a read is matched with a binary search.
//
// Original chunk hashes: the paz file is cut into 4 KiB chunks aligned to ABSOLUTE file offsets;
// chunk j of a record covers [max(offset, (b + j) * 4096), min(end, (b + j + 1) * 4096)) with
// b = offset / 4096, hashed with lookup3(bytes, 0xC5EDE). A game read that fully covers a chunk is
// verified without keeping the original bytes in memory; aligned reads (any 4 KiB multiple) and
// whole-entry reads always cover their chunks fully. See serve.h for partial chunks.
//
// Version 2 (per-texture stages, multi.h) adds the transition steps of the stage id and, per record,
// the texture's region (season::Region) and asset seed (lookup3(asset, 0xC5EDE)) from the inventory,
// so the plugin can compute each texture's own stage. Version 1 packs are still read: their records
// get hasMeta = false (the plugin then assumes Temperate and lookup3(path)).
//
// Integrity: header hash, index hash, per-record dataHash (checked when a payload is loaded), and
// structural checks on load (sorted, non-overlapping, inside the paz and payload ranges).
// Binding: gameBuild (exe size + PE timestamp) and the self hash of every group's 0.pamt.
#pragma once
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "byte_source.h"
#include "qsp.h"

namespace qs::pack {

constexpr uint32_t kFormatVersion = 2;      // written by PackWriter
constexpr uint32_t kMinFormatVersion = 1;   // oldest version PackReader accepts
constexpr uint32_t kHeaderSize = 64;
constexpr uint32_t kChunkShift = 12;
constexpr uint32_t kChunkSize = 1u << kChunkShift;
constexpr uint32_t kMaxRecordSize = 256u << 20;  // sanity limit for one payload

struct PazInfo {
    std::string group;
    uint32_t pazIndex = 0;
    uint64_t pazSize = 0;
    uint32_t firstRecord = 0, recordCount = 0;
    std::string key() const;  // "0012/3.paz"
};

std::string pazKey(const std::string& group, uint32_t pazIndex);
// Canonical game build string stored in packs, e.g. "exe 336264088 6B2F3AF2".
std::string gameBuildId(uint64_t exeSize, uint32_t peTimestamp);

// Chunk grid helpers (absolute 4 KiB alignment, clipped to the record range).
uint32_t chunkCount(uint64_t offset, uint32_t size);
void chunkRange(uint64_t offset, uint32_t size, uint32_t j, uint64_t& begin, uint64_t& end);
uint32_t chunkIndexOf(uint64_t recordOffset, uint64_t fileOffset);  // chunk containing fileOffset

class PackWriter {
public:
    ~PackWriter();
    // Creates `path`.tmp; finish() renames it to `path`.
    bool open(const std::string& path, const Header& header, std::string* error = nullptr);
    // Format version to write (default kFormatVersion; 1 writes the old layout, for tests). Call before open().
    void setFormatVersion(uint32_t v) { version_ = v; }
    void setPazSize(const std::string& group, uint32_t pazIndex, uint64_t size);
    // Adds one substitution. `r` needs group, path, pazIndex, offset, storedSize (size, flags, region,
    // assetSeed optional).
    // Hashes and chunk hashes are computed here; the replacement is appended to the payload area.
    bool add(const Record& r, const uint8_t* original, const uint8_t* replacement, std::string* error = nullptr);
    bool finish(std::string* error = nullptr);
    void abort();
    uint64_t payloadBytes() const { return payloadSize_; }
    size_t recordCount() const { return recs_.size(); }

private:
    struct Pending {
        Record r;
        std::vector<uint32_t> chunks;
    };
    std::string path_, tmp_;
    FILE* f_ = nullptr;
    Header header_;
    uint64_t payloadSize_ = 0;
    uint32_t version_ = kFormatVersion;
    std::vector<Pending> recs_;
    std::vector<PazInfo> pazSizes_;
};

class PackReader {
public:
    // Reads and checks header and index (not the payloads).
    bool open(std::unique_ptr<ByteSource> src, std::string* error = nullptr);
    bool openFile(const std::string& utf8Path, std::string* error = nullptr);

    const Header& header() const { return header_; }
    const std::vector<Record>& records() const { return recs_; }
    const std::vector<PazInfo>& paz() const { return paz_; }
    uint64_t payloadSize() const { return payloadSize_; }
    int findPaz(const std::string& key) const;  // -1 if absent
    const uint32_t* chunkHashes(const Record& r) const { return chunks_.data() + r.chunkFirst; }
    size_t chunkHashCount() const { return chunks_.size(); }
    size_t indexBytes() const { return indexSize_; }

    // Loads one payload and checks its dataHash.
    bool readPayload(const Record& r, std::vector<uint8_t>& out, std::string* error = nullptr) const;
    // Reads every payload (qs_gen --check-pack).
    bool verifyAll(std::string* error = nullptr) const;

private:
    std::unique_ptr<ByteSource> src_;
    Header header_;
    std::vector<Record> recs_;
    std::vector<PazInfo> paz_;
    std::vector<uint32_t> chunks_;
    uint64_t payloadOffset_ = 0, payloadSize_ = 0, indexSize_ = 0;
};

}  // namespace qs::pack
