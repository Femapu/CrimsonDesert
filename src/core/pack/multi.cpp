// QuattroStagioni - per-texture stage serving from several packs. See multi.h.
#include "multi.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <set>

#include "../lookup3.h"
#include "plan.h"

namespace qs::pack {
namespace {

std::atomic<uint32_t> g_indexIds{1};

int stepsOf(const season::SeasonConfig& cfg) { return cfg.stageCount() / season::kPhaseCount - 1; }

struct Resolved {
    int pack = -1;          // pack serving this calendar stage (-1 vanilla)
    bool fallback = false;  // `pack` is an earlier stage standing in for a missing one
    bool missing = false;   // the stage itself has no open pack
};

// Calendar stage -> serving pack, with the fallback walk of multi.h.
std::vector<Resolved> resolveCalendar(const TextureIndex& idx, const season::SeasonConfig& cfg) {
    const int n = stepsOf(cfg), count = cfg.stageCount();
    std::vector<int> own(size_t(count), -1);
    for (int i = 0; i < count; ++i) {
        const std::string id = stageIdAt(cfg, i);
        const int p = idx.packOfStage(id);
        if (p < 0) continue;
        // A transition pack generated with other transition steps has another blend factor: unusable.
        const uint32_t ps = idx.packs()[size_t(p)]->server->reader().header().transitionSteps;
        if (i % (n + 1) != 0 && ps != 0 && int(ps) != n) continue;
        own[size_t(i)] = p;
    }
    std::vector<Resolved> res(static_cast<size_t>(count), Resolved());
    for (int d = 0; d < count; ++d) {
        Resolved& r = res[size_t(d)];
        if (stageIdAt(cfg, d) == "U") continue;  // vanilla summer: nothing to serve, nothing missing
        if (own[size_t(d)] >= 0) { r.pack = own[size_t(d)]; continue; }
        r.missing = true;
        for (int k = 1; k < count; ++k) {
            const int e = ((d - k) % count + count) % count;
            if (stageIdAt(cfg, e) == "U") break;
            if (own[size_t(e)] >= 0) {
                r.pack = own[size_t(e)];
                r.fallback = true;
                break;
            }
        }
    }
    return res;
}

void evaluate(const TextureIndex& idx, const season::SeasonConfig& cfg, const std::vector<Resolved>& res, double day,
              size_t t, ServeSnapshot& s) {
    const TextureIndex::Tex& x = idx.textures()[t];
    const season::Region r = season::Region(x.region < season::kRegionCount ? x.region : 0);
    const double j = double(season::assetJitter(cfg, x.seed));
    const season::VisualStage st = season::stageAt(cfg, r, day + j);
    const int n = stepsOf(cfg);
    const int d = int(st.from) * (n + 1) + st.step;
    s.desired[t] = int16_t(d);
    const double next = cfg.nextStageChangeDay(day + j, r);
    // Exactly at a boundary stageAt may still round to the old stage while nextStageChangeDay already
    // points at the following boundary: then the change is due now (re-evaluated at the next update).
    const bool stale = std::isfinite(next) && season::stageAt(cfg, r, 0.5 * (day + j + next)) != st;
    s.nextChange[t] = stale ? day : next - j;
    const Resolved& rv = res[size_t(d)];
    const bool has = rv.pack >= 0 && idx.record(t, size_t(rv.pack)) != TextureIndex::kNoRecord;
    s.serve[t] = int16_t(has ? rv.pack : -1);
    s.fallback[t] = uint8_t(has && rv.fallback);
}

void summarize(const std::vector<Resolved>* res, const season::SeasonConfig* cfg, ServeSnapshot& s) {
    const size_t np = s.index->packs().size();
    s.packAssigned.assign(np, 0);
    s.packFallback.assign(np, 0);
    s.servedTextures = s.fallbackTextures = s.missingTextures = 0;
    s.validUntil = std::numeric_limits<double>::infinity();
    std::set<int> missing;
    for (size_t t = 0; t < s.serve.size(); ++t) {
        if (s.serve[t] >= 0) {
            s.servedTextures++;
            s.packAssigned[size_t(s.serve[t])]++;
            if (s.fallback[t]) { s.fallbackTextures++; s.packFallback[size_t(s.serve[t])]++; }
        }
        if (res && s.desired[t] >= 0 && (*res)[size_t(s.desired[t])].missing) {
            s.missingTextures++;
            missing.insert(s.desired[t]);
        }
        if (!s.nextChange.empty()) s.validUntil = std::min(s.validUntil, s.nextChange[t]);
    }
    s.missingStages.clear();
    if (cfg)
        for (int d : missing) s.missingStages.push_back(stageIdAt(*cfg, d));
}

}  // namespace

// ------------------------------------------------------------------------------------ TextureIndex
std::shared_ptr<const TextureIndex> TextureIndex::build(std::vector<std::shared_ptr<LoadedPack>> packs) {
    std::shared_ptr<TextureIndex> idx(new TextureIndex());
    packs.erase(std::remove_if(packs.begin(), packs.end(),
                               [](const std::shared_ptr<LoadedPack>& p) { return !p || !p->server; }),
                packs.end());
    if (packs.size() > 0x7FFF) packs.resize(0x7FFF);
    idx->packs_ = packs;
    idx->id_ = g_indexIds.fetch_add(1);
    struct E {
        std::string key;
        uint64_t offset;
        uint32_t size, seed, rec;
        uint8_t region;
        bool meta;
        uint16_t pack;
    };
    std::vector<E> es;
    for (size_t p = 0; p < packs.size(); ++p) {
        const PackReader& rd = packs[p]->server->reader();
        for (const PazInfo& z : rd.paz()) {
            const std::string key = z.key();
            for (uint32_t k = z.firstRecord; k < z.firstRecord + z.recordCount; ++k) {
                const Record& r = rd.records()[k];
                E e{key, r.offset, r.storedSize, r.assetSeed, k, r.region, r.hasMeta, uint16_t(p)};
                if (!r.hasMeta) {  // v1 pack: Temperate and a path seed (no asset name stored)
                    e.seed = cdHash(r.path.data(), r.path.size());
                    e.region = uint8_t(season::Region::Temperate);
                }
                es.push_back(std::move(e));
            }
        }
    }
    std::sort(es.begin(), es.end(), [](const E& a, const E& b) {
        if (a.key != b.key) return a.key < b.key;
        if (a.offset != b.offset) return a.offset < b.offset;
        return a.pack < b.pack;
    });
    const size_t np = packs.size();
    for (const E& e : es) {
        bool same = false;
        if (!idx->paz_.empty() && idx->paz_.back().key == e.key && idx->paz_.back().count) {
            const Tex& last = idx->tex_.back();
            if (last.offset == e.offset && last.storedSize == e.size) same = true;
            else if (e.offset < last.offset + last.storedSize) { idx->conflicts_++; continue; }
        }
        if (!same) {
            if (idx->paz_.empty() || idx->paz_.back().key != e.key) {
                Paz z;
                z.key = e.key;
                z.first = uint32_t(idx->tex_.size());
                idx->paz_.push_back(z);
            }
            Tex t;
            t.offset = e.offset;
            t.storedSize = e.size;
            t.seed = e.seed;
            t.region = e.region;
            t.hasMeta = e.meta;
            t.paz = uint32_t(idx->paz_.size() - 1);
            idx->tex_.push_back(t);
            idx->paz_.back().count++;
            idx->recs_.insert(idx->recs_.end(), np, kNoRecord);
        } else if (e.meta && !idx->tex_.back().hasMeta) {  // prefer v2 metadata over a v1 guess
            idx->tex_.back().seed = e.seed;
            idx->tex_.back().region = e.region;
            idx->tex_.back().hasMeta = true;
        }
        uint32_t& slot = idx->recs_[(idx->tex_.size() - 1) * np + e.pack];
        if (slot == kNoRecord) slot = e.rec;
        else idx->conflicts_++;
    }
    return idx;
}

int TextureIndex::findPaz(const std::string& key) const {
    size_t lo = 0, hi = paz_.size();
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        const int c = paz_[mid].key.compare(key);
        if (c == 0) return int(mid);
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    return -1;
}

int TextureIndex::packOfStage(const std::string& stageId) const {
    for (size_t p = 0; p < packs_.size(); ++p)
        if (packs_[p]->stageId == stageId) return int(p);
    return -1;
}

size_t TextureIndex::bytes() const {
    size_t b = tex_.size() * sizeof(Tex) + recs_.size() * sizeof(uint32_t);
    for (const Paz& z : paz_) b += sizeof(Paz) + z.key.size();
    return b;
}

void TextureIndex::overlapping(int slot, uint64_t off, uint64_t len, uint32_t& first, uint32_t& last) const {
    first = last = 0;
    if (slot < 0 || size_t(slot) >= paz_.size() || len == 0) return;
    const Paz& z = paz_[size_t(slot)];
    const uint32_t b = z.first, e = z.first + z.count;
    uint32_t lo = b, hi = e;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (tex_[mid].offset <= off) lo = mid + 1;
        else hi = mid;
    }
    uint32_t i = lo;
    if (i > b && tex_[i - 1].offset + tex_[i - 1].storedSize > off) --i;
    first = i;
    const uint64_t end = off + len;
    while (i < e && tex_[i].offset < end) ++i;
    last = i;
}

// ------------------------------------------------------------------------------------ ServeSnapshot
bool ServeSnapshot::touches(int slot, uint64_t off, uint64_t len) const {
    if (!index) return false;
    uint32_t a, b;
    index->overlapping(slot, off, len, a, b);
    for (uint32_t t = a; t < b; ++t)
        if (serve[t] >= 0) return true;
    return false;
}

ApplyReport ServeSnapshot::apply(int slot, uint64_t off, uint8_t* buf, size_t len, OriginalReader* original) const {
    ApplyReport rep;
    if (!index || !buf) return rep;
    uint32_t a, b;
    index->overlapping(slot, off, len, a, b);
    for (uint32_t t = a; t < b; ++t) {
        const int p = serve[t];
        if (p < 0) continue;
        const uint32_t rec = index->record(t, size_t(p));
        LoadedPack& lp = *index->packs()[size_t(p)];
        switch (lp.server->applyRecord(rec, off, buf, len, original, rep)) {
            case RecordResult::Patched:
                lp.patched++;
                if (fallback[t]) lp.fallbackPatched++;
                break;
            case RecordResult::Already: lp.already++; break;
            case RecordResult::Mismatch: lp.mismatch++; break;
            case RecordResult::Unverifiable:
            case RecordResult::PayloadError: lp.refused++; break;
            case RecordResult::None: break;
        }
    }
    return rep;
}

// ------------------------------------------------------------------------------------ assignment
std::string stageConfigKey(const season::SeasonConfig& c) {
    std::string k;
    char buf[96];
    auto f = [&](double v) {
        std::snprintf(buf, sizeof buf, "%.9g;", v);
        k += buf;
    };
    for (float d : c.phaseDays) f(d);
    f(c.transitionFraction);
    f(c.transitionSteps);
    f(c.assetJitterDays);
    for (const season::RegionConfig& r : c.regions) {
        f(r.enabled ? 1 : 0);
        f(r.dayOffset);
        for (float s : r.phaseScale) f(s);
    }
    return k;
}

std::shared_ptr<const ServeSnapshot> assignStages(const std::shared_ptr<const TextureIndex>& index,
                                                  const season::SeasonConfig& cfg, double day, const ServeSnapshot* prev) {
    std::shared_ptr<ServeSnapshot> s(new ServeSnapshot());
    s->index = index;
    s->day = day;
    s->configKey = stageConfigKey(cfg);
    if (!index) return s;
    const size_t nt = index->textures().size();
    const std::vector<Resolved> res = resolveCalendar(*index, cfg);
    const bool incremental = prev && prev->index == index && !prev->forced && prev->configKey == s->configKey &&
                             day >= prev->day && prev->serve.size() == nt;
    if (incremental) {
        s->serve = prev->serve;
        s->desired = prev->desired;
        s->fallback = prev->fallback;
        s->nextChange = prev->nextChange;
        for (size_t t = 0; t < nt; ++t)
            if (s->nextChange[t] <= day) {
                evaluate(*index, cfg, res, day, t, *s);
                s->updatedTextures++;
            }
    } else {
        s->serve.assign(nt, -1);
        s->desired.assign(nt, -1);
        s->fallback.assign(nt, 0);
        s->nextChange.assign(nt, 0);
        for (size_t t = 0; t < nt; ++t) evaluate(*index, cfg, res, day, t, *s);
        s->updatedTextures = nt;
    }
    summarize(&res, &cfg, *s);
    return s;
}

std::shared_ptr<const ServeSnapshot> assignForced(const std::shared_ptr<const TextureIndex>& index,
                                                  const std::string& stageId) {
    std::shared_ptr<ServeSnapshot> s(new ServeSnapshot());
    s->index = index;
    s->forced = true;
    s->forcedStage = stageId;
    if (!index) return s;
    const size_t nt = index->textures().size();
    const int p = stageId.empty() ? -1 : index->packOfStage(stageId);
    s->serve.assign(nt, -1);
    s->desired.assign(nt, -1);
    s->fallback.assign(nt, 0);
    for (size_t t = 0; t < nt; ++t)
        if (p >= 0 && index->record(t, size_t(p)) != TextureIndex::kNoRecord) s->serve[t] = int16_t(p);
    s->updatedTextures = nt;
    summarize(nullptr, nullptr, *s);
    return s;
}

}  // namespace qs::pack
