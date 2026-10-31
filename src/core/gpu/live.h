// QuattroStagioni - GPU live swap, part 4: settings, per-texture targets and safety checks (portable,
// unit tested; used by src/win/gpu/gpu_swap.cpp).
//
//   GpuSwapSettings      [GpuSwap] keys of QuattroStagioni.ini, every key optional (defaults below), so an
//                        old ini without the section keeps working.
//   planTargets          which stage slot every indexed texture must show, from the stage the file hooks
//                        serve for it right now (io per-texture snapshot, or one global stage).
//   checkDestination     the last check before our copy: the live resource description must match the
//                        tracked one exactly and the mip layout must match the data we are about to write.
//   FrameRate            per-frame telemetry (MB per frame over the summary interval).
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "swap_model.h"

namespace qs {
class Ini;
}

namespace qs::gpu {

struct GpuSwapSettings {
    bool enabled = true;          // test build: ON (the release default may become OFF, see docs/dev/gpu_swap.md)
    double maxMBPerFrame = 4.0;   // upload budget per presented frame (one batch per Present)
    int logLevel = 1;             // 0 errors only, 1 + summaries (throttled), 2 + one line per batch
    int ringMB = 48;              // upload ring, 3 segments: a single mip larger than a segment is skipped
    int cacheMB = 96;             // decoded texture cache
    bool assumeCommonState = false;  // texture whose post-upload state was never seen: false = skip it
    bool preloadAllPacks = false;    // index every *.qsp of the folder, not only the stages being served

    // Reads [GpuSwap]; out-of-range values are clamped and reported in `warnings`.
    void load(const Ini& ini, std::vector<std::string>& warnings);
    uint64_t budgetBytesPerFrame() const;
};

// Ini text of the section with the defaults (dist template, docs).
std::string gpuSwapIniTemplate();

// "<group>/<n>.paz@<offset>" (FingerprintIndex key) -> paz key "<group>/<n>.paz" and offset.
bool splitTextureKey(const std::string& key, std::string& pazKey, uint64_t& offset);

// Stage served right now for one texture: stage id, "" for vanilla. Return false when unknown (the
// texture is left as it is).
using ServedStageFn = std::function<bool(const std::string& pazKey, uint64_t offset, std::string& stageId)>;
// Slot of a stage id in the library, -1 if its pack is not indexed (yet).
using SlotOfFn = std::function<int(const std::string& stageId)>;
// Slot to show for a texture whose stage slot is `slot` (StageLibrary::targetFor).
using TargetForFn = std::function<int(uint32_t texture, int slot)>;

struct TargetPlan {
    std::vector<int> slot;                    // per texture id: target slot, -1 = leave as is
    std::vector<std::string> missingStages;   // stages served by io with no indexed pack (sorted, unique)
    size_t staged = 0, vanilla = 0, waiting = 0, unknown = 0;
};

// Targets from the per-texture stages served by the file hooks.
TargetPlan planTargets(const FingerprintIndex& index, const ServedStageFn& served, const SlotOfFn& slotOf,
                       const TargetForFn& targetFor);
// Targets for one global stage slot (demo, explicit gpuSwapSetStage()).
TargetPlan planGlobal(const FingerprintIndex& index, int slot, const TargetForFn& targetFor);

// Live description of a destination resource (ID3D12Resource::GetDesc).
struct LiveDesc {
    uint32_t dimension = 0;     // D3D12_RESOURCE_DIMENSION (TEXTURE2D = 3)
    uint32_t dxgiFormat = 0;
    uint64_t width = 0;
    uint32_t height = 0;
    uint16_t depthOrArraySize = 1, mipLevels = 1;
    uint32_t sampleCount = 1;
    uint32_t layout = 0;        // D3D12_TEXTURE_LAYOUT: only UNKNOWN (0, driver swizzle) like plain game textures
    uint32_t flags = 0;         // D3D12_RESOURCE_FLAGS: only NONE (no render target, UAV, simultaneous access)
};
constexpr uint32_t kDimTexture2D = 3;

// True if writing DDS mip `ddsMip` of `tex` (family `fam`, data layout `data`) into subresource `sub`
// of a resource with live description `live`, tracked as `r`, is safe. `why` gets the reason otherwise.
bool checkDestination(const LiveDesc& live, const Resident& r, const TextureDesc& tex, uint32_t sub, uint32_t ddsMip,
                      const MipLayout& data, std::string* why);

// Average bytes per frame between two summaries.
class FrameRate {
public:
    // Returns MB per frame since the previous call (0 if no frame was presented) and starts a new interval.
    double sample(uint64_t totalBytes, uint64_t totalFrames);

private:
    uint64_t bytes0_ = 0, frames0_ = 0;
};

}  // namespace qs::gpu
