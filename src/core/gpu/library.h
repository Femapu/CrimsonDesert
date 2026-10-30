// QuattroStagioni - GPU live swap, part 3: stage library (portable, unit tested).
//
// Knows, for every texture of every opened season pack, where its data lives for each stage slot:
// slot 0 = the original stored bytes in the game's .paz (verified against the record's origHash),
// slot k = the replacement payload of the k-th pack. Builds the FingerprintIndex (prints of the original
// and of every replacement, since textures loaded while a pack is active carry the pack's bytes) and
// serves decoded mip data for the swap uploads, with a small LRU cache of decoded textures.
//
// Prints: the generator should store them in the pack index (proposed fields in docs/dev/gpu_swap.md).
// Until then `StoredPrintsFn` returns false and the library computes them from the data when a pack is
// added: it reads and decodes every payload and every original (about the size of the pack itself),
// so the plugin must call addPack() from a background thread.
#pragma once
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "../pack/qsp_io.h"
#include "swap_model.h"

namespace qs::gpu {

struct PackLoadStats {
    uint32_t records = 0, indexed = 0, skippedFormat = 0, originalErrors = 0, payloadErrors = 0;
    uint64_t bytesRead = 0;
    bool printsFromIndex = false;
};

class StageLibrary {
public:
    // Opens a .paz of the game for raw reads (the plugin must bypass its own ReadFile hooks).
    using PazOpener = std::function<std::unique_ptr<pack::ByteSource>(const std::string& group, uint32_t pazIndex)>;
    // Prints stored in the pack index (future field). Return false to have them computed from the data.
    using StoredPrintsFn = std::function<bool(const pack::PackReader&, size_t record, TexturePrints& original,
                                              TexturePrints& replacement)>;

    explicit StageLibrary(PazOpener opener, StoredPrintsFn stored = nullptr, size_t cacheBytes = 64u << 20);

    // Opens a pack (header and index checked) and indexes its records. Returns the stage slot (> 0) or -1.
    // Adding the same stage id again returns the existing slot without reloading.
    int addPack(std::unique_ptr<pack::ByteSource> src, std::string* error = nullptr, PackLoadStats* stats = nullptr);

    const FingerprintIndex& index() const { return index_; }
    int slotOf(const std::string& stageId) const { return index_.slotOf(stageId); }
    std::string stageOf(int slot) const { return index_.stageOf(slot); }
    // Slot to show for `texture` when `activeSlot` is the current stage: activeSlot if its pack changes the
    // texture, else 0 (the stage keeps the original look for it).
    int targetFor(uint32_t texture, int activeSlot) const;
    // Packed size of a mip (0 if unknown).
    uint64_t mipBytes(uint32_t texture, uint32_t ddsMip) const;

    // Decoded data of one mip: `dds` holds the whole decoded texture, the mip is [offset, offset + size).
    struct MipData {
        std::shared_ptr<const std::vector<uint8_t>> dds;
        uint64_t offset = 0, size = 0;
        MipLayout layout;
        Family family = Family::Unknown;
    };
    bool mipData(uint32_t texture, int slot, uint32_t ddsMip, MipData& out, std::string* error = nullptr);

    size_t cacheBytes() const;

private:
    struct Source {
        std::string group;
        uint32_t pazIndex = 0;
        uint64_t offset = 0;
        uint32_t storedSize = 0, origHash = 0;
        std::vector<int32_t> recBySlot;  // record index in packs_[slot], -1 = none
        std::vector<uint64_t> mipOffset;  // inside the decoded DDS, header included
    };
    pack::ByteSource* paz(const std::string& group, uint32_t pazIndex);
    bool decoded(uint32_t texture, int slot, std::shared_ptr<const std::vector<uint8_t>>& out, std::string* error);

    PazOpener opener_;
    StoredPrintsFn stored_;
    FingerprintIndex index_;
    mutable std::mutex m_;  // sources_, packs_, paz_, cache
    std::vector<std::unique_ptr<pack::PackReader>> packs_;  // by slot (slot 0 = null)
    std::vector<Source> sources_;                           // by texture id
    std::map<std::string, std::unique_ptr<pack::ByteSource>> paz_;
    std::mutex pazMutex_;
    size_t cacheLimit_;
    size_t cacheUsed_ = 0;
    std::list<uint64_t> lru_;
    struct Cached {
        std::shared_ptr<const std::vector<uint8_t>> data;
        std::list<uint64_t>::iterator it;
    };
    std::unordered_map<uint64_t, Cached> cache_;
};

}  // namespace qs::gpu
