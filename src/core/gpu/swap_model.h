// QuattroStagioni - GPU live swap, part 2: fingerprint index, resident texture tracker, upload budget
// scheduler (portable, unit tested; the D3D12 side is src/win/gpu/gpu_swap.cpp).
//
// Stages are small integers ("slots"): slot 0 is the original game texture (summer, "U", no pack), slot
// k > 0 is the k-th season pack opened by the library. A print can belong to several slots of the same
// texture (a stage that leaves a mip unchanged), so matches carry a slot MASK. A print shared by two
// different textures is "ambiguous" and never identifies anything (tiny mips, flat colours).
//
// Resources are opaque 64-bit keys (the ID3D12Resource pointer on Windows). For every resource the
// tracker keeps which texture it holds, which DDS mip its subresource 0 is (the game may skip top mips
// for lower texture quality), the slot mask of the content of each subresource and the resource state
// the game leaves it in after its uploads (observed from ResourceBarrier, default COMMON).
// All classes are thread safe (internal locks), because the hooks run on every recording thread.
#pragma once
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "fingerprint.h"

namespace qs::gpu {

constexpr int kMaxSlots = 63;                  // slot masks are 64-bit; bit 63 = "uploaded, stage unknown"
constexpr uint64_t kUnknownContent = 1ull << 63;
constexpr uint32_t kNoTexture = 0xFFFFFFFFu;
constexpr uint32_t kAmbiguous = 0xFFFFFFFEu;
inline uint64_t slotBit(int slot) { return 1ull << slot; }

struct TextureDesc {
    std::string key;            // "<group>/<paz>.paz@<offset>": same texture in every pack
    Family family = Family::Unknown;
    uint32_t dxgiFormat = 0, width = 0, height = 0, mipCount = 0;
    uint64_t slots = 0;         // slots whose prints were added
};

struct Match {
    uint32_t texture = kNoTexture;  // kAmbiguous if the print belongs to several textures
    uint32_t mip = 0;               // DDS mip of the print
    uint64_t slots = 0;             // slots whose data has this print at this mip
    bool found() const { return texture != kNoTexture && texture != kAmbiguous; }
};

class FingerprintIndex {
public:
    FingerprintIndex();
    // Stage slots. "" / "U" / "none" are slot 0. addStage returns the existing slot for a known id.
    int addStage(const std::string& stageId);
    int slotOf(const std::string& stageId) const;  // -1 if unknown
    std::string stageOf(int slot) const;

    // Registers a texture (same key -> same id). Returns kNoTexture if the key exists with another shape.
    uint32_t addTexture(const std::string& key, const TexturePrints& shape);
    // Adds the prints of one slot of a texture (one per mip, same count as the texture's mips).
    bool addPrints(uint32_t texture, int slot, const std::vector<uint64_t>& prints);

    bool lookup(uint64_t print, Match& out) const;
    // Fast prefilter for the copy hook: is there any indexed mip of this family and size?
    bool hasShape(Family f, uint32_t width, uint32_t height) const;
    bool texture(uint32_t id, TextureDesc& out) const;
    size_t textureCount() const;
    size_t printCount() const;
    size_t ambiguousCount() const;

private:
    struct Entry {
        uint32_t texture;
        uint32_t mip;
        uint64_t slots;
    };
    static uint64_t shapeKey(Family f, uint32_t w, uint32_t h);
    mutable std::shared_mutex m_;
    std::vector<std::string> stages_;
    std::vector<TextureDesc> textures_;
    std::unordered_map<std::string, uint32_t> byKey_;
    std::unordered_map<uint64_t, Entry> prints_;
    std::unordered_set<uint64_t> shapes_;
    size_t ambiguous_ = 0;
};

// Shape of a destination resource (from ID3D12Resource::GetDesc).
struct ResourceShape {
    uint32_t dxgiFormat = 0;
    uint32_t width = 0, height = 0;
    uint16_t mipLevels = 1, arraySize = 1;
};

struct Resident {
    uint32_t texture = kNoTexture;
    int32_t mipOffset = 0;          // DDS mip of subresource s = s + mipOffset
    ResourceShape shape;
    std::vector<uint64_t> content;  // per subresource: slot mask of the content, 0 = never uploaded by the game
    uint32_t state = 0;             // D3D12_RESOURCE_STATES the game leaves the texture in (0 = COMMON)
    bool stateSeen = false;
    uint64_t lastUpload = 0;        // tick of the last game upload (re-swap jobs wait a little after it)
};

struct SwapJob {
    uint64_t resource = 0;
    uint32_t texture = kNoTexture;
    uint32_t sub = 0;       // destination subresource
    uint32_t ddsMip = 0;    // source mip in the texture data
    int slot = 0;           // stage data to write
    uint64_t bytes = 0;     // packed size of the mip
    uint64_t notBefore = 0; // scheduler tick
};

class ResidentTracker {
public:
    enum class Upload { Ignored, New, Updated, Retargeted };
    // A game upload covering the whole subresource `sub` of `res` matched `m` at tick `now`.
    // Returns Ignored when the match does not fit the resource shape (wrong size, array texture...).
    Upload onUpload(uint64_t res, const ResourceShape& shape, uint32_t sub, const Match& m, uint64_t now);
    // A full upload into a tracked resource with content we do not know: the resource now holds
    // something else (pooled resource reused for another texture), so it is dropped. True if dropped.
    bool onUnknownUpload(uint64_t res, uint32_t sub);
    // CopyResource / texture-to-texture CopyTextureRegion: dst inherits src's tracking (defragmentation).
    bool onCopy(uint64_t dst, uint64_t src, const ResourceShape& dstShape, int dstSub = -1, int srcSub = -1);
    // Transition barrier seen on a tracked resource (state after the barrier).
    void onState(uint64_t res, uint32_t stateAfter);
    bool onDestroyed(uint64_t res);
    // Our own copy wrote `slot` data into `sub`.
    void onSwapped(uint64_t res, uint32_t sub, int slot);

    bool get(uint64_t res, Resident& out) const;
    bool tracked(uint64_t res) const;
    // Runs fn under the tracker lock if `res` is tracked; returns false otherwise. The D3D12 side takes its
    // reference on the resource inside fn, so a concurrent destruction callback (onDestroyed, same lock)
    // cannot free the object between the check and the AddRef.
    bool withTracked(uint64_t res, const std::function<void(const Resident&)>& fn) const;
    size_t size() const;
    std::vector<uint64_t> resources() const;

    // Jobs that bring every uploaded subresource to the slot returned by target(texture) (-1 = leave).
    using TargetFn = std::function<int(uint32_t texture)>;
    using BytesFn = std::function<uint64_t(uint32_t texture, uint32_t ddsMip)>;
    void plan(const TargetFn& target, const BytesFn& bytes, std::vector<SwapJob>& out, uint64_t notBefore = 0) const;
    void planResource(uint64_t res, const TargetFn& target, const BytesFn& bytes, std::vector<SwapJob>& out,
                      uint64_t notBefore = 0) const;

private:
    void planOne(uint64_t res, const Resident& r, const TargetFn& target, const BytesFn& bytes, std::vector<SwapJob>& out,
                 uint64_t notBefore) const;
    mutable std::mutex m_;
    std::unordered_map<uint64_t, Resident> res_;
};

// FIFO of swap jobs with a byte budget per tick. A newer job for the same (resource, subresource)
// replaces the queued one. take() never returns a job before its notBefore tick and returns at least one
// job when one is eligible, even if it alone exceeds the budget (the caller decides if it fits).
class SwapScheduler {
public:
    void push(const SwapJob& j);
    void clear();
    void dropResource(uint64_t res);
    std::vector<SwapJob> take(uint64_t budgetBytes, uint64_t now);
    size_t size() const;
    uint64_t queuedBytes() const;

private:
    static uint64_t key(uint64_t res, uint32_t sub) { return res * 1315423911ull ^ sub; }
    struct Item {
        SwapJob job;
        uint64_t k;
    };
    mutable std::mutex m_;
    std::list<Item> q_;
    std::unordered_multimap<uint64_t, std::list<Item>::iterator> byKey_;
    uint64_t bytes_ = 0;
};

}  // namespace qs::gpu
