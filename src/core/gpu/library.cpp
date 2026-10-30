// QuattroStagioni - GPU live swap: stage library (packs + original game data, prints, decoded mips).
#include "library.h"

#include "../lookup3.h"

namespace qs::gpu {

StageLibrary::StageLibrary(PazOpener opener, StoredPrintsFn stored, size_t cacheBytes)
    : opener_(std::move(opener)), stored_(std::move(stored)), cacheLimit_(cacheBytes) {
    packs_.emplace_back();  // slot 0: original data, no pack
}

pack::ByteSource* StageLibrary::paz(const std::string& group, uint32_t pazIndex) {
    std::lock_guard<std::mutex> lk(pazMutex_);
    const std::string k = pack::pazKey(group, pazIndex);
    auto it = paz_.find(k);
    if (it != paz_.end()) return it->second.get();
    std::unique_ptr<pack::ByteSource> s = opener_ ? opener_(group, pazIndex) : nullptr;
    pack::ByteSource* raw = s.get();
    paz_[k] = std::move(s);  // a failed open is remembered as null
    return raw;
}

int StageLibrary::addPack(std::unique_ptr<pack::ByteSource> src, std::string* error, PackLoadStats* stats) {
    PackLoadStats st;
    auto rd = std::make_unique<pack::PackReader>();
    if (!rd->open(std::move(src), error)) return -1;
    const std::string stage = rd->header().stageId;
    if (index_.slotOf(stage) == 0) {
        if (error) *error = "pack stage '" + stage + "' is the vanilla stage";
        return -1;
    }
    int existing = index_.slotOf(stage);
    {
        std::lock_guard<std::mutex> lk(m_);
        if (existing > 0 && size_t(existing) < packs_.size() && packs_[size_t(existing)]) return existing;
    }
    const int slot = index_.addStage(stage);
    if (slot <= 0) {
        if (error) *error = "too many stages";
        return -1;
    }
    const auto& recs = rd->records();
    st.records = uint32_t(recs.size());
    std::vector<uint8_t> payload, original;
    for (size_t i = 0; i < recs.size(); ++i) {
        const pack::Record& r = recs[i];
        const pack::PazInfo& pz = rd->paz()[r.pazSlot];
        const std::string key = pack::pazKey(pz.group, pz.pazIndex) + "@" + std::to_string(r.offset);
        TexturePrints orig, repl;
        bool haveOrig = false, haveRepl = false;
        if (stored_ && stored_(*rd, i, orig, repl)) {
            haveOrig = !orig.mips.empty();
            haveRepl = !repl.mips.empty();
            st.printsFromIndex = true;
        }
        if (!haveRepl) {
            std::string e;
            if (!rd->readPayload(r, payload, &e)) {
                ++st.payloadErrors;
                continue;
            }
            st.bytesRead += payload.size();
            if (!computePrintsStored(payload.data(), payload.size(), repl, &e)) {
                ++st.skippedFormat;
                continue;
            }
        }
        // the original is read only once per texture (first pack that references it)
        uint32_t known = kNoTexture;
        {
            TextureDesc td;
            uint32_t id = index_.addTexture(key, repl);
            if (id == kNoTexture) {
                ++st.skippedFormat;  // same location, different shape: stale pack
                continue;
            }
            known = id;
            index_.texture(id, td);
            haveOrig = haveOrig || (td.slots & slotBit(0));
        }
        if (!haveOrig) {
            pack::ByteSource* s = paz(pz.group, pz.pazIndex);
            original.resize(r.storedSize);
            std::string e;
            if (s && s->readAt(r.offset, original.data(), original.size()) && cdHash(original.data(), original.size()) == r.origHash &&
                computePrintsStored(original.data(), original.size(), orig, &e)) {
                haveOrig = true;
                st.bytesRead += original.size();
            } else {
                ++st.originalErrors;
            }
        }
        index_.addPrints(known, slot, repl.mips);
        if (haveOrig && !orig.mips.empty()) index_.addPrints(known, 0, orig.mips);
        {
            std::lock_guard<std::mutex> lk(m_);
            if (sources_.size() <= known) sources_.resize(known + 1);
            Source& so = sources_[known];
            if (so.recBySlot.empty()) {
                so.group = pz.group;
                so.pazIndex = pz.pazIndex;
                so.offset = r.offset;
                so.storedSize = r.storedSize;
                so.origHash = r.origHash;
                for (uint64_t o : repl.mipOffset) so.mipOffset.push_back(o + repl.headerSize);
            }
            if (so.recBySlot.size() <= size_t(slot)) so.recBySlot.resize(size_t(slot) + 1, -1);
            so.recBySlot[size_t(slot)] = int32_t(i);
        }
        ++st.indexed;
    }
    {
        std::lock_guard<std::mutex> lk(m_);
        if (packs_.size() <= size_t(slot)) packs_.resize(size_t(slot) + 1);
        packs_[size_t(slot)] = std::move(rd);
    }
    if (stats) *stats = st;
    return slot;
}

int StageLibrary::targetFor(uint32_t texture, int activeSlot) const {
    if (activeSlot <= 0) return 0;
    std::lock_guard<std::mutex> lk(m_);
    if (texture >= sources_.size()) return 0;
    const Source& s = sources_[texture];
    return size_t(activeSlot) < s.recBySlot.size() && s.recBySlot[size_t(activeSlot)] >= 0 ? activeSlot : 0;
}

uint64_t StageLibrary::mipBytes(uint32_t texture, uint32_t ddsMip) const {
    TextureDesc td;
    if (!index_.texture(texture, td) || ddsMip >= td.mipCount) return 0;
    return mipLayout(td.family, td.width, td.height, ddsMip).size();
}

bool StageLibrary::decoded(uint32_t texture, int slot, std::shared_ptr<const std::vector<uint8_t>>& out, std::string* error) {
    const uint64_t key = (uint64_t(texture) << 8) | uint64_t(slot);
    Source so;
    const pack::PackReader* rd = nullptr;
    {
        std::lock_guard<std::mutex> lk(m_);
        auto it = cache_.find(key);
        if (it != cache_.end()) {
            lru_.splice(lru_.begin(), lru_, it->second.it);
            out = it->second.data;
            return true;
        }
        if (texture >= sources_.size() || sources_[texture].recBySlot.empty()) {
            if (error) *error = "unknown texture";
            return false;
        }
        so = sources_[texture];
        if (slot > 0) {
            if (size_t(slot) >= so.recBySlot.size() || so.recBySlot[size_t(slot)] < 0 || size_t(slot) >= packs_.size() ||
                !packs_[size_t(slot)]) {
                if (error) *error = "stage has no data for this texture";
                return false;
            }
            rd = packs_[size_t(slot)].get();
        }
    }
    std::vector<uint8_t> stored;
    if (slot == 0) {
        pack::ByteSource* s = paz(so.group, so.pazIndex);
        stored.resize(so.storedSize);
        if (!s || !s->readAt(so.offset, stored.data(), stored.size())) {
            if (error) *error = "original not readable from " + pack::pazKey(so.group, so.pazIndex);
            return false;
        }
        if (cdHash(stored.data(), stored.size()) != so.origHash) {
            if (error) *error = "original bytes changed (game patched?)";
            return false;
        }
    } else if (!rd->readPayload(rd->records()[size_t(so.recBySlot[size_t(slot)])], stored, error)) {
        return false;
    }
    auto dds = std::make_shared<std::vector<uint8_t>>();
    if (!decodeStored(stored.data(), stored.size(), *dds, error)) return false;
    out = dds;
    std::lock_guard<std::mutex> lk(m_);
    if (cache_.count(key)) return true;
    lru_.push_front(key);
    cache_[key] = Cached{out, lru_.begin()};
    cacheUsed_ += out->size();
    while (cacheUsed_ > cacheLimit_ && lru_.size() > 1) {
        auto last = cache_.find(lru_.back());
        cacheUsed_ -= last->second.data->size();
        cache_.erase(last);
        lru_.pop_back();
    }
    return true;
}

bool StageLibrary::mipData(uint32_t texture, int slot, uint32_t ddsMip, MipData& out, std::string* error) {
    TextureDesc td;
    if (!index_.texture(texture, td) || ddsMip >= td.mipCount) {
        if (error) *error = "bad texture or mip";
        return false;
    }
    if (!decoded(texture, slot, out.dds, error)) return false;
    uint64_t off = 0;
    {
        std::lock_guard<std::mutex> lk(m_);
        off = sources_[texture].mipOffset[ddsMip];
    }
    out.family = td.family;
    out.layout = mipLayout(td.family, td.width, td.height, ddsMip);
    out.offset = off;
    out.size = out.layout.size();
    if (off + out.size > out.dds->size()) {
        if (error) *error = "mip outside decoded data";
        return false;
    }
    return true;
}

size_t StageLibrary::cacheBytes() const {
    std::lock_guard<std::mutex> lk(m_);
    return cacheUsed_;
}

}  // namespace qs::gpu
