// QuattroStagioni - GPU live swap of resident textures when the season stage changes.
//
// The file hooks (io_hooks) change textures only when the game reads them; this module also rewrites
// the textures already in VRAM. It hooks only D3D12 runtime code (d3d12.dll!D3D12CreateDevice and the
// ID3D12GraphicsCommandList vtable: CopyTextureRegion, CopyResource, ResourceBarrier), never the game
// exe. Design, assumptions and risks: docs/dev/gpu_swap.md; portable logic: src/core/gpu/.
//
// Wiring (src/win/main.cpp, owned by integration):
//   - gpuSwapStart(cfg) once, AFTER qs::io::packsStart() and qs::overlayStart() (MinHook is already
//     initialised by io::install; the module tolerates either order). It returns immediately; a worker
//     thread installs the hooks and loads the packs. It needs the overlay: uploads are submitted on the
//     game's DIRECT queue that the overlay captured (overlayAcquireGameQueue()).
//   - stage changes: by default (cfg.followActivePack) the worker follows qs::io::activePackStage()
//     by itself, so season_tick.cpp needs no change. Without io packs call gpuSwapSetStage().
//   - gpuSwapStop() on FreeLibrary only (DLL_PROCESS_DETACH with reserved == NULL), BEFORE
//     qs::overlayStop() and qs::io::uninstall(): it waits for its GPU work and detaches the private-data
//     objects it attached to game textures (their code lives in this DLL).
#pragma once
#include <cstdint>
#include <string>

namespace qs {

struct GpuSwapConfig {
    std::wstring gameRoot;            // folder with 0000/, 0001/... (original data for the vanilla stage)
    std::wstring packsDir;            // absolute folder of the .qsp packs
    bool followActivePack = true;     // poll io::activePackStage() (false: only gpuSwapSetStage())
    std::string initialStage;         // used when followActivePack is false ("" = vanilla)
    int budgetKBPerTick = 8192;       // bytes uploaded per tick (one tick ~ one frame)
    int tickMs = 16;
    int ringMB = 48;                  // upload ring (3 segments); a mip larger than a segment is skipped
    int cacheMB = 96;                 // decoded texture cache
    int reswapDelayTicks = 4;         // wait after a stale game upload before fixing it (queue order)
    bool ownQueueFallback = false;    // no overlay queue after 20 s: use our own DIRECT queue (riskier)
    bool preloadAllPacks = true;      // index every pack of the folder at start (textures resident in any
                                      // stage, vanilla included, are recognised from the first frames)
    bool verifyPacks = true;          // use only packs that pass io's game build check (loadVerifiedPack)
};

// Starts the worker (idempotent). False only if the thread cannot be created.
bool gpuSwapStart(const GpuSwapConfig& cfg);
void gpuSwapStop();
// Requests a stage ("" / "U" / "none" = vanilla, otherwise a pack stage id such as "A" or "A>W.2").
// Non-blocking: the worker loads the pack (if needed) and schedules the swaps.
void gpuSwapSetStage(const std::string& stageId);

struct GpuSwapStats {
    bool hooksInstalled = false, queueReady = false, disabled = false;
    std::string activeStage;          // stage whose data the tracked textures are being moved to
    uint64_t copiesSeen = 0;          // CopyTextureRegion buffer -> texture
    uint64_t copiesCandidate = 0;     // shape known to the index
    uint64_t copiesGpuSource = 0;     // source buffer not CPU visible (cannot fingerprint)
    uint64_t printsMatched = 0, printsUnknown = 0, printsAmbiguous = 0;
    uint64_t texToTexCopies = 0, barriersSeen = 0;
    uint64_t tracked = 0, sentinelsAttached = 0, destroyed = 0, untracked = 0;
    uint64_t jobsQueued = 0, jobsDone = 0, jobsFailed = 0, bytesUploaded = 0, batches = 0, ticksWaitingGpu = 0;
    uint64_t indexTextures = 0, indexPrints = 0;
    bool idle = false;                // stage loaded, nothing queued, no batch in flight
};
GpuSwapStats gpuSwapStats();

}  // namespace qs
