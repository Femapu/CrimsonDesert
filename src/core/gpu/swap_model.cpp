// QuattroStagioni - GPU live swap: fingerprint index, resident tracker, budget scheduler.
#include "swap_model.h"

#include <algorithm>

namespace qs::gpu {

// ------------------------------------------------------------------ FingerprintIndex
FingerprintIndex::FingerprintIndex() { stages_.push_back(""); }

static bool isVanillaId(const std::string& s) { return s.empty() || s == "U" || s == "none"; }

int FingerprintIndex::addStage(const std::string& id) {
    if (isVanillaId(id)) return 0;
    std::unique_lock<std::shared_mutex> lk(m_);
    for (size_t i = 0; i < stages_.size(); ++i)
        if (stages_[i] == id) return int(i);
    if (stages_.size() > size_t(kMaxSlots - 1)) return -1;
    stages_.push_back(id);
    return int(stages_.size() - 1);
}

int FingerprintIndex::slotOf(const std::string& id) const {
    if (isVanillaId(id)) return 0;
    std::shared_lock<std::shared_mutex> lk(m_);
    for (size_t i = 0; i < stages_.size(); ++i)
        if (stages_[i] == id) return int(i);
    return -1;
}

std::string FingerprintIndex::stageOf(int slot) const {
    std::shared_lock<std::shared_mutex> lk(m_);
    if (slot <= 0 || size_t(slot) >= stages_.size()) return "U";
    return stages_[size_t(slot)];
}

uint64_t FingerprintIndex::shapeKey(Family f, uint32_t w, uint32_t h) {
    return (uint64_t(f) << 56) | (uint64_t(w & 0x0FFFFFFFu) << 28) | uint64_t(h & 0x0FFFFFFFu);
}

uint32_t FingerprintIndex::addTexture(const std::string& key, const TexturePrints& s) {
    std::unique_lock<std::shared_mutex> lk(m_);
    auto it = byKey_.find(key);
    if (it != byKey_.end()) {
        const TextureDesc& t = textures_[it->second];
        if (t.family != s.family || t.width != s.width || t.height != s.height || t.mipCount != s.mipCount)
            return kNoTexture;
        return it->second;
    }
    TextureDesc t;
    t.key = key;
    t.family = s.family;
    t.dxgiFormat = s.dxgiFormat;
    t.width = s.width;
    t.height = s.height;
    t.mipCount = s.mipCount;
    textures_.push_back(t);
    uint32_t id = uint32_t(textures_.size() - 1);
    byKey_[key] = id;
    for (uint32_t m = 0; m < s.mipCount; ++m) {
        MipLayout l = mipLayout(s.family, s.width, s.height, m);
        shapes_.insert(shapeKey(s.family, l.width, l.height));
    }
    return id;
}

bool FingerprintIndex::addPrints(uint32_t tex, int slot, const std::vector<uint64_t>& prints) {
    if (slot < 0 || slot >= kMaxSlots) return false;
    std::unique_lock<std::shared_mutex> lk(m_);
    if (tex >= textures_.size() || prints.size() != textures_[tex].mipCount) return false;
    textures_[tex].slots |= slotBit(slot);
    for (uint32_t m = 0; m < prints.size(); ++m) {
        auto it = prints_.find(prints[m]);
        if (it == prints_.end()) {
            prints_.emplace(prints[m], Entry{tex, m, slotBit(slot)});
        } else if (it->second.texture == tex && it->second.mip == m) {
            it->second.slots |= slotBit(slot);
        } else if (it->second.texture != kAmbiguous) {
            it->second.texture = kAmbiguous;
            ++ambiguous_;
        }
    }
    return true;
}

bool FingerprintIndex::lookup(uint64_t print, Match& out) const {
    std::shared_lock<std::shared_mutex> lk(m_);
    auto it = prints_.find(print);
    if (it == prints_.end()) {
        out = Match();
        return false;
    }
    out.texture = it->second.texture;
    out.mip = it->second.mip;
    out.slots = it->second.slots;
    return out.found();
}

bool FingerprintIndex::hasShape(Family f, uint32_t w, uint32_t h) const {
    std::shared_lock<std::shared_mutex> lk(m_);
    return shapes_.count(shapeKey(f, w, h)) != 0;
}

bool FingerprintIndex::texture(uint32_t id, TextureDesc& out) const {
    std::shared_lock<std::shared_mutex> lk(m_);
    if (id >= textures_.size()) return false;
    out = textures_[id];
    return true;
}

size_t FingerprintIndex::textureCount() const {
    std::shared_lock<std::shared_mutex> lk(m_);
    return textures_.size();
}

size_t FingerprintIndex::printCount() const {
    std::shared_lock<std::shared_mutex> lk(m_);
    return prints_.size();
}

size_t FingerprintIndex::ambiguousCount() const {
    std::shared_lock<std::shared_mutex> lk(m_);
    return ambiguous_;
}

// ------------------------------------------------------------------ ResidentTracker
ResidentTracker::Upload ResidentTracker::onUpload(uint64_t res, const ResourceShape& shape, uint32_t sub, const Match& m,
                                                  uint64_t now) {
    if (!m.found() || shape.arraySize != 1 || sub >= shape.mipLevels) return Upload::Ignored;
    const int32_t off = int32_t(m.mip) - int32_t(sub);
    if (off < 0) return Upload::Ignored;
    std::lock_guard<std::mutex> lk(m_);
    auto it = res_.find(res);
    Upload kind = Upload::Updated;
    if (it == res_.end() || it->second.texture != m.texture || it->second.mipOffset != off ||
        it->second.shape.mipLevels != shape.mipLevels || it->second.shape.width != shape.width ||
        it->second.shape.height != shape.height) {
        kind = it == res_.end() ? Upload::New : Upload::Retargeted;
        Resident r;
        r.texture = m.texture;
        r.mipOffset = off;
        r.shape = shape;
        r.content.assign(shape.mipLevels, 0);
        if (it != res_.end() && it->second.stateSeen) {  // same D3D12 object: keep what we learned
            r.state = it->second.state;
            r.stateSeen = true;
        }
        res_[res] = std::move(r);
        it = res_.find(res);
    }
    it->second.content[sub] = m.slots ? m.slots : kUnknownContent;
    it->second.lastUpload = now;
    return kind;
}

bool ResidentTracker::onUnknownUpload(uint64_t res, uint32_t sub) {
    std::lock_guard<std::mutex> lk(m_);
    auto it = res_.find(res);
    if (it == res_.end()) return false;
    res_.erase(it);
    return true;
}

bool ResidentTracker::onCopy(uint64_t dst, uint64_t src, const ResourceShape& dstShape, int dstSub, int srcSub) {
    std::lock_guard<std::mutex> lk(m_);
    auto s = res_.find(src);
    if (s == res_.end()) {
        if (dstSub < 0) res_.erase(dst);  // whole resource overwritten with unknown content
        return false;
    }
    Resident src_ = s->second;
    if (dstSub < 0) {  // CopyResource: identical shape required by D3D12
        if (dstShape.mipLevels != src_.shape.mipLevels || dstShape.width != src_.shape.width ||
            dstShape.height != src_.shape.height)
            return false;
        Resident r = src_;
        r.shape = dstShape;
        auto d = res_.find(dst);
        r.stateSeen = d != res_.end() && d->second.stateSeen;
        r.state = r.stateSeen ? d->second.state : 0;
        res_[dst] = std::move(r);
        return true;
    }
    // one subresource: dst must be (or become) the same texture with a consistent mip offset
    if (srcSub < 0 || uint32_t(srcSub) >= src_.content.size() || dstShape.arraySize != 1 ||
        uint32_t(dstSub) >= dstShape.mipLevels)
        return false;
    const int32_t ddsMip = srcSub + src_.mipOffset;
    const int32_t off = ddsMip - dstSub;
    if (off < 0) return false;
    auto d = res_.find(dst);
    if (d == res_.end() || d->second.texture != src_.texture || d->second.mipOffset != off) {
        Resident r;
        r.texture = src_.texture;
        r.mipOffset = off;
        r.shape = dstShape;
        r.content.assign(dstShape.mipLevels, 0);
        res_[dst] = std::move(r);
        d = res_.find(dst);
    }
    d->second.content[size_t(dstSub)] = src_.content[size_t(srcSub)];
    return true;
}

void ResidentTracker::onState(uint64_t res, uint32_t stateAfter) {
    std::lock_guard<std::mutex> lk(m_);
    auto it = res_.find(res);
    if (it == res_.end()) return;
    it->second.state = stateAfter;
    it->second.stateSeen = true;
}

bool ResidentTracker::onDestroyed(uint64_t res) {
    std::lock_guard<std::mutex> lk(m_);
    return res_.erase(res) != 0;
}

void ResidentTracker::onSwapped(uint64_t res, uint32_t sub, int slot) {
    std::lock_guard<std::mutex> lk(m_);
    auto it = res_.find(res);
    if (it == res_.end() || sub >= it->second.content.size()) return;
    it->second.content[sub] = slotBit(slot);
}

bool ResidentTracker::get(uint64_t res, Resident& out) const {
    std::lock_guard<std::mutex> lk(m_);
    auto it = res_.find(res);
    if (it == res_.end()) return false;
    out = it->second;
    return true;
}

bool ResidentTracker::tracked(uint64_t res) const {
    std::lock_guard<std::mutex> lk(m_);
    return res_.count(res) != 0;
}

bool ResidentTracker::withTracked(uint64_t res, const std::function<void(const Resident&)>& fn) const {
    std::lock_guard<std::mutex> lk(m_);
    auto it = res_.find(res);
    if (it == res_.end()) return false;
    fn(it->second);
    return true;
}

size_t ResidentTracker::size() const {
    std::lock_guard<std::mutex> lk(m_);
    return res_.size();
}

std::vector<uint64_t> ResidentTracker::resources() const {
    std::lock_guard<std::mutex> lk(m_);
    std::vector<uint64_t> v;
    v.reserve(res_.size());
    for (auto& kv : res_) v.push_back(kv.first);
    return v;
}

void ResidentTracker::planOne(uint64_t res, const Resident& r, const TargetFn& target, const BytesFn& bytes,
                              std::vector<SwapJob>& out, uint64_t notBefore) const {
    const int slot = target(r.texture);
    if (slot < 0) return;
    for (uint32_t s = 0; s < r.content.size(); ++s) {
        const uint64_t c = r.content[s];
        if (c == 0 || (c & slotBit(slot))) continue;  // never uploaded, or already the right stage
        SwapJob j;
        j.resource = res;
        j.texture = r.texture;
        j.sub = s;
        j.ddsMip = s + uint32_t(r.mipOffset);
        j.slot = slot;
        j.bytes = bytes(r.texture, j.ddsMip);
        j.notBefore = notBefore;
        if (j.bytes) out.push_back(j);
    }
}

void ResidentTracker::plan(const TargetFn& target, const BytesFn& bytes, std::vector<SwapJob>& out, uint64_t notBefore) const {
    std::vector<std::pair<uint64_t, Resident>> snap;
    {
        std::lock_guard<std::mutex> lk(m_);
        snap.assign(res_.begin(), res_.end());
    }
    // deterministic order (tests, logs): by texture id then resource
    std::sort(snap.begin(), snap.end(), [](const auto& a, const auto& b) {
        return a.second.texture != b.second.texture ? a.second.texture < b.second.texture : a.first < b.first;
    });
    for (auto& kv : snap) planOne(kv.first, kv.second, target, bytes, out, notBefore);
}

void ResidentTracker::planResource(uint64_t res, const TargetFn& target, const BytesFn& bytes, std::vector<SwapJob>& out,
                                   uint64_t notBefore) const {
    Resident r;
    if (get(res, r)) planOne(res, r, target, bytes, out, notBefore);
}

// ------------------------------------------------------------------ SwapScheduler
void SwapScheduler::push(const SwapJob& j) {
    std::lock_guard<std::mutex> lk(m_);
    const uint64_t k = key(j.resource, j.sub);
    auto range = byKey_.equal_range(k);
    for (auto it = range.first; it != range.second; ++it) {
        SwapJob& q = it->second->job;
        if (q.resource == j.resource && q.sub == j.sub) {
            bytes_ -= q.bytes;
            q = j;  // keep the queue position, newest content
            bytes_ += j.bytes;
            return;
        }
    }
    q_.push_back(Item{j, k});
    byKey_.emplace(k, std::prev(q_.end()));
    bytes_ += j.bytes;
}

void SwapScheduler::clear() {
    std::lock_guard<std::mutex> lk(m_);
    q_.clear();
    byKey_.clear();
    bytes_ = 0;
}

void SwapScheduler::dropResource(uint64_t res) {
    std::lock_guard<std::mutex> lk(m_);
    for (auto it = q_.begin(); it != q_.end();) {
        if (it->job.resource != res) {
            ++it;
            continue;
        }
        auto range = byKey_.equal_range(it->k);
        for (auto b = range.first; b != range.second; ++b)
            if (b->second == it) {
                byKey_.erase(b);
                break;
            }
        bytes_ -= it->job.bytes;
        it = q_.erase(it);
    }
}

std::vector<SwapJob> SwapScheduler::take(uint64_t budget, uint64_t now) {
    std::lock_guard<std::mutex> lk(m_);
    std::vector<SwapJob> out;
    uint64_t used = 0;
    for (auto it = q_.begin(); it != q_.end();) {
        if (it->job.notBefore > now) {
            ++it;
            continue;
        }
        if (!out.empty() && used + it->job.bytes > budget) break;
        used += it->job.bytes;
        out.push_back(it->job);
        auto range = byKey_.equal_range(it->k);
        for (auto b = range.first; b != range.second; ++b)
            if (b->second == it) {
                byKey_.erase(b);
                break;
            }
        bytes_ -= it->job.bytes;
        it = q_.erase(it);
        if (used >= budget) break;
    }
    return out;
}

size_t SwapScheduler::size() const {
    std::lock_guard<std::mutex> lk(m_);
    return q_.size();
}

uint64_t SwapScheduler::queuedBytes() const {
    std::lock_guard<std::mutex> lk(m_);
    return bytes_;
}

}  // namespace qs::gpu
