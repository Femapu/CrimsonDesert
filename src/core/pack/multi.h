// QuattroStagioni - several season packs open at once, one visual stage PER TEXTURE.
//
// Packs stay one per stage (they are the payload store). The plugin keeps the packs of every stage
// that is currently needed open (typically 2-4, see plan.h) and decides for each texture which of
// them serves it: stage = season::stageAt(cfg, region(texture), day, seed(texture)), so per-asset
// jitter and regional calendars really show up in game.
//
// Three immutable layers, so the read hooks never wait on the season tick:
//   LoadedPack     one open pack (PackServer) plus its serving counters; shared by every snapshot.
//   TextureIndex   built when the set of open packs changes: every texture any open pack knows
//                  (paz key + offset), with its region and asset seed (pack format v2; v1 records:
//                  Temperate and lookup3(path)) and, for each open pack, its record or kNoRecord.
//   ServeSnapshot  built when the day crosses a stage boundary of some texture: which pack serves
//                  each texture (or vanilla). The per-texture next change day is precomputed, so an
//                  update only re-evaluates the textures whose boundary was crossed.
// SnapshotSlot publishes the current snapshot with atomic shared_ptr load/store: a hook copies the
// pointer (a few instructions under libstdc++'s internal spin/mutex pool, never a long lock) and keeps
// using its copy even if the tick swaps in a new one meanwhile.
//
// Missing packs. When the pack of a texture's stage is not open, the texture is served by the nearest
// EARLIER stage in calendar order that has an open pack; walking back to the vanilla Summer plateau
// means vanilla. A texture that the chosen pack does not list (its look does not change at that stage,
// or the fitter kept it vanilla) is served vanilla. The manual override (forced stage) serves every
// texture from one pack, like the single-pack plugin did.
#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../season/season.h"
#include "serve.h"

namespace qs::pack {

struct LoadedPack {
    std::string stageId;                 // header().stageId
    std::string file;                    // file name (logs, menu)
    std::shared_ptr<PackServer> server;
    // Reads served through ServeSnapshot::apply (records, not reads).
    std::atomic<uint64_t> patched{0}, already{0}, fallbackPatched{0}, mismatch{0}, refused{0};
};

class TextureIndex {
public:
    static constexpr uint32_t kNoRecord = 0xFFFFFFFFu;
    struct Tex {
        uint64_t offset = 0;
        uint32_t storedSize = 0;
        uint32_t seed = 0;        // asset seed (lookup3(asset, 0xC5EDE))
        uint8_t region = 0;       // season::Region
        bool hasMeta = false;     // region/seed come from a v2 record
        uint32_t paz = 0;         // index into paz()
    };
    struct Paz {
        std::string key;          // "0012/3.paz"
        uint32_t first = 0, count = 0;  // textures [first, first + count), sorted by offset
    };

    // Builds the index of `packs` (any order). Records of different packs describing the same texture
    // (same paz key and offset) must have the same stored size; a record that conflicts with another
    // texture (different size, or overlapping) is ignored and counted in conflicts().
    static std::shared_ptr<const TextureIndex> build(std::vector<std::shared_ptr<LoadedPack>> packs);

    const std::vector<std::shared_ptr<LoadedPack>>& packs() const { return packs_; }
    const std::vector<Tex>& textures() const { return tex_; }
    const std::vector<Paz>& paz() const { return paz_; }
    int findPaz(const std::string& key) const;   // -1 if no open pack touches this paz
    uint32_t record(size_t tex, size_t pack) const { return recs_[tex * packs_.size() + pack]; }
    int packOfStage(const std::string& stageId) const;  // -1 if no open pack has that stage
    uint32_t id() const { return id_; }          // unique per build (cache key for per-archive slot caches)
    size_t conflicts() const { return conflicts_; }
    size_t bytes() const;                         // approximate memory used by the index
    // Texture range [first, last) of paz `slot` overlapping [off, off + len).
    void overlapping(int slot, uint64_t off, uint64_t len, uint32_t& first, uint32_t& last) const;

private:
    std::vector<std::shared_ptr<LoadedPack>> packs_;
    std::vector<Tex> tex_;
    std::vector<Paz> paz_;
    std::vector<uint32_t> recs_;  // tex_.size() x packs_.size()
    size_t conflicts_ = 0;
    uint32_t id_ = 0;
};

struct ServeSnapshot {
    std::shared_ptr<const TextureIndex> index;
    std::vector<int16_t> serve;        // per texture: pack index serving it, -1 = vanilla
    std::vector<int16_t> desired;      // per texture: calendar index of its own stage (-1 forced/unknown)
    std::vector<uint8_t> fallback;     // per texture: 1 when `serve` stands in for a missing stage pack
    std::vector<double> nextChange;    // per texture: absolute day of its next stage change
    double day = 0;
    double validUntil = 0;             // min(nextChange): the assignment holds for [day, validUntil)
    std::string configKey;             // stageConfigKey() of the config used
    std::string forcedStage;           // non-empty: manual override, one global stage
    bool forced = false;
    // Summary (menu / log).
    size_t servedTextures = 0;         // served by some pack
    size_t fallbackTextures = 0;       // of which by a fallback pack
    size_t missingTextures = 0;        // their own stage has no open pack (served by a fallback or vanilla)
    size_t updatedTextures = 0;        // textures re-evaluated by the last update (incremental)
    std::vector<std::string> missingStages;  // stages wanted by some texture with no open pack
    std::vector<uint32_t> packAssigned, packFallback;  // per pack: textures it serves / as fallback

    bool empty() const { return !index || servedTextures == 0; }
    int slotFor(const std::string& pazKey) const { return index ? index->findPaz(pazKey) : -1; }
    // True if a texture served by some pack overlaps the read.
    bool touches(int slot, uint64_t off, uint64_t len) const;
    // Substitutes every served texture overlapping a completed read (PackServer::applyRecord rules:
    // per-chunk verification, all-or-nothing per record). Thread safe.
    ApplyReport apply(int slot, uint64_t off, uint8_t* buf, size_t len, OriginalReader* original) const;
};

// String of the config fields that decide stages (calendar, transitions, jitter, regions). Two configs
// with the same key give the same stage for every texture and day.
std::string stageConfigKey(const season::SeasonConfig& cfg);

// Per-texture stages for `day`. When `prev` uses the same index and config and `day` >= prev->day,
// only the textures whose next change day was reached are re-evaluated; otherwise everything is.
std::shared_ptr<const ServeSnapshot> assignStages(const std::shared_ptr<const TextureIndex>& index,
                                                  const season::SeasonConfig& cfg, double day,
                                                  const ServeSnapshot* prev = nullptr);
// Manual override: every texture served by the pack of `stageId` (vanilla where it has no record).
// An empty or unknown stage id serves everything vanilla.
std::shared_ptr<const ServeSnapshot> assignForced(const std::shared_ptr<const TextureIndex>& index,
                                                  const std::string& stageId);

class SnapshotSlot {
public:
    std::shared_ptr<const ServeSnapshot> get() const { return std::atomic_load(&p_); }
    void set(std::shared_ptr<const ServeSnapshot> s) { std::atomic_store(&p_, std::move(s)); }

private:
    std::shared_ptr<const ServeSnapshot> p_;
};

}  // namespace qs::pack
