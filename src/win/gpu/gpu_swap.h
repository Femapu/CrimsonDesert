// QuattroStagioni - GPU live swap of resident textures when the season stage changes.
//
// The file hooks (io_hooks) change textures only when the game reads them; this module also rewrites
// the textures already in VRAM. It hooks only D3D12 runtime code (d3d12.dll!D3D12CreateDevice and the
// ID3D12GraphicsCommandList vtable: CopyTextureRegion, CopyResource, ResourceBarrier), never the game
// exe. Design, assumptions and risks: docs/dev/gpu_swap.md; portable logic: src/core/gpu/.
//
// Wiring (src/win/main.cpp):
//   - gpuSwapStart(cfg) once, AFTER qs::io::packsStart() and qs::overlayStart(), only with [GpuSwap]
//     Enabled=1 and packs on. It returns immediately; a worker thread installs the hooks and indexes the
//     packs. Uploads are recorded by the worker and submitted from the overlay's Present hook
//     (overlaySetPresentCallback) on the game's DIRECT queue: one batch per presented frame, at most
//     [GpuSwap] MaxMBPerFrame (a single mip larger than that goes alone).
//   - stage changes: by default (cfg.followServed) the worker follows io::serveSnapshot(), i.e. exactly
//     the per-texture stages the file hooks serve (calendar tick and F5 override alike), so
//     season_tick.cpp needs no change. Without io packs (demo) call gpuSwapSetStage().
//   - any D3D12 error, device removal, failed check of the game's queue or exception: the module logs
//     "gpu: DISABLED: <reason>" and stays off for the session (textures then change after a reload, as
//     without it). gpuSwapStop() on FreeLibrary only, BEFORE qs::overlayStop() and qs::io::uninstall().
#pragma once
#include <cstdint>
#include <string>

namespace qs {

struct GpuSwapConfig {
    std::wstring gameRoot;            // folder with 0000/, 0001/... (original data for the vanilla stage)
    std::wstring packsDir;            // absolute folder of the .qsp packs
    bool followServed = true;         // follow io::serveSnapshot() (false: only gpuSwapSetStage())
    std::string initialStage;         // used when followServed is false ("" = vanilla)
    uint64_t budgetBytesPerFrame = 4ull << 20;  // [GpuSwap] MaxMBPerFrame
    int logLevel = 1;                 // [GpuSwap] LogLevel: 0 errors, 1 summaries, 2 one line per batch
    int ringMB = 48;                  // upload ring (3 segments); a mip larger than a segment is skipped
    int cacheMB = 96;                 // decoded texture cache
    int reswapDelayFrames = 4;        // wait after a stale game upload before fixing it (queue order)
    bool assumeCommonState = false;   // post-upload state never seen: false = skip the texture
    bool preloadAllPacks = false;     // index every pack of the folder, not only the stages being served
    bool verifyPacks = true;          // use only packs that pass io's game build check (loadVerifiedPack)
};

// Starts the worker (idempotent). False only if the thread cannot be created.
bool gpuSwapStart(const GpuSwapConfig& cfg);
void gpuSwapStop();
// Demo / followServed = false: requests one global stage ("" / "U" / "none" = vanilla, otherwise a pack
// stage id such as "A" or "A>W.2"). Non-blocking: the worker loads the pack and schedules the swaps.
void gpuSwapSetStage(const std::string& stageId);

struct GpuSwapStats {
    bool started = false, hooksInstalled = false, queueReady = false, disabled = false;
    std::string disabledReason;       // "" while enabled
    std::string activeStage;          // demo: global stage; follow mode: "per texture"
    uint64_t copiesSeen = 0;          // CopyTextureRegion buffer -> texture
    uint64_t copiesCandidate = 0;     // shape known to the index
    uint64_t copiesGpuSource = 0;     // source buffer not CPU visible (cannot fingerprint)
    uint64_t printsMatched = 0, printsUnknown = 0, printsAmbiguous = 0;
    uint64_t printsDeferred = 0, printsDropped = 0;  // read at ExecuteCommandLists / lists reset unexecuted
    uint64_t texToTexCopies = 0, barriersSeen = 0;
    uint64_t tracked = 0, sentinelsAttached = 0, destroyed = 0, untracked = 0;
    uint64_t jobsQueued = 0, jobsDone = 0, jobsSkipped = 0, jobsFailed = 0, pending = 0;
    uint64_t bytesUploaded = 0, batches = 0, ticksWaitingGpu = 0, frames = 0;
    uint64_t indexTextures = 0, indexPrints = 0;
    double mbPerFrame = 0;            // average over the last summary interval
    bool idle = false;                // targets applied, nothing queued, no batch in flight
};
GpuSwapStats gpuSwapStats();

}  // namespace qs
