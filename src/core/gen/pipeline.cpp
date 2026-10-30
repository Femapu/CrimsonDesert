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
    // A game hotfix can move an entry inside its paz without changing it: same paz, stored size, size
    // and flags. The live pamt is the source of truth, so take its offset (noted in the log).
    if (!d.empty() && item.paz == r.entry.pazIndex && item.stored == r.entry.storedSize && item.size == r.entry.size &&
        item.offset != r.entry.offset) {
        r.message = "offset " + std::to_string(item.offset) + " -> " + std::to_string(r.entry.offset) +
                    " taken from the live pamt (inventory is stale, entry unchanged)";
        d.clear();
    }
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

// Stored and decoded bytes of one item, shared by every stage of a multi-stage run.
struct Prepared {
    const Group* g = nullptr;
    std::vector<uint8_t> stored, plain, dec;  // dec = stored after decryption
    EntryLayout L;
};

// Reads and decodes the item (after checkIndex). Returns false and fills `r` (Error) on failure.
bool readDecode(GameArchive& archive, const Item& item, Prepared& p, TextureResult& r) {
    std::string err;
    if (!archive.readStored(item.group, r.entry, p.stored, &err)) {
        r.outcome = Outcome::Error;
        r.message = err;
        return false;
    }
    if (!decodeStored(r.entry, p.g->keyMaterial, p.stored.data(), p.stored.size(), p.plain, &p.L, &err)) {
        r.outcome = Outcome::Error;
        r.message = "decode: " + err;
        return false;
    }
    p.dec = p.stored;
    if (p.L.crypto == 3) crypto::cryptEntry(p.L.path, p.dec.data(), p.dec.size(), p.L.keyMaterial);
    return true;
}

// The fitter for one stage on a prepared item. `r` arrives with entry set.
void fitPrepared(const Item& item, const EffectiveStage& eff, const TextureTransform& transform, const FitPolicy& policy,
                 const Prepared& p, TextureResult& r) {
    std::string why;
    for (size_t si = 0; si < policy.strengths.size(); ++si) {
        const float s = policy.strengths[si];
        std::vector<uint8_t> out;
        int maxBias = 0;
        bool hard = false;
        if (tryStrength(item, eff, s, transform, policy, p.L, p.plain, p.dec, out, maxBias, why, hard, r.encodeCalls)) {
            r.maxBias = maxBias;
            r.strength = s;
            if (out == p.stored) {
                r.outcome = Outcome::Identical;
                return;
            }
            r.outcome = si > 0 ? Outcome::FitMild : (maxBias > 0 ? Outcome::FitBias : Outcome::Fit);
            r.original = p.stored;
            r.replacement = std::move(out);
            return;
        }
        if (hard) {
            r.outcome = Outcome::Error;
            r.message = why;
            return;
        }
    }
    r.outcome = Outcome::Vanilla;
    r.message = why;
}

// Every stage of one item: index check and decode once, fit per affected stage.
std::vector<TextureResult> processStages(GameArchive& archive, const Item& item, const std::vector<StageSpec>& stages,
                                         const TextureTransform& transform, const FitPolicy& policy) {
    std::vector<TextureResult> rs(stages.size());
    TextureResult base;
    Prepared p;
    if (!checkIndex(archive, item, p.g, base)) {
        for (TextureResult& r : rs) r = base;
        return rs;
    }
    std::vector<EffectiveStage> eff(stages.size());
    bool any = false;
    for (size_t i = 0; i < stages.size(); ++i) {
        eff[i] = effectiveStage(item, stages[i]);
        rs[i].entry = base.entry;
        rs[i].message = base.message;
        if (eff[i].vanilla() || !transform.affects(item, eff[i])) rs[i].outcome = Outcome::Unaffected;
        else any = true;
    }
    if (!any) return rs;
    if (!readDecode(archive, item, p, base)) {
        for (TextureResult& r : rs)
            if (r.outcome != Outcome::Unaffected) r = base;
        return rs;
    }
    for (size_t i = 0; i < stages.size(); ++i)
        if (rs[i].outcome != Outcome::Unaffected) fitPrepared(item, eff[i], transform, policy, p, rs[i]);
    return rs;
}

GenSummary addSummaries(const std::vector<GenSummary>& sums) {
    GenSummary t;
    for (const GenSummary& s : sums) {
        t.total = s.total;
        t.selected = s.selected;
        t.affected += s.affected;
        t.affectedBytes += s.affectedBytes;
        t.unaffected += s.unaffected;
        t.fit += s.fit;
        t.fitBias += s.fitBias;
        t.fitMild += s.fitMild;
        t.identical += s.identical;
        t.vanilla += s.vanilla;
        t.indexMismatch += s.indexMismatch;
        t.errors += s.errors;
        t.records += s.records;
        t.packedBytes += s.packedBytes;
    }
    return t;
}

}  // namespace

TextureResult processTexture(GameArchive& archive, const Item& item, const StageSpec& stage,
                             const TextureTransform& transform, const FitPolicy& policy) {
    return processStages(archive, item, std::vector<StageSpec>{stage}, transform, policy)[0];
}

bool runGeneration(GameArchive& archive, const std::vector<Item>& items, const TextureTransform& transform,
                   const GenOptions& opt, GenSummary& sum, std::string* error) {
    std::vector<GenSummary> sums;
    const bool ok = runGenerationMulti(archive, items, transform, std::vector<StageSpec>{opt.stage},
                                       std::vector<std::string>{opt.outPath}, opt, sums, error);
    sum = sums.empty() ? GenSummary() : sums[0];
    return ok;
}

bool runGenerationMulti(GameArchive& archive, const std::vector<Item>& items, const TextureTransform& transform,
                        const std::vector<StageSpec>& stages, const std::vector<std::string>& outPaths,
                        const GenOptions& opt, std::vector<GenSummary>& sums, std::string* error) {
    const auto t0 = std::chrono::steady_clock::now();
    const size_t ns = stages.size();
    sums.assign(ns, GenSummary());
    for (GenSummary& s : sums) s.total = items.size();
    auto log = [&](const std::string& s) { if (opt.log) opt.log(s); };
    auto fail = [&](const std::string& m) { if (error) *error = m; return false; };
    if (ns == 0) return fail("no stage to generate");
    if (!opt.dryRun && outPaths.size() != ns) return fail("one output path per stage is needed");
    auto tag = [&](size_t si) { return ns > 1 ? "[" + stages[si].id() + "] " : std::string(); };

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
    std::vector<std::set<std::string>> groups(ns);
    for (size_t si = 0; si < ns; ++si) {
        sums[si].selected = sel.size();
        for (const Item* it : sel) {
            const EffectiveStage eff = effectiveStage(*it, stages[si]);
            if (!eff.vanilla() && transform.affects(*it, eff)) {
                sums[si].affected++;
                sums[si].affectedBytes += it->stored;
                groups[si].insert(it->group);
            }
        }
    }
    if (opt.dryRun) {
        size_t mismatches = 0;
        for (const Item* it : sel) {
            TextureResult r;
            const Group* g = nullptr;
            if (!checkIndex(archive, *it, g, r)) {
                mismatches++;
                log(std::string(toString(r.outcome)) + " " + it->group + "/" + it->path + ": " + r.message);
            }
        }
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        for (GenSummary& s : sums) { s.indexMismatch = mismatches; s.seconds = secs; }
        return true;
    }

    // ---- pack headers: game build and pamt hashes of every group involved --------------------------
    uint64_t exeSize = 0;
    uint32_t exeTs = 0;
    if (!archive.exeBuild(exeSize, exeTs)) return fail("cannot read bin64/CrimsonDesert.exe (game build binding)");
    std::vector<std::unique_ptr<pack::PackWriter>> writers(ns);
    std::vector<std::set<std::string>> pazDone(ns);
    std::string err;
    for (size_t si = 0; si < ns; ++si) {
        pack::Header hdr;
        hdr.stageId = stages[si].id();
        hdr.generator = opt.generator + " / " + transform.name();
        hdr.recipeVersion = transform.version();
        hdr.transitionSteps = uint32_t(stages[si].steps);
        hdr.gameBuild = pack::gameBuildId(exeSize, exeTs);
        for (const std::string& gname : groups[si]) {
            const Group* g = archive.group(gname, &err);
            if (!g) { log("group " + gname + ": " + err); continue; }
            if (g->pamt.storedSelfHash() != g->pamt.computedSelfHash())
                return fail(gname + "/0.pamt self hash does not verify: archive damaged or modified by another tool");
            hdr.pamtHashes.emplace_back(gname, g->pamt.storedSelfHash());
        }
        writers[si].reset(new pack::PackWriter());
        if (!writers[si]->open(outPaths[si], hdr, &err)) {
            for (auto& w : writers)
                if (w) w->abort();
            return fail(err);
        }
    }

    // ---- parallel processing, in-order commit ------------------------------------------------------
    const size_t n = sel.size();
    const size_t threads = size_t(opt.threads < 1 ? 1 : (opt.threads > 64 ? 64 : opt.threads));
    const size_t window = threads * 4;
    std::vector<std::unique_ptr<std::vector<TextureResult>>> slots(n);
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
            std::unique_ptr<std::vector<TextureResult>> r(new std::vector<TextureResult>());
            try {
                *r = processStages(archive, *sel[i], stages, transform, opt.policy);
            } catch (const std::exception& e) {
                r->assign(ns, TextureResult());
                for (TextureResult& x : *r) {
                    x.outcome = Outcome::Error;
                    x.message = std::string("exception: ") + e.what();
                }
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
        std::unique_ptr<std::vector<TextureResult>> rv;
        {
            std::unique_lock<std::mutex> lock(m);
            cv.wait(lock, [&] { return slots[i] != nullptr; });
            rv = std::move(slots[i]);
        }
        const Item& it = *sel[i];
        for (size_t si = 0; si < ns && ok; ++si) {
            TextureResult& r = (*rv)[si];
            GenSummary& sum = sums[si];
            switch (r.outcome) {
                case Outcome::Unaffected: sum.unaffected++; break;
                case Outcome::Identical: sum.identical++; break;
                case Outcome::Vanilla: sum.vanilla++; break;
                case Outcome::IndexMismatch: sum.indexMismatch++; break;
                case Outcome::Error: sum.errors++; break;
                case Outcome::Fit: sum.fit++; break;
                case Outcome::FitBias: sum.fitBias++; break;
                case Outcome::FitMild: sum.fitMild++; break;
            }
            if (r.outcome == Outcome::Vanilla || r.outcome == Outcome::Error || r.outcome == Outcome::IndexMismatch)
                log(tag(si) + std::string(toString(r.outcome)) + " " + it.group + "/" + it.path + ": " + r.message);
            else if (si == 0 && !r.message.empty())
                log("NOTE " + it.group + "/" + it.path + ": " + r.message);
            if (r.replacement.empty()) continue;
            const std::string key = pack::pazKey(it.group, r.entry.pazIndex);
            if (!pazDone[si].count(key)) {
                uint64_t ps = 0;
                if (!archive.pazFileSize(it.group, r.entry.pazIndex, ps)) {
                    ok = false;
                    err = "cannot size " + key;
                }
                writers[si]->setPazSize(it.group, r.entry.pazIndex, ps);
                pazDone[si].insert(key);
            }
            pack::Record rec;
            rec.group = it.group;
            rec.path = it.path;
            rec.pazIndex = r.entry.pazIndex;
            rec.offset = r.entry.offset;
            rec.storedSize = r.entry.storedSize;
            rec.size = r.entry.size;
            rec.flags = r.entry.flags;
            rec.region = uint8_t(regionOf(it));
            rec.assetSeed = it.seed;
            if (ok && !writers[si]->add(rec, r.original.data(), r.replacement.data(), &err)) ok = false;
        }
        {
            std::lock_guard<std::mutex> lock(m);
            committed = i + 1;
            if (!ok) stop = true;
        }
        cv.notify_all();
        if (opt.progress) opt.progress(i + 1, n, ns == 1 ? sums[0] : addSummaries(sums));
        if (!ok) break;
    }
    {
        std::lock_guard<std::mutex> lock(m);
        stop = true;
    }
    cv.notify_all();
    for (std::thread& t : pool) t.join();
    if (!ok) {
        for (auto& w : writers) w->abort();
        return fail(err);
    }
    for (size_t si = 0; si < ns; ++si) {
        sums[si].records = writers[si]->recordCount();
        sums[si].packedBytes = writers[si]->payloadBytes();
        if (!writers[si]->finish(&err)) {
            for (auto& w : writers) w->abort();
            return fail(stages[si].id() + ": " + err);
        }
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    for (GenSummary& s : sums) s.seconds = secs;
    return true;
}

}  // namespace qs::gen
