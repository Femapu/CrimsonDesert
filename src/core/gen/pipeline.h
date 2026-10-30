// QuattroStagioni - generator pipeline: inventory item -> verified replacement stored bytes -> pack.
//
// Per texture (processTexture):
//   1. the item is re-checked against the live pamt (path, paz, offset, sizes); any difference skips it;
//   2. the stored entry is read and decoded strictly to a plain DDS (archive.h);
//   3. THE FITTER: the transform builds the modified texture at strength 1; every mip is encoded with
//      fitBias 0, 2, 4 ... 10 until its LZ4 block reaches the ORIGINAL stored size exactly (raw mips
//      always fit); if some mip cannot fit at the highest bias the whole texture is retried at a milder
//      strength (0.75, 0.5, 0.25); if nothing fits the texture stays vanilla (not in the pack);
//   4. the stored blob is assembled in the original layout, re-encrypted if needed and decoded again
//      strictly; only a byte-exact round trip is accepted.
// runGeneration processes items on N threads and commits results strictly in inventory order, so the
// pack is byte-identical for any thread count. At most 4 x N finished results wait in memory.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "archive.h"
#include "inventory.h"
#include "stage.h"
#include "transform.h"

namespace qs::gen {

struct FitPolicy {
    std::vector<int> biases{0, 2, 4, 6, 8, 10};
    std::vector<float> strengths{1.0f, 0.75f, 0.5f, 0.25f};
};

enum class Outcome {
    Unaffected,     // the stage does not change this texture (looks / recipe)
    Fit,            // fitted at strength 1 with fitBias 0
    FitBias,        // fitted at strength 1 with fitBias > 0 on some mip
    FitMild,        // fitted only at a reduced strength
    Identical,      // transform produced the original bytes (nothing to serve)
    Vanilla,        // could not fit at any bias/strength: keeps vanilla bytes
    IndexMismatch,  // inventory differs from the live pamt, or entry missing: skipped
    Error,          // read/decode/transform error: skipped
};
const char* toString(Outcome o);

struct TextureResult {
    Outcome outcome = Outcome::Error;
    int maxBias = 0;
    float strength = 1.0f;
    std::string message;              // reason for Vanilla / Error / IndexMismatch
    PamtEntry entry;
    std::vector<uint8_t> original;    // stored bytes (only kept for fitted results)
    std::vector<uint8_t> replacement; // same size as original
    uint32_t encodeCalls = 0;
};

TextureResult processTexture(GameArchive& archive, const Item& item, const StageSpec& stage,
                             const TextureTransform& transform, const FitPolicy& policy);

struct GenSummary {
    size_t total = 0, selected = 0;
    size_t affected = 0;        // selected items the stage changes (dry run: what would be processed)
    uint64_t affectedBytes = 0; // their stored bytes
    size_t unaffected = 0, fit = 0, fitBias = 0, fitMild = 0, identical = 0, vanilla = 0, indexMismatch = 0, errors = 0;
    uint64_t packedBytes = 0;   // payload bytes written
    size_t records = 0;
    double seconds = 0;
    size_t attempted() const { return fit + fitBias + fitMild + vanilla; }
    double fitRate() const { return attempted() ? double(fit + fitBias + fitMild) / double(attempted()) : 1.0; }
};

struct GenOptions {
    StageSpec stage;
    int threads = 1;
    std::string only;              // ECMAScript regex on the archive path (case-insensitive), empty = all
    bool dryRun = false;           // select and check only, no transform, no pack
    std::string outPath;           // pack file (ignored in dry runs)
    FitPolicy policy;
    std::string generator = "qs_gen";
    std::function<void(size_t done, size_t total, const GenSummary&)> progress;  // main thread
    std::function<void(const std::string&)> log;                                 // main thread
};

bool runGeneration(GameArchive& archive, const std::vector<Item>& items, const TextureTransform& transform,
                   const GenOptions& opt, GenSummary& summary, std::string* error = nullptr);

}  // namespace qs::gen
