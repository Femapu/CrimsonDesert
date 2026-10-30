// QuattroStagioni - generator pipeline and fitter. See pipeline.h.
#include "pipeline.h"

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <regex>
#include <set>
#include <thread>

#include "../crypto/chacha20.h"
#include "../pack/qsp_io.h"

namespace qs::gen {

const char* toString(Outcome o) {
    switch (o) {
        case Outcome::Unaffected: return "unaffected";
        case Outcome::Fit: return "fit";
        case Outcome::FitBias: return "fit(bias)";
        case Outcome::FitMild: return "fit(mild)";
        case Outcome::Identical: return "identical";
        case Outcome::Vanilla: return "VANILLA";
        case Outcome::IndexMismatch: return "INDEX-MISMATCH";
        case Outcome::Error: return "ERROR";
    }
    return "?";
}

namespace {

// Checks the item against the live index. Returns false and fills `r` when it must be skipped.
bool checkIndex(GameArchive& archive, const Item& item, const Group*& g, TextureResult& r) {
    std::string err;
    g = archive.group(item.group, &err);
    if (!g) {
        r.outcome = Outcome::IndexMismatch;
        r.message = err;
        return false;
    }
    auto it = g->byPath.find(item.path);
    if (it == g->byPath.end()) {
        r.outcome = Outcome::IndexMismatch;
        r.message = "not in " + item.group + "/0.pamt";
        return false;
    }
    r.entry = it->second;
    std::string d = compareWithIndex(item, r.entry);
    if (!d.empty()) {
        r.outcome = Outcome::IndexMismatch;
        r.message = "inventory differs from pamt: " + d;
        return false;
    }
    return true;
}

// One attempt at a given strength. Returns true when every mip fitted; `out` then holds the stored blob.
bool tryStrength(const Item& item, const EffectiveStage& eff, float strength, const TextureTransform& tr,
                 const FitPolicy& policy, const EntryLayout& L, const std::vector<uint8_t>& plain,
                 const std::vector<uint8_t>& decStored, std::vector<uint8_t>& out, int& maxBias, std::string& why,
                 bool& hardError, uint32_t& encodeCalls) {
    hardError = false;
    maxBias = 0;
    std::string err;
    std::unique_ptr<TextureWork> work = tr.begin(item, eff, strength, plain.data(), plain.size(), &err);
    if (!work) {
        why = "transform: " + err;
        hardError = true;
        return false;
    }
    if (L.compression == 1) {
        if (work->mipCount() != L.mipStored.size()) {
            why = "transform returned " + std::to_string(work->mipCount()) + " mips, texture has " +
                  std::to_string(L.mipStored.size());
            hardError = true;
            return false;
        }
        std::vector<uint8_t> newPlain = L.header;
        std::vector<std::vector<uint8_t>> chunks(L.mipStored.size());
        std::vector<uint8_t> raw;
        for (size_t m = 0; m < L.mipStored.size(); ++m) {
            const size_t rawSize = L.dds.mipRawSize[m];
            const uint8_t* origMip = plain.data() + L.mipPlainOffset[m];
            bool fitted = false;
            size_t best = 0;
            for (size_t bi = 0; bi < policy.biases.size() && !fitted; ++bi) {
                const int bias = policy.biases[bi];
                ++encodeCalls;
                if (!work->encodeMip(m, bias, raw, &err)) {
                    why = "encodeMip " + std::to_string(m) + ": " + err;
                    hardError = true;
                    return false;
                }
                if (raw.size() != rawSize) {
                    why = "encodeMip " + std::to_string(m) + " returned " + std::to_string(raw.size()) + " bytes";
                    hardError = true;
                    return false;
                }
                if (std::memcmp(raw.data(), origMip, rawSize) == 0) {  // unchanged mip: reuse the original chunk
                    chunks[m].assign(decStored.begin() + long(L.mipStoredOffset[m]),
                                     decStored.begin() + long(L.mipStoredOffset[m] + L.mipStored[m]));
                    fitted = true;
                } else {
                    MipEncode me = encodeMipStored(L, m, raw.data(), raw.size());
                    if (me.ok) {
                        chunks[m] = std::move(me.chunk);
                        fitted = true;
                    } else if (best == 0 || (me.bestSize && me.bestSize < best)) {
                        best = me.bestSize;
                    }
                }
                if (fitted && bias > maxBias) maxBias = bias;
                if (!L.mipIsLz4(m) && !fitted) break;  // raw mips cannot fail
            }
            if (!fitted) {
                why = "mip " + std::to_string(m) + " compresses to " + std::to_string(best) + " > " +
                      std::to_string(L.mipStored[m]) + " bytes";
                return false;
            }
            newPlain.insert(newPlain.end(), raw.begin(), raw.end());
        }
        if (!assembleStored(L, chunks, newPlain, out, &err)) {
            why = "assemble: " + err;
            hardError = true;
            return false;
        }
        return true;
    }
    // Compression 0 / 2: whole-entry encode, one bias for all mips.
    DdsInfo info;
    if (!parseDdsHeader(plain.data(), plain.size(), info, &err)) {
        why = "not a DDS entry: " + err;
        hardError = true;
        return false;
    }
    for (int bias : policy.biases) {
        std::vector<uint8_t> newPlain(plain.begin(), plain.begin() + info.headerSize), raw;
        for (size_t m = 0; m < work->mipCount(); ++m) {
            ++encodeCalls;
            if (!work->encodeMip(m, bias, raw, &err)) {
                why = "encodeMip: " + err;
                hardError = true;
                return false;
            }
            newPlain.insert(newPlain.end(), raw.begin(), raw.end());
        }
        if (newPlain.size() != plain.size()) {
            why = "transform changed the texture size";
            hardError = true;
            return false;
        }
        int failedMip = -1;
        if (encodeStoredLike(L, newPlain, out, &err, &failedMip)) {
            maxBias = bias;
            return true;
        }
        if (failedMip < 0) {
            why = err;
            hardError = true;
            return false;
        }
        why = err;
    }
    return false;
}

}  // namespace

TextureResult processTexture(GameArchive& archive, const Item& item, const StageSpec& stage,
                             const TextureTransform& transform, const FitPolicy& policy) {
    TextureResult r;
    const Group* g = nullptr;
    if (!checkIndex(archive, item, g, r)) return r;
    const EffectiveStage eff = effectiveStage(item, stage);
    if (eff.vanilla() || !transform.affects(item, eff)) {
        r.outcome = Outcome::Unaffected;
        return r;
    }
    std::string err;
    std::vector<uint8_t> stored, plain;
    if (!archive.readStored(item.group, r.entry, stored, &err)) {
        r.outcome = Outcome::Error;
        r.message = err;
        return r;
    }
    EntryLayout L;
    if (!decodeStored(r.entry, g->keyMaterial, stored.data(), stored.size(), plain, &L, &err)) {
        r.outcome = Outcome::Error;
        r.message = "decode: " + err;
        return r;
    }
    std::vector<uint8_t> dec = stored;
    if (L.crypto == 3) crypto::cryptEntry(L.path, dec.data(), dec.size(), L.keyMaterial);

    std::string why;
    for (size_t si = 0; si < policy.strengths.size(); ++si) {
        const float s = policy.strengths[si];
        std::vector<uint8_t> out;
        int maxBias = 0;
        bool hard = false;
        if (tryStrength(item, eff, s, transform, policy, L, plain, dec, out, maxBias, why, hard, r.encodeCalls)) {
            r.maxBias = maxBias;
            r.strength = s;
            if (out == stored) {
                r.outcome = Outcome::Identical;
                return r;
            }
            r.outcome = si > 0 ? Outcome::FitMild : (maxBias > 0 ? Outcome::FitBias : Outcome::Fit);
            r.original = std::move(stored);
            r.replacement = std::move(out);
            return r;
        }
        if (hard) {
            r.outcome = Outcome::Error;
            r.message = why;
            return r;
        }
    }
    r.outcome = Outcome::Vanilla;
    r.message = why;
    return r;
}

bool runGeneration(GameArchive& archive, const std::vector<Item>& items, const TextureTransform& transform,
                   const GenOptions& opt, GenSummary& sum, std::string* error) {
    const auto t0 = std::chrono::steady_clock::now();
    sum = GenSummary();
    sum.total = items.size();
    auto log = [&](const std::string& s) { if (opt.log) opt.log(s); };
    auto fail = [&](const std::string& m) { if (error) *error = m; return false; };

    // ---- selection (regex, index check, affects) -------------------------------------------------
    std::vector<const Item*> sel;
    std::unique_ptr<std::regex> re;
    if (!opt.only.empty()) {
        try {
            re.reset(new std::regex(opt.only, std::regex::ECMAScript | std::regex::icase));
        } catch (const std::exception& e) {
            return fail("bad --only regex: " + std::string(e.what()));
        }
    }
    for (const Item& it : items)
        if (!re || std::regex_search(it.path, *re)) sel.push_back(&it);
    sum.selected = sel.size();

    std::set<std::string> groups;
    for (const Item* it : sel) {
        const EffectiveStage eff = effectiveStage(*it, opt.stage);
        if (!eff.vanilla() && transform.affects(*it, eff)) {
            sum.affected++;
            sum.affectedBytes += it->stored;
            groups.insert(it->group);
        }
    }
    if (opt.dryRun) {
        for (const Item* it : sel) {
            TextureResult r;
            const Group* g = nullptr;
            if (!checkIndex(archive, *it, g, r)) {
                sum.indexMismatch++;
                log(std::string(toString(r.outcome)) + " " + it->group + "/" + it->path + ": " + r.message);
            }
        }
        sum.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        return true;
    }

    // ---- pack header: game build and pamt hashes of every group involved ---------------------------
    pack::Header hdr;
    hdr.stageId = opt.stage.id();
    hdr.generator = opt.generator + " / " + transform.name();
    hdr.recipeVersion = transform.version();
    uint64_t exeSize = 0;
    uint32_t exeTs = 0;
    if (!archive.exeBuild(exeSize, exeTs)) return fail("cannot read bin64/CrimsonDesert.exe (game build binding)");
    hdr.gameBuild = pack::gameBuildId(exeSize, exeTs);
    for (const std::string& gname : groups) {
        std::string err;
        const Group* g = archive.group(gname, &err);
        if (!g) { log("group " + gname + ": " + err); continue; }
        if (g->pamt.storedSelfHash() != g->pamt.computedSelfHash())
            return fail(gname + "/0.pamt self hash does not verify: archive damaged or modified by another tool");
        hdr.pamtHashes.emplace_back(gname, g->pamt.storedSelfHash());
    }
    pack::PackWriter writer;
    std::string err;
    if (!writer.open(opt.outPath, hdr, &err)) return fail(err);
    std::set<std::string> pazDone;

    // ---- parallel processing, in-order commit ------------------------------------------------------
    const size_t n = sel.size();
    const size_t threads = size_t(opt.threads < 1 ? 1 : (opt.threads > 64 ? 64 : opt.threads));
    const size_t window = threads * 4;
    std::vector<std::unique_ptr<TextureResult>> slots(n);
    std::mutex m;
    std::condition_variable cv;
    size_t next = 0, committed = 0;
    bool stop = false;

    auto worker = [&]() {
        for (;;) {
            size_t i;
            {
                std::unique_lock<std::mutex> lock(m);
                cv.wait(lock, [&] { return stop || next >= n || next < committed + window; });
                if (stop || next >= n) return;
                i = next++;
            }
            std::unique_ptr<TextureResult> r(new TextureResult());
            try {
                *r = processTexture(archive, *sel[i], opt.stage, transform, opt.policy);
            } catch (const std::exception& e) {
                r->outcome = Outcome::Error;
                r->message = std::string("exception: ") + e.what();
            }
            {
                std::lock_guard<std::mutex> lock(m);
                slots[i] = std::move(r);
            }
            cv.notify_all();
        }
    };
    std::vector<std::thread> pool;
    for (size_t t = 0; t < threads; ++t) pool.emplace_back(worker);

    bool ok = true;
    for (size_t i = 0; i < n; ++i) {
        std::unique_ptr<TextureResult> r;
        {
            std::unique_lock<std::mutex> lock(m);
            cv.wait(lock, [&] { return slots[i] != nullptr; });
            r = std::move(slots[i]);
        }
        const Item& it = *sel[i];
        switch (r->outcome) {
            case Outcome::Unaffected: sum.unaffected++; break;
            case Outcome::Identical: sum.identical++; break;
            case Outcome::Vanilla: sum.vanilla++; break;
            case Outcome::IndexMismatch: sum.indexMismatch++; break;
            case Outcome::Error: sum.errors++; break;
            case Outcome::Fit: sum.fit++; break;
            case Outcome::FitBias: sum.fitBias++; break;
            case Outcome::FitMild: sum.fitMild++; break;
        }
        if (r->outcome == Outcome::Vanilla || r->outcome == Outcome::Error || r->outcome == Outcome::IndexMismatch)
            log(std::string(toString(r->outcome)) + " " + it.group + "/" + it.path + ": " + r->message);
        if (ok && !r->replacement.empty()) {
            const std::string key = pack::pazKey(it.group, r->entry.pazIndex);
            if (!pazDone.count(key)) {
                uint64_t ps = 0;
                if (!archive.pazFileSize(it.group, r->entry.pazIndex, ps)) {
                    ok = false;
                    err = "cannot size " + key;
                }
                writer.setPazSize(it.group, r->entry.pazIndex, ps);
                pazDone.insert(key);
            }
            pack::Record rec;
            rec.group = it.group;
            rec.path = it.path;
            rec.pazIndex = r->entry.pazIndex;
            rec.offset = r->entry.offset;
            rec.storedSize = r->entry.storedSize;
            rec.size = r->entry.size;
            rec.flags = r->entry.flags;
            if (ok && !writer.add(rec, r->original.data(), r->replacement.data(), &err)) ok = false;
        }
        {
            std::lock_guard<std::mutex> lock(m);
            committed = i + 1;
            if (!ok) stop = true;
        }
        cv.notify_all();
        if (opt.progress) opt.progress(i + 1, n, sum);
        if (!ok) break;
    }
    {
        std::lock_guard<std::mutex> lock(m);
        stop = true;
    }
    cv.notify_all();
    for (std::thread& t : pool) t.join();
    if (!ok) {
        writer.abort();
        return fail(err);
    }
    sum.records = writer.recordCount();
    sum.packedBytes = writer.payloadBytes();
    if (!writer.finish(&err)) return fail(err);
    sum.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return true;
}

}  // namespace qs::gen
