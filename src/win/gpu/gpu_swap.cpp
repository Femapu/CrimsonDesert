// QuattroStagioni - GPU live swap: D3D12 hooks, resident texture identification, budgeted re-uploads.
//
// Why it is built this way (details and risks: docs/dev/gpu_swap.md):
//   - Only D3D12 runtime code is hooked: d3d12.dll!D3D12CreateDevice (to get the game's device as early
//     as possible) and three ID3D12GraphicsCommandList vtable slots read from our own throw-away lists
//     (DIRECT and COPY): CopyTextureRegion, CopyResource, ResourceBarrier. Late start: the device comes
//     from the overlay's queue instead. The game exe is never touched.
//   - Identification: a CopyTextureRegion from a CPU-visible buffer (UPLOAD heap) into a whole texture
//     subresource is fingerprinted (core/gpu/fingerprint.h: a 256-byte sample of the mip read through
//     Map on the source buffer) and looked up in the index of every opened pack (original + stage data).
//     The resource description comes from ID3D12Resource::GetDesc, so no Create*Resource hook is needed.
//   - Lifetime: every tracked resource gets a tiny COM object as private data
//     (SetPrivateDataInterface); its Release, called when the resource is destroyed, untracks it. No
//     Release hook. While one of our copies is in flight we hold a reference on the destination.
//   - Resource state: transitions recorded by the game right after its uploads (StateBefore COMMON or
//     COPY_DEST) tell us the state it leaves the texture in (usually PIXEL|NON_PIXEL_SHADER_RESOURCE);
//     never seen = COMMON (promotion/decay). Our copy is wrapped in state -> COPY_DEST -> state barriers.
//   - Targets: the worker follows io::serveSnapshot(), the per-texture stages the file hooks serve, and
//     indexes only the packs of the stages being served (plus the originals). gpu::planTargets turns that
//     into one target slot per texture; resident textures whose content differs are queued.
//   - Uploads: the worker takes jobs from the budget scheduler ([GpuSwap] MaxMBPerFrame), checks every
//     destination against its live description (gpu::checkDestination: format, size, mips, array, the
//     data layout) and skips it on any difference, writes the decoded mips into a persistently mapped
//     3-segment upload ring and records ONE command list (own allocator per segment, own list, own
//     fence). The overlay's Present hook then executes it on the game's DIRECT queue, the queue that
//     owns the swapchain, followed by a signal of our fence: one batch per presented frame, ordered with
//     the game's own submissions, and the render thread only pays ExecuteCommandLists + Signal.
//     A segment is reused only when its fence has completed: nobody ever waits on the GPU, and we never
//     wait on (or signal) the game's fences.
//   - Safety: any D3D12 failure, device removal, queue change we cannot follow or exception disables
//     the module for the session ("gpu: DISABLED: <reason>"); the file hooks keep working, so the new
//     stage shows up after a reload as before.
#include <windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <set>
#include <stdexcept>
#include <unordered_set>
#include <vector>

#include "../../core/gpu/library.h"
#include "../../core/gpu/live.h"
#include "../io_hooks.h"
#include "../log.h"
#include "../overlay/overlay.h"
#include "../setup.h"
#include "../winutil.h"
#include "MinHook.h"
#include "gpu_swap.h"

namespace qs {
namespace {

using namespace gpu;

// ID3D12GraphicsCommandList vtable slots (COM ABI, d3d12.h order)
constexpr int kVtCopyTextureRegion = 16;
constexpr int kVtCopyResource = 17;
constexpr int kVtResourceBarrier = 26;
constexpr D3D12_HEAP_TYPE kHeapTypeGpuUpload = D3D12_HEAP_TYPE(5);  // ReBAR heap, CPU visible

using CopyTextureRegionFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, const D3D12_TEXTURE_COPY_LOCATION*, UINT, UINT,
                                                     UINT, const D3D12_TEXTURE_COPY_LOCATION*, const D3D12_BOX*);
using CopyResourceFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12Resource*, ID3D12Resource*);
using ResourceBarrierFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*);
using D3D12CreateDeviceFn = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);

CopyTextureRegionFn oCopyTextureRegion = nullptr;
CopyResourceFn oCopyResource = nullptr;
ResourceBarrierFn oResourceBarrier = nullptr;
D3D12CreateDeviceFn oD3D12CreateDevice = nullptr;

// {6A1D53B1-0C3E-4E9B-9F51-5153574150FF}: private data slot of our destruction sentinel
const GUID kSentinelGuid = {0x6a1d53b1, 0x0c3e, 0x4e9b, {0x9f, 0x51, 0x51, 0x53, 0x57, 0x41, 0x50, 0xff}};

enum class State : int { Stopped = 0, Running, Disabled };
std::atomic<int> g_state{int(State::Stopped)};
std::atomic<int> g_inFlight{0};
std::atomic<bool> g_listHooked{false};
std::atomic<bool> g_stopping{false};
std::atomic<bool> g_anyTracked{false};
std::atomic<bool> g_queueReady{false};
std::atomic<bool> g_gpuBusy{false};  // a batch of ours is in flight (stats read it from other threads)
std::atomic<uint64_t> g_frames{0};   // presents seen by our callback: the scheduler's tick
std::atomic<bool> g_targetsReady{false};
std::shared_ptr<const std::vector<int>> g_targets;  // per texture id: target slot (-1 = leave); atomic_load/store
std::mutex g_reasonMutex;
std::string g_disabledReason;
HANDLE g_worker = nullptr, g_stopEvent = nullptr;
GpuSwapConfig g_cfg;

std::mutex g_hookMutex;
std::vector<void*> g_hookTargets;
std::mutex g_listHookMutex;

std::unique_ptr<StageLibrary> g_lib;
ResidentTracker g_tracker;
SwapScheduler g_sched;

std::mutex g_stageMutex;
std::string g_requestedStage;

std::mutex g_dirtyMutex;
std::vector<uint64_t> g_dirty;

std::mutex g_sentMutex;
std::unordered_set<uint64_t> g_sentinels;

thread_local bool t_ours = false;  // our own recording: the hooks ignore it

struct Counters {
    std::atomic<uint64_t> copiesSeen{0}, copiesCandidate{0}, copiesGpuSource{0}, printsMatched{0}, printsUnknown{0},
        printsAmbiguous{0}, texToTex{0}, barriers{0}, sentinels{0}, destroyed{0}, untracked{0}, jobsQueued{0},
        jobsDone{0}, jobsSkipped{0}, jobsFailed{0}, bytes{0}, batches{0}, waitingGpu{0};
} C;

struct InFlight {
    InFlight() { g_inFlight.fetch_add(1); }
    ~InFlight() { g_inFlight.fetch_sub(1); }
};

bool running() { return g_state.load() == int(State::Running); }
bool logOn(int level) { return g_cfg.logLevel >= level; }

// The one way out: the module stays off for the session, the hooks become pass-through.
void disableSwap(const std::string& why) {
    int expected = int(State::Running);
    if (!g_state.compare_exchange_strong(expected, int(State::Disabled))) return;
    {
        std::lock_guard<std::mutex> lk(g_reasonMutex);
        g_disabledReason = why;
    }
    QS_ERR("gpu: DISABLED: %s (resident textures keep their stage until the game reloads them; file hooks unaffected)",
           why.c_str());
}

int targetOf(uint32_t texture) {
    std::shared_ptr<const std::vector<int>> t = std::atomic_load(&g_targets);
    return t && texture < t->size() ? (*t)[texture] : -1;
}
void** vtableOf(void* obj) { return *reinterpret_cast<void***>(obj); }

template <class T>
void safeRelease(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

bool hookTarget(void* target, void* detour, void** original, const char* name) {
    std::lock_guard<std::mutex> lk(g_hookMutex);
    for (void* t : g_hookTargets)
        if (t == target) return true;
    MH_STATUS st = MH_CreateHook(target, detour, original);
    if (st != MH_OK) {
        QS_ERR("gpu: MH_CreateHook(%s @%p) failed: %s", name, target, MH_StatusToString(st));
        return false;
    }
    st = MH_EnableHook(target);
    if (st != MH_OK) {
        QS_ERR("gpu: MH_EnableHook(%s) failed: %s", name, MH_StatusToString(st));
        MH_RemoveHook(target);
        return false;
    }
    g_hookTargets.push_back(target);
    QS_INFO("gpu: hooked %s @%p", name, target);
    return true;
}

// ------------------------------------------------------------------ destruction sentinel
void onSentinelFired(uint64_t key) {
    if (!g_stopping.load()) {
        if (g_tracker.onDestroyed(key)) C.destroyed.fetch_add(1);
        g_sched.dropResource(key);
    }
    std::lock_guard<std::mutex> lk(g_sentMutex);
    g_sentinels.erase(key);
}

// Private data object of a tracked resource. The resource holds the only reference; the runtime
// releases it when the resource is destroyed (or when we detach it at unload).
class Sentinel final : public IUnknown {
public:
    explicit Sentinel(uint64_t key) : key_(key) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (riid == __uuidof(IUnknown)) {
            *out = static_cast<IUnknown*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ULONG(InterlockedIncrement(&ref_)); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG r = InterlockedDecrement(&ref_);
        if (r == 0) {
            onSentinelFired(key_);
            delete this;
        }
        return ULONG(r);
    }

private:
    virtual ~Sentinel() = default;
    LONG ref_ = 1;
    uint64_t key_;
};

void attachSentinel(ID3D12Resource* r) {
    const uint64_t key = uint64_t(reinterpret_cast<uintptr_t>(r));
    {
        std::lock_guard<std::mutex> lk(g_sentMutex);
        if (!g_sentinels.insert(key).second) return;  // one sentinel per live object
    }
    auto* s = new (std::nothrow) Sentinel(key);
    if (!s) {
        onSentinelFired(key);
        return;
    }
    if (SUCCEEDED(r->SetPrivateDataInterface(kSentinelGuid, s))) C.sentinels.fetch_add(1);
    else QS_DBG("gpu: SetPrivateDataInterface failed on %p, not tracked", (void*)r);
    s->Release();  // on failure this fires at once and untracks the resource: no lifetime, no tracking
}

void pushDirty(uint64_t key) {
    std::lock_guard<std::mutex> lk(g_dirtyMutex);
    g_dirty.push_back(key);
}

// ------------------------------------------------------------------ hook logic
ResourceShape shapeOf(const D3D12_RESOURCE_DESC& d) {
    ResourceShape s;
    s.dxgiFormat = uint32_t(d.Format);
    s.width = uint32_t(d.Width);
    s.height = d.Height;
    s.mipLevels = d.MipLevels ? d.MipLevels : 1;
    s.arraySize = d.DepthOrArraySize;
    return s;
}

bool coversWhole(UINT x, UINT y, UINT z, const D3D12_BOX* box, const MipLayout& l) {
    if (x || y || z) return false;
    if (!box) return true;
    return box->left == 0 && box->top == 0 && box->front == 0 && box->right >= l.width && box->bottom >= l.height;
}

void trackMatch(ID3D12Resource* dr, const ResourceShape& sh, uint32_t sub, const Match& m) {
    const uint64_t key = uint64_t(reinterpret_cast<uintptr_t>(dr));
    auto kind = g_tracker.onUpload(key, sh, sub, m, g_frames.load());
    if (kind == ResidentTracker::Upload::Ignored) return;
    g_anyTracked.store(true);
    if (kind == ResidentTracker::Upload::New || kind == ResidentTracker::Upload::Retargeted) attachSentinel(dr);
    const int tgt = targetOf(m.texture);
    if (tgt >= 0 && !(m.slots & slotBit(tgt))) pushDirty(key);  // uploaded with another stage than the target
}

void observeUpload(ID3D12Resource* dr, UINT sub, UINT x, UINT y, UINT z, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp,
                   ID3D12Resource* sr, const D3D12_BOX* box) {
    C.copiesSeen.fetch_add(1, std::memory_order_relaxed);
    const D3D12_RESOURCE_DESC dd = dr->GetDesc();
    if (dd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || dd.DepthOrArraySize != 1) return;
    const Family fam = familyFromDxgi(uint32_t(dd.Format));
    if (fam == Family::Unknown) return;
    const ResourceShape sh = shapeOf(dd);
    if (sub >= sh.mipLevels) return;  // other array slice / plane
    const MipLayout l = mipLayout(fam, sh.width, sh.height, sub);
    if (!coversWhole(x, y, z, box, l) || fp.Footprint.Width < l.width || fp.Footprint.Height < l.height ||
        fp.Footprint.RowPitch < l.rowBytes)
        return;  // partial update: tracking left as is
    const uint64_t key = uint64_t(reinterpret_cast<uintptr_t>(dr));
    if (!g_lib->index().hasShape(fam, l.width, l.height)) {
        if (g_anyTracked.load() && g_tracker.onUnknownUpload(key, sub)) C.untracked.fetch_add(1);
        return;
    }
    C.copiesCandidate.fetch_add(1, std::memory_order_relaxed);
    D3D12_HEAP_PROPERTIES hp{};
    D3D12_HEAP_FLAGS hf{};
    bool cpu = SUCCEEDED(sr->GetHeapProperties(&hp, &hf)) &&
               (hp.Type == D3D12_HEAP_TYPE_UPLOAD || hp.Type == kHeapTypeGpuUpload ||
                (hp.Type == D3D12_HEAP_TYPE_CUSTOM && hp.CPUPageProperty != D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE &&
                 hp.CPUPageProperty != D3D12_CPU_PAGE_PROPERTY_UNKNOWN));
    if (!cpu) {
        C.copiesGpuSource.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    void* p = nullptr;
    if (FAILED(sr->Map(0, nullptr, &p)) || !p) return;
    const uint64_t print = mipPrint(fam, l, static_cast<const uint8_t*>(p) + fp.Offset, fp.Footprint.RowPitch);
    D3D12_RANGE none{0, 0};
    sr->Unmap(0, &none);
    Match m;
    if (g_lib->index().lookup(print, m)) {
        C.printsMatched.fetch_add(1, std::memory_order_relaxed);
        trackMatch(dr, sh, sub, m);
    } else if (m.texture == kAmbiguous) {
        C.printsAmbiguous.fetch_add(1, std::memory_order_relaxed);
    } else {
        C.printsUnknown.fetch_add(1, std::memory_order_relaxed);
        if (g_anyTracked.load() && g_tracker.onUnknownUpload(key, sub)) C.untracked.fetch_add(1);
    }
}

void observeTexToTex(ID3D12Resource* dr, UINT dsub, ID3D12Resource* sr, UINT ssub, UINT x, UINT y, UINT z, const D3D12_BOX* box) {
    if (!g_anyTracked.load()) return;
    const uint64_t dk = uint64_t(reinterpret_cast<uintptr_t>(dr)), sk = uint64_t(reinterpret_cast<uintptr_t>(sr));
    if (!g_tracker.tracked(sk) && !g_tracker.tracked(dk)) return;
    C.texToTex.fetch_add(1, std::memory_order_relaxed);
    const D3D12_RESOURCE_DESC dd = dr->GetDesc();
    const ResourceShape sh = shapeOf(dd);
    const Family fam = familyFromDxgi(sh.dxgiFormat);
    if (fam == Family::Unknown || dsub >= sh.mipLevels) return;
    if (!coversWhole(x, y, z, box, mipLayout(fam, sh.width, sh.height, dsub))) return;
    if (g_tracker.onCopy(dk, sk, sh, int(dsub), int(ssub))) attachSentinel(dr);
    else if (g_tracker.onUnknownUpload(dk, dsub)) C.untracked.fetch_add(1);
}

void STDMETHODCALLTYPE hkCopyTextureRegion(ID3D12GraphicsCommandList* list, const D3D12_TEXTURE_COPY_LOCATION* dst, UINT x, UINT y,
                                           UINT z, const D3D12_TEXTURE_COPY_LOCATION* src, const D3D12_BOX* box) {
    InFlight g;
    if (!t_ours && running() && dst && src && dst->pResource && src->pResource &&
        dst->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX) {
        try {
            if (src->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT)
                observeUpload(dst->pResource, dst->SubresourceIndex, x, y, z, src->PlacedFootprint, src->pResource, box);
            else
                observeTexToTex(dst->pResource, dst->SubresourceIndex, src->pResource, src->SubresourceIndex, x, y, z, box);
        } catch (...) {
            disableSwap("exception in the CopyTextureRegion hook");
        }
    }
    oCopyTextureRegion(list, dst, x, y, z, src, box);
}

void STDMETHODCALLTYPE hkCopyResource(ID3D12GraphicsCommandList* list, ID3D12Resource* dst, ID3D12Resource* src) {
    InFlight g;
    if (!t_ours && running() && dst && src && g_anyTracked.load()) {
        try {
            const uint64_t dk = uint64_t(reinterpret_cast<uintptr_t>(dst)), sk = uint64_t(reinterpret_cast<uintptr_t>(src));
            if (g_tracker.tracked(sk) || g_tracker.tracked(dk)) {
                C.texToTex.fetch_add(1, std::memory_order_relaxed);
                if (g_tracker.onCopy(dk, sk, shapeOf(dst->GetDesc()))) attachSentinel(dst);
            }
        } catch (...) {
            disableSwap("exception in the CopyResource hook");
        }
    }
    oCopyResource(list, dst, src);
}

void STDMETHODCALLTYPE hkResourceBarrier(ID3D12GraphicsCommandList* list, UINT n, const D3D12_RESOURCE_BARRIER* b) {
    InFlight g;
    if (!t_ours && b && g_anyTracked.load() && running()) {
        for (UINT i = 0; i < n; ++i) {
            if (b[i].Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION || !b[i].Transition.pResource) continue;
            const D3D12_RESOURCE_STATES before = b[i].Transition.StateBefore;
            // only transitions out of COMMON / copy states (cheap filter: the game's upload transitions and
            // copy-from-texture round trips; per-frame render target barriers rarely start from these)
            if (before != D3D12_RESOURCE_STATE_COMMON &&
                !(before & (D3D12_RESOURCE_STATE_COPY_DEST | D3D12_RESOURCE_STATE_COPY_SOURCE)))
                continue;
            C.barriers.fetch_add(1, std::memory_order_relaxed);
            try {
                g_tracker.onState(uint64_t(reinterpret_cast<uintptr_t>(b[i].Transition.pResource)),
                                  uint32_t(b[i].Transition.StateAfter));
            } catch (...) {
                disableSwap("exception in the ResourceBarrier hook");
                break;
            }
        }
    }
    oResourceBarrier(list, n, b);
}

// Reads the command list vtable from throw-away DIRECT and COPY lists of `device` and hooks it.
void installListHooks(ID3D12Device* device) {
    if (g_listHooked.load() || !device) return;
    std::lock_guard<std::mutex> lk(g_listHookMutex);
    if (g_listHooked.load()) return;
    bool ok = false;
    const D3D12_COMMAND_LIST_TYPE types[] = {D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_TYPE_COPY};
    for (D3D12_COMMAND_LIST_TYPE t : types) {
        ID3D12CommandAllocator* a = nullptr;
        ID3D12GraphicsCommandList* l = nullptr;
        if (SUCCEEDED(device->CreateCommandAllocator(t, IID_PPV_ARGS(&a))) &&
            SUCCEEDED(device->CreateCommandList(0, t, a, nullptr, IID_PPV_ARGS(&l)))) {
            void** vt = vtableOf(l);
            bool h = hookTarget(vt[kVtCopyTextureRegion], reinterpret_cast<void*>(&hkCopyTextureRegion),
                                reinterpret_cast<void**>(&oCopyTextureRegion), "ID3D12GraphicsCommandList::CopyTextureRegion");
            h = hookTarget(vt[kVtCopyResource], reinterpret_cast<void*>(&hkCopyResource), reinterpret_cast<void**>(&oCopyResource),
                           "ID3D12GraphicsCommandList::CopyResource") && h;
            h = hookTarget(vt[kVtResourceBarrier], reinterpret_cast<void*>(&hkResourceBarrier),
                           reinterpret_cast<void**>(&oResourceBarrier), "ID3D12GraphicsCommandList::ResourceBarrier") && h;
            ok = ok || h;
            l->Close();
        }
        safeRelease(l);
        safeRelease(a);
    }
    g_listHooked.store(ok);
    if (!ok) disableSwap("command list hooks failed");
}

HRESULT WINAPI hkD3D12CreateDevice(IUnknown* adapter, D3D_FEATURE_LEVEL fl, REFIID riid, void** out) {
    InFlight g;
    HRESULT hr = oD3D12CreateDevice(adapter, fl, riid, out);
    if (SUCCEEDED(hr) && out && *out && running() && !g_listHooked.load()) {
        ID3D12Device* dev = nullptr;
        if (SUCCEEDED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&dev)))) {
            QS_INFO("gpu: game created D3D12 device %p", (void*)dev);
            installListHooks(dev);
            dev->Release();
        }
    }
    return hr;
}

// ------------------------------------------------------------------ original data (.paz) without the file hooks
// io_hooks substitutes ReadFile on .paz handles, so the vanilla stage reads the archive with NtReadFile.
using NtReadFileFn = LONG(NTAPI*)(HANDLE, HANDLE, void*, void*, void*, void*, ULONG, LARGE_INTEGER*, ULONG*);
struct IoStatus {
    union {
        LONG Status;
        void* Pointer;
    };
    ULONG_PTR Information;
};

class NtFileSource : public pack::ByteSource {
public:
    NtFileSource(HANDLE h, uint64_t size, NtReadFileFn rd) : h_(h), size_(size), rd_(rd) {}
    ~NtFileSource() override { CloseHandle(h_); }
    bool readAt(uint64_t off, void* dst, size_t n) override {
        if (off + n > size_) return false;
        std::lock_guard<std::mutex> lk(m_);
        uint8_t* p = static_cast<uint8_t*>(dst);
        while (n) {
            ULONG chunk = n > (1u << 30) ? (1u << 30) : ULONG(n);
            IoStatus io{};
            LARGE_INTEGER o;
            o.QuadPart = LONGLONG(off);
            LONG st = rd_(h_, nullptr, nullptr, nullptr, &io, p, chunk, &o, nullptr);
            if (st != 0 || io.Information != chunk) return false;
            p += chunk;
            off += chunk;
            n -= chunk;
        }
        return true;
    }
    uint64_t size() const override { return size_; }

private:
    HANDLE h_;
    uint64_t size_;
    NtReadFileFn rd_;
    std::mutex m_;
};

std::unique_ptr<pack::ByteSource> openPaz(const std::string& group, uint32_t pazIndex) {
    static NtReadFileFn rd = reinterpret_cast<NtReadFileFn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtReadFile")));
    if (!rd) return nullptr;
    std::wstring rel = widen(group);
    for (wchar_t& c : rel)
        if (c == L'/') c = L'\\';
    std::wstring path = joinPath(joinPath(g_cfg.gameRoot, rel), std::to_wstring(pazIndex) + L".paz");
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        QS_ERR("gpu: cannot open %s (%lu): vanilla stage unavailable for its textures", narrow(path).c_str(), GetLastError());
        return nullptr;
    }
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    return std::make_unique<NtFileSource>(h, uint64_t(sz.QuadPart), rd);
}

// ------------------------------------------------------------------ uploader
// The worker owns everything below. The present callback only touches U.queue, U.list, U.fence and the
// batch hand-off, under g_upMutex taken with try_lock: the render thread never waits for the worker.
enum : int { kBatchNone = 0, kBatchRecorded = 1, kBatchSubmitted = 2 };

struct Segment {
    ID3D12CommandAllocator* alloc = nullptr;
    UINT64 fence = 0;                   // our fence value that releases this segment
    uint64_t offset = 0;
    std::vector<ID3D12Resource*> held;  // destinations of the copies recorded in this segment
};

struct Uploader {
    ID3D12Device* dev = nullptr;
    ID3D12CommandQueue* queue = nullptr;  // the game's DIRECT queue (overlay), referenced
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;         // ours: we never wait on or signal the game's fences
    HANDLE ev = nullptr;
    UINT64 fenceLast = 0;
    ID3D12Resource* ring = nullptr;
    uint8_t* mapped = nullptr;
    uint64_t segSize = 0;
    Segment seg[3];
    int cur = 0;
    bool ready = false;
    UINT64 batchFence = 0;                // value signalled after the recorded batch
    std::vector<SwapJob> batchJobs;
    uint64_t batchBytes = 0;
} U;

std::mutex g_upMutex;
std::atomic<int> g_batch{kBatchNone};
std::atomic<bool> g_submitFailed{false}, g_queueMismatch{false};
bool g_replan = false;  // worker: rebuild the job list (uploader rebuilt, recorded batch dropped)

void reclaim(UINT64 done) {
    for (Segment& s : U.seg)
        if (s.fence <= done && !s.held.empty()) {
            for (ID3D12Resource* r : s.held) r->Release();  // may be the last reference: sentinel fires
            s.held.clear();
        }
}

// Present thread (overlay render lock held). Never blocks.
void presentCallback(ID3D12CommandQueue* q) {
    InFlight g;
    g_frames.fetch_add(1, std::memory_order_relaxed);
    if (!running() || g_batch.load() != kBatchRecorded) return;
    std::unique_lock<std::mutex> lk(g_upMutex, std::try_to_lock);
    if (!lk.owns_lock() || !U.ready || g_batch.load() != kBatchRecorded) return;
    if (q != U.queue) {  // the overlay moved to another swapchain/queue: the worker rebuilds
        g_queueMismatch.store(true);
        return;
    }
    ID3D12CommandList* lists[] = {U.list};
    U.queue->ExecuteCommandLists(1, lists);
    if (FAILED(U.queue->Signal(U.fence, U.batchFence))) g_submitFailed.store(true);
    g_batch.store(kBatchSubmitted);
}

// Worker: a submitted batch is now in the game's queue; its copies count as done.
void finalizeBatch() {
    if (g_batch.load() != kBatchSubmitted) return;
    std::lock_guard<std::mutex> lk(g_upMutex);
    for (const SwapJob& j : U.batchJobs) {
        g_tracker.onSwapped(j.resource, j.sub, j.slot);
        C.bytes.fetch_add(j.bytes, std::memory_order_relaxed);
    }
    C.jobsDone.fetch_add(U.batchJobs.size());
    C.batches.fetch_add(1);
    if (logOn(2))
        QS_INFO("gpu: batch %llu submitted: %zu subresources, %llu KB, fence %llu", (unsigned long long)C.batches.load(),
                U.batchJobs.size(), (unsigned long long)(U.batchBytes >> 10), (unsigned long long)U.batchFence);
    U.batchJobs.clear();
    U.cur = (U.cur + 1) % 3;
    g_batch.store(kBatchNone);
}

void releaseUploader(bool wait) {
    finalizeBatch();
    std::lock_guard<std::mutex> lk(g_upMutex);
    if (g_batch.load() == kBatchRecorded) {  // never executed: its references are released now
        Segment& s = U.seg[U.cur];
        for (ID3D12Resource* r : s.held) r->Release();
        s.held.clear();
        s.fence = 0;
        U.batchJobs.clear();
        g_batch.store(kBatchNone);
        g_replan = true;
    }
    // Our own fence only (bounded): the game's fences are never waited on.
    if (U.fence && U.ev && wait && U.fence->GetCompletedValue() < U.fenceLast &&
        SUCCEEDED(U.fence->SetEventOnCompletion(U.fenceLast, U.ev)))
        WaitForSingleObject(U.ev, 2000);
    if (U.fence) reclaim(wait ? U.fence->GetCompletedValue() : 0);
    for (Segment& s : U.seg) {
        if (!wait) s.held.clear();  // device lost: leak rather than release into a dead device
        else if (!s.held.empty()) {  // GPU did not finish in 2 s: leak (safer than freeing in-flight memory)
            QS_ERR("gpu: %zu destinations still in flight at release, references kept", s.held.size());
            s.held.clear();
        }
        safeRelease(s.alloc);
        s = Segment();
    }
    if (U.ring && U.mapped) U.ring->Unmap(0, nullptr);
    safeRelease(U.ring);
    safeRelease(U.list);
    safeRelease(U.fence);
    if (U.ev) CloseHandle(U.ev);
    safeRelease(U.queue);
    safeRelease(U.dev);
    U = Uploader();
    g_queueReady.store(false);
}

// Takes ownership of the queue reference.
bool initUploader(ID3D12CommandQueue* queue) {
    D3D12_COMMAND_QUEUE_DESC qd = queue->GetDesc();
    if (qd.Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
        queue->Release();
        QS_ERR("gpu: the overlay queue is not a DIRECT queue");
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(g_upMutex);
        U.queue = queue;
    }
    if (FAILED(queue->GetDevice(IID_PPV_ARGS(&U.dev)))) {
        releaseUploader(true);
        return false;
    }
    installListHooks(U.dev);  // late start: no D3D12CreateDevice seen
    const uint64_t ring = uint64_t(std::max(12, g_cfg.ringMB)) << 20;
    U.segSize = ring / 3 / D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT * D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = U.segSize * 3;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bool ok = SUCCEEDED(U.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                       IID_PPV_ARGS(&U.ring)));
    void* p = nullptr;
    D3D12_RANGE none{0, 0};
    ok = ok && SUCCEEDED(U.ring->Map(0, &none, &p)) && p;
    U.mapped = static_cast<uint8_t*>(p);
    for (int i = 0; ok && i < 3; ++i) {
        U.seg[i].offset = U.segSize * uint64_t(i);
        ok = SUCCEEDED(U.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&U.seg[i].alloc)));
    }
    ok = ok && SUCCEEDED(U.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, U.seg[0].alloc, nullptr, IID_PPV_ARGS(&U.list))) &&
         SUCCEEDED(U.list->Close());
    ok = ok && SUCCEEDED(U.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&U.fence)));
    U.ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ok = ok && U.ev;
    if (!ok) {
        QS_ERR("gpu: uploader setup failed");
        releaseUploader(true);
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(g_upMutex);
        U.ready = true;
    }
    g_queueReady.store(true);
    QS_INFO("gpu: uploader ready on the game queue %p (ring %llu MB, budget %.2f MB per frame)", (void*)queue,
            (unsigned long long)(U.segSize * 3 >> 20), double(g_cfg.budgetBytesPerFrame) / 1048576.0);
    return true;
}

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* r, UINT sub, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER x{};
    x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    x.Transition.pResource = r;
    x.Transition.Subresource = sub;
    x.Transition.StateBefore = a;
    x.Transition.StateAfter = b;
    return x;
}

void skipJob(const SwapJob& j, const char* why) {
    const uint64_t n = C.jobsSkipped.fetch_add(1);
    if (logOn(1) && n < 20)
        QS_INFO("gpu: swap skipped (%s): resource %p sub %u texture %u", why, (void*)uintptr_t(j.resource), j.sub, j.texture);
}

// Worker: records the next batch (one segment, at most the per-frame budget) for the present callback.
void recordBatch() {
    if (g_batch.load() != kBatchNone) return;
    const UINT64 done = U.fence->GetCompletedValue();
    if (done == UINT64_MAX) {  // fence of a removed device
        releaseUploader(false);
        disableSwap("D3D12 device removed (fence)");
        return;
    }
    reclaim(done);
    Segment& s = U.seg[U.cur];
    if (s.fence > done) {
        C.waitingGpu.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const uint64_t budget = std::min<uint64_t>(std::max<uint64_t>(64 << 10, g_cfg.budgetBytesPerFrame), U.segSize);
    std::vector<SwapJob> jobs = g_sched.take(budget, g_frames.load());
    if (jobs.empty()) return;

    std::vector<D3D12_RESOURCE_BARRIER> pre, post;
    std::vector<std::pair<D3D12_TEXTURE_COPY_LOCATION, D3D12_TEXTURE_COPY_LOCATION>> copies;
    std::vector<SwapJob> did;
    uint64_t used = 0;
    for (size_t ji = 0; ji < jobs.size(); ++ji) {
        const SwapJob& j = jobs[ji];
        Resident r;
        if (!g_tracker.get(j.resource, r) || r.texture != j.texture || j.sub >= r.content.size()) {
            skipJob(j, "resource gone or reused");
            continue;
        }
        if (r.content[j.sub] & slotBit(j.slot)) continue;  // already right (a game upload did it)
        if (!r.stateSeen && !g_cfg.assumeCommonState) {
            skipJob(j, "state after the game's upload never seen");
            continue;
        }
        TextureDesc td;
        if (!g_lib->index().texture(j.texture, td)) {
            skipJob(j, "texture not in the index");
            continue;
        }
        StageLibrary::MipData md;
        std::string err;
        if (!g_lib->mipData(j.texture, j.slot, j.ddsMip, md, &err)) {
            if (C.jobsFailed.fetch_add(1) < 20)
                QS_ERR("gpu: no data for texture %u stage %s mip %u: %s", j.texture, g_lib->stageOf(j.slot).c_str(), j.ddsMip,
                       err.c_str());
            continue;
        }
        auto* res = reinterpret_cast<ID3D12Resource*>(uintptr_t(j.resource));
        bool alive = false;
        g_tracker.withTracked(j.resource, [&](const Resident&) { alive = res->AddRef() > 1; });
        if (!alive) {  // not tracked any more, or being destroyed (that reference is not ours to release)
            skipJob(j, "resource being destroyed");
            continue;
        }
        const D3D12_RESOURCE_DESC dd = res->GetDesc();
        LiveDesc live;
        live.dimension = uint32_t(dd.Dimension);
        live.dxgiFormat = uint32_t(dd.Format);
        live.width = dd.Width;
        live.height = dd.Height;
        live.depthOrArraySize = dd.DepthOrArraySize;
        live.mipLevels = dd.MipLevels;
        live.sampleCount = dd.SampleDesc.Count;
        live.layout = uint32_t(dd.Layout);
        live.flags = uint32_t(dd.Flags);
        std::string why;
        if (!checkDestination(live, r, td, j.sub, j.ddsMip, md.layout, &why)) {
            res->Release();
            skipJob(j, why.c_str());
            continue;
        }
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT rows = 0;
        UINT64 rowSize = 0, total = 0;
        const uint64_t base = s.offset + (used + 511) / 512 * 512;
        U.dev->GetCopyableFootprints(&dd, j.sub, 1, base, &fp, &rows, &rowSize, &total);
        if (rows != md.layout.rows || rowSize != md.layout.rowBytes || total == UINT64(-1) ||
            fp.Footprint.Width < md.layout.width || fp.Footprint.Height < md.layout.height) {
            res->Release();
            skipJob(j, "copyable footprint differs from the data");
            continue;
        }
        if (fp.Offset + total > s.offset + U.segSize) {
            res->Release();
            if (copies.empty()) {  // larger than a whole segment: cannot ever be uploaded
                skipJob(j, "mip larger than the upload segment (raise RingMB)");
                continue;
            }
            for (size_t k = ji; k < jobs.size(); ++k) g_sched.push(jobs[k]);  // next frame
            break;
        }
        packedToFootprint(md.dds->data() + md.offset, md.layout, U.mapped + fp.Offset, fp.Footprint.RowPitch);
        used = fp.Offset + total - s.offset;
        const auto st = r.stateSeen ? D3D12_RESOURCE_STATES(r.state) : D3D12_RESOURCE_STATE_COMMON;
        if (st != D3D12_RESOURCE_STATE_COPY_DEST) {
            pre.push_back(transition(res, j.sub, st, D3D12_RESOURCE_STATE_COPY_DEST));
            post.push_back(transition(res, j.sub, D3D12_RESOURCE_STATE_COPY_DEST, st));
        }
        D3D12_TEXTURE_COPY_LOCATION dl{}, sl{};
        dl.pResource = res;
        dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dl.SubresourceIndex = j.sub;
        sl.pResource = U.ring;
        sl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        sl.PlacedFootprint = fp;
        copies.emplace_back(dl, sl);
        s.held.push_back(res);
        did.push_back(j);
    }
    if (copies.empty()) return;
    t_ours = true;
    bool ok = SUCCEEDED(s.alloc->Reset()) && SUCCEEDED(U.list->Reset(s.alloc, nullptr));
    if (ok) {
        if (!pre.empty()) U.list->ResourceBarrier(UINT(pre.size()), pre.data());
        for (auto& c : copies) U.list->CopyTextureRegion(&c.first, 0, 0, 0, &c.second, nullptr);
        if (!post.empty()) U.list->ResourceBarrier(UINT(post.size()), post.data());
        ok = SUCCEEDED(U.list->Close());
    }
    t_ours = false;
    if (!ok) {
        for (ID3D12Resource* r : s.held) r->Release();
        s.held.clear();
        disableSwap("recording our command list failed");
        return;
    }
    std::lock_guard<std::mutex> lk(g_upMutex);
    U.batchFence = ++U.fenceLast;
    s.fence = U.batchFence;  // the segment and its references wait for this value
    U.batchJobs = std::move(did);
    U.batchBytes = used;
    g_batch.store(kBatchRecorded);
}

// ------------------------------------------------------------------ worker
uint64_t mipBytesFn(uint32_t t, uint32_t m) { return g_lib->mipBytes(t, m); }

// Opens, checks and indexes one pack file. Returns its slot or -1.
int loadPack(const std::wstring& path, std::string& err) {
    if (g_cfg.verifyPacks) {  // same game build / pamt checks as io_hooks: never swap in a stale pack
        std::string why;
        if (!loadVerifiedPack(g_cfg.gameRoot, path, 1u << 20, why)) {
            err = why;
            return -1;
        }
    }
    PackLoadStats st;
    const DWORD t0 = GetTickCount();
    auto src = pack::openFileSource(narrow(path), &err);
    const int slot = src ? g_lib->addPack(std::move(src), &err, &st) : -1;
    if (slot < 0) return -1;
    QS_INFO("gpu: pack %s (stage %s) indexed in %lu ms: %u records, %u indexed, %u unsupported, %u originals unreadable, "
            "prints %s, %llu MB read; index %zu textures, %zu prints (%zu ambiguous)",
            narrow(path).c_str(), g_lib->stageOf(slot).c_str(), GetTickCount() - t0, st.records, st.indexed, st.skippedFormat,
            st.originalErrors, st.printsFromIndex ? "from the pack index" : "computed from the data",
            (unsigned long long)(st.bytesRead >> 20), g_lib->index().textureCount(), g_lib->index().printCount(),
            g_lib->index().ambiguousCount());
    return slot;
}

std::vector<std::wstring> g_preload;  // pack files still to index (worker only)

void listPacks() {
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW(joinPath(g_cfg.packsDir, L"*.qsp").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) g_preload.push_back(joinPath(g_cfg.packsDir, fd.cFileName));
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    std::sort(g_preload.begin(), g_preload.end());
    QS_INFO("gpu: %zu packs to index in the background (PreloadAllPacks=1)", g_preload.size());
}

void preloadOne() {
    if (g_preload.empty() || !g_targetsReady.load()) return;
    std::wstring path = g_preload.back();
    g_preload.pop_back();
    std::string err;
    if (loadPack(path, err) < 0) QS_INFO("gpu: pack %s skipped: %s", narrow(path).c_str(), err.c_str());
}

std::shared_ptr<const pack::ServeSnapshot> g_lastSnap;
size_t g_lastTexCount = size_t(-1);
std::string g_lastWant = "\x01";  // never a real id: forces the first plan
std::set<std::string> g_failedStages;
DWORD g_lastPlanLog = 0;

// Stage the file hooks serve for one texture of the snapshot ("" = vanilla).
bool servedBy(const pack::ServeSnapshot* snap, const std::string& paz, uint64_t off, std::string& stage) {
    stage.clear();
    if (!snap || !snap->index) return true;  // nothing published: the game reads vanilla data
    const int ps = snap->slotFor(paz);
    if (ps < 0) return true;  // no open pack touches this archive
    uint32_t a = 0, b = 0;
    snap->index->overlapping(ps, off, 1, a, b);
    const auto& tex = snap->index->textures();
    for (uint32_t i = a; i < b && i < tex.size(); ++i)
        if (tex[i].offset == off) {
            const int p = i < snap->serve.size() ? snap->serve[i] : -1;
            if (p >= 0 && size_t(p) < snap->index->packs().size()) stage = snap->index->packs()[size_t(p)]->stageId;
            return true;
        }
    return true;  // not listed by any open pack: vanilla
}

void scheduleAll(const TargetPlan& plan) {
    auto targets = std::make_shared<const std::vector<int>>(plan.slot);
    std::atomic_store(&g_targets, targets);
    g_targetsReady.store(true);
    std::vector<SwapJob> jobs;
    g_tracker.plan([&targets](uint32_t t) { return t < targets->size() ? (*targets)[t] : -1; }, &mipBytesFn, jobs,
                   g_frames.load());
    g_sched.clear();
    for (const SwapJob& j : jobs) g_sched.push(j);
    C.jobsQueued.fetch_add(jobs.size());
    const DWORD now = GetTickCount();
    if ((logOn(1) && (!jobs.empty() || now - g_lastPlanLog > 60000)) || logOn(2)) {
        g_lastPlanLog = now;
        std::string waiting;
        for (const std::string& m : plan.missingStages) waiting += (waiting.empty() ? "" : ", ") + m;
        QS_INFO("gpu: targets: %zu staged, %zu vanilla, %zu waiting for a pack%s%s%s, %zu not served; %zu resident textures "
                "tracked, %zu subresources scheduled (%llu KB)",
                plan.staged, plan.vanilla, plan.waiting, waiting.empty() ? "" : " (", waiting.c_str(), waiting.empty() ? "" : ")",
                plan.unknown, g_tracker.size(), jobs.size(), (unsigned long long)(g_sched.queuedBytes() >> 10));
    }
}

int ensureStage(const std::string& stage) {
    int slot = g_lib->slotOf(stage);
    if (slot >= 0 || g_failedStages.count(stage)) return slot;
    std::string err;
    slot = loadPack(resolvePackPath(g_cfg.packsDir, stage), err);
    if (slot < 0) {
        QS_ERR("gpu: pack for stage %s not usable (%s): its resident textures change only after a reload", stage.c_str(),
               err.c_str());
        g_failedStages.insert(stage);
    }
    return slot;
}

// Follows what the file hooks serve. Loads at most one pack per call (indexing takes seconds).
void refreshTargets() {
    TargetPlan plan;
    auto targetFor = [](uint32_t t, int slot) { return g_lib->targetFor(t, slot); };
    if (g_cfg.followServed) {
        std::shared_ptr<const pack::ServeSnapshot> snap = io::serveSnapshot();
        if (snap && snap->index) {
            const auto& packs = snap->index->packs();
            for (size_t p = 0; p < packs.size(); ++p) {
                if (p < snap->packAssigned.size() && snap->packAssigned[p] == 0) continue;  // serves nothing now
                const std::string& st = packs[p]->stageId;
                if (g_lib->slotOf(st) >= 0 || g_failedStages.count(st)) continue;
                ensureStage(st);
                return;  // next iteration: one pack at a time keeps stop and recording responsive
            }
        }
        const size_t texCount = g_lib->index().textureCount();
        if (!g_replan && snap == g_lastSnap && texCount == g_lastTexCount) return;
        const pack::ServeSnapshot* sp = snap.get();
        plan = planTargets(
            g_lib->index(), [sp](const std::string& paz, uint64_t off, std::string& st) { return servedBy(sp, paz, off, st); },
            [](const std::string& st) { return g_lib->slotOf(st); }, targetFor);
        g_lastSnap = snap;
        g_lastTexCount = texCount;
    } else {
        std::string want;
        {
            std::lock_guard<std::mutex> lk(g_stageMutex);
            want = g_requestedStage;
        }
        const int slot = want.empty() || want == "U" || want == "none" ? 0 : ensureStage(want);
        const size_t texCount = g_lib->index().textureCount();
        if (!g_replan && want == g_lastWant && texCount == g_lastTexCount) return;
        plan = planGlobal(g_lib->index(), slot, targetFor);
        g_lastWant = want;
        g_lastTexCount = texCount;
    }
    g_replan = false;
    scheduleAll(plan);
}

void drainDirty() {
    std::vector<uint64_t> d;
    {
        std::lock_guard<std::mutex> lk(g_dirtyMutex);
        d.swap(g_dirty);
    }
    if (d.empty() || !g_targetsReady.load()) return;
    std::sort(d.begin(), d.end());
    d.erase(std::unique(d.begin(), d.end()), d.end());
    std::vector<SwapJob> jobs;
    const uint64_t notBefore = g_frames.load() + uint64_t(std::max(0, g_cfg.reswapDelayFrames));
    for (uint64_t k : d) g_tracker.planResource(k, [](uint32_t t) { return targetOf(t); }, &mipBytesFn, jobs, notBefore);
    for (const SwapJob& j : jobs) g_sched.push(j);
    C.jobsQueued.fetch_add(jobs.size());
}

void ensureUploader() {
    static unsigned checks = 0;
    if (U.ready) {
        if (!g_queueMismatch.load() && (++checks % 256) != 0) return;  // ~1 s, or at once on a mismatch
        ID3D12CommandQueue* q = overlayAcquireGameQueue();
        if (q && q != U.queue) {
            QS_INFO("gpu: game queue changed (%p -> %p), rebuilding the uploader", (void*)U.queue, (void*)q);
            releaseUploader(true);
            g_queueMismatch.store(false);
            g_replan = true;
            if (!initUploader(q)) disableSwap("uploader setup failed on the new game queue");
            return;
        }
        if (q) {
            q->Release();
            g_queueMismatch.store(false);
        }
        return;
    }
    ID3D12CommandQueue* q = overlayAcquireGameQueue();
    if (q && !initUploader(q)) disableSwap("uploader setup failed");
}

FrameRate g_rate;
std::atomic<double> g_mbPerFrame{0.0};

void logSummary(const char* why) {
    GpuSwapStats s = gpuSwapStats();
    QS_INFO("gpu [%s]: tracked %llu, pending %llu, scheduled %llu, done %llu, skipped %llu, failed %llu, %.2f MB/frame, "
            "%llu MB in %llu batches, frames %llu, gpu waits %llu; copies %llu (candidates %llu, gpu-only source %llu), "
            "prints matched %llu unknown %llu ambiguous %llu; sentinels %llu, destroyed %llu, untracked %llu; index %llu "
            "textures, %llu prints%s%s",
            why, (unsigned long long)s.tracked, (unsigned long long)s.pending, (unsigned long long)s.jobsQueued,
            (unsigned long long)s.jobsDone, (unsigned long long)s.jobsSkipped, (unsigned long long)s.jobsFailed, s.mbPerFrame,
            (unsigned long long)(s.bytesUploaded >> 20), (unsigned long long)s.batches, (unsigned long long)s.frames,
            (unsigned long long)s.ticksWaitingGpu, (unsigned long long)s.copiesSeen, (unsigned long long)s.copiesCandidate,
            (unsigned long long)s.copiesGpuSource, (unsigned long long)s.printsMatched, (unsigned long long)s.printsUnknown,
            (unsigned long long)s.printsAmbiguous, (unsigned long long)s.sentinelsAttached, (unsigned long long)s.destroyed,
            (unsigned long long)s.untracked, (unsigned long long)s.indexTextures, (unsigned long long)s.indexPrints,
            s.disabled ? ", DISABLED: " : "", s.disabledReason.c_str());
}

void checkDirectStorage() {
    static bool logged = false;
    if (logged) return;
    const wchar_t* names[] = {L"dstorage.dll", L"dstoragecore.dll"};
    for (const wchar_t* n : names)
        if (GetModuleHandleW(n)) {
            logged = true;
            QS_INFO("gpu: DirectStorage loaded (%ls): uploads through its GPU decompression path are not seen by the copy "
                    "hook; watch 'gpu-only source' in the summary (docs/dev/gpu_swap.md)", n);
        }
}

HMODULE loadSystemDll(const wchar_t* name) {
    wchar_t dir[MAX_PATH];
    UINT n = GetSystemDirectoryW(dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return nullptr;
    return LoadLibraryW((std::wstring(dir) + L"\\" + name).c_str());
}

DWORD WINAPI workerMain(void*) {
    QS_INFO("gpu: worker started (MaxMBPerFrame %.2f, LogLevel %d, follow served stages %d, AssumeCommonState %d)",
            double(g_cfg.budgetBytesPerFrame) / 1048576.0, g_cfg.logLevel, int(g_cfg.followServed), int(g_cfg.assumeCommonState));
    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
        disableSwap("MH_Initialize failed");
        return 0;
    }
    HMODULE d3d12 = loadSystemDll(L"d3d12.dll");
    void* create = d3d12 ? reinterpret_cast<void*>(GetProcAddress(d3d12, "D3D12CreateDevice")) : nullptr;
    if (!create || !hookTarget(create, reinterpret_cast<void*>(&hkD3D12CreateDevice), reinterpret_cast<void**>(&oD3D12CreateDevice),
                               "d3d12!D3D12CreateDevice"))
        QS_INFO("gpu: D3D12CreateDevice hook not installed, relying on the overlay queue (late start)");
    overlaySetPresentCallback(&presentCallback);
    checkDirectStorage();
    if (g_cfg.preloadAllPacks) listPacks();
    log::flush();
    DWORD lastSummary = GetTickCount();
    uint64_t lastSeen = 0;
    bool deviceLost = false;
    while (WaitForSingleObject(g_stopEvent, 4) == WAIT_TIMEOUT && running()) {
        try {
            if (g_submitFailed.load()) {
                disableSwap("Signal on the game queue failed");
                break;
            }
            if (U.ready && U.dev->GetDeviceRemovedReason() != S_OK) {
                deviceLost = true;
                releaseUploader(false);
                disableSwap("D3D12 device removed");
                break;
            }
            finalizeBatch();
            refreshTargets();
            preloadOne();
            drainDirty();
            ensureUploader();
            if (U.ready && running()) recordBatch();
            g_gpuBusy.store(U.ready && U.fence && U.fence->GetCompletedValue() < U.fenceLast);
        } catch (const std::exception& e) {
            disableSwap(std::string("exception in the worker: ") + e.what());
        } catch (...) {
            disableSwap("exception in the worker");
        }
        const DWORD now = GetTickCount();
        if (now - lastSummary > 30000) {
            lastSummary = now;
            g_mbPerFrame.store(g_rate.sample(C.bytes.load(), g_frames.load()));
            checkDirectStorage();
            // throttled: only when something moved since the last summary
            const uint64_t seen = C.copiesSeen.load() + C.jobsQueued.load() + C.jobsDone.load() + C.jobsSkipped.load() +
                                  C.destroyed.load() + C.printsMatched.load();
            if (logOn(1) && seen != lastSeen) logSummary("periodic");
            lastSeen = seen;
            log::flush();
        }
    }
    // Off for the session after an error: give the memory back (our fence only, bounded wait).
    if (g_state.load() == int(State::Disabled) && !deviceLost && !g_stopping.load()) {
        overlaySetPresentCallback(nullptr);
        releaseUploader(true);
        logSummary("disabled");
        log::flush();
    }
    return 0;
}

}  // namespace

// ------------------------------------------------------------------ public API
bool gpuSwapStart(const GpuSwapConfig& cfg) {
    int expected = int(State::Stopped);
    if (!g_state.compare_exchange_strong(expected, int(State::Running))) return true;
    g_cfg = cfg;
    g_requestedStage = cfg.initialStage;
    g_lib = std::make_unique<StageLibrary>(&openPaz, nullptr, size_t(std::max(16, cfg.cacheMB)) << 20);
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_worker = g_stopEvent ? CreateThread(nullptr, 0, workerMain, nullptr, 0, nullptr) : nullptr;
    if (!g_worker) {
        disableSwap("cannot start the worker thread");
        return false;
    }
    return true;
}

void gpuSwapSetStage(const std::string& stageId) {
    std::lock_guard<std::mutex> lk(g_stageMutex);
    g_requestedStage = stageId;
}

GpuSwapStats gpuSwapStats() {
    GpuSwapStats s;
    s.started = g_state.load() != int(State::Stopped);
    s.hooksInstalled = g_listHooked.load();
    s.queueReady = g_queueReady.load();
    s.disabled = g_state.load() == int(State::Disabled);
    {
        std::lock_guard<std::mutex> lk(g_reasonMutex);
        s.disabledReason = g_disabledReason;
    }
    if (g_cfg.followServed) s.activeStage = "per texture";
    else {
        std::lock_guard<std::mutex> lk(g_stageMutex);
        s.activeStage = g_requestedStage.empty() ? "U" : g_requestedStage;
    }
    s.copiesSeen = C.copiesSeen;
    s.copiesCandidate = C.copiesCandidate;
    s.copiesGpuSource = C.copiesGpuSource;
    s.printsMatched = C.printsMatched;
    s.printsUnknown = C.printsUnknown;
    s.printsAmbiguous = C.printsAmbiguous;
    s.texToTexCopies = C.texToTex;
    s.barriersSeen = C.barriers;
    s.tracked = g_tracker.size();
    s.sentinelsAttached = C.sentinels;
    s.destroyed = C.destroyed;
    s.untracked = C.untracked;
    s.jobsQueued = C.jobsQueued;
    s.jobsDone = C.jobsDone;
    s.jobsSkipped = C.jobsSkipped;
    s.jobsFailed = C.jobsFailed;
    s.pending = g_sched.size();
    s.bytesUploaded = C.bytes;
    s.batches = C.batches;
    s.ticksWaitingGpu = C.waitingGpu;
    s.frames = g_frames.load();
    s.mbPerFrame = g_mbPerFrame.load();
    if (g_lib) {
        s.indexTextures = g_lib->index().textureCount();
        s.indexPrints = g_lib->index().printCount();
    }
    s.idle = g_targetsReady.load() && s.pending == 0 && g_batch.load() == kBatchNone && !g_gpuBusy.load();
    {
        std::lock_guard<std::mutex> lk(g_dirtyMutex);
        s.idle = s.idle && g_dirty.empty();
    }
    return s;
}

void gpuSwapStop() {
    if (g_state.load() == int(State::Stopped)) return;
    overlaySetPresentCallback(nullptr);  // returns when no callback runs any more
    if (g_stopEvent) SetEvent(g_stopEvent);
    if (g_worker) {
        WaitForSingleObject(g_worker, 5000);
        CloseHandle(g_worker);
        g_worker = nullptr;
    }
    g_state.store(int(State::Disabled));  // detours become pass-through
    {
        std::lock_guard<std::mutex> lk(g_hookMutex);
        for (void* t : g_hookTargets) MH_DisableHook(t);
    }
    for (int i = 0; i < 200 && g_inFlight.load() > 0; ++i) Sleep(10);
    logSummary("unload");
    releaseUploader(true);
    // Detach our sentinels: their code lives in this DLL, which is about to be unloaded.
    g_stopping.store(true);
    std::vector<uint64_t> keys;
    {
        std::lock_guard<std::mutex> lk(g_sentMutex);
        keys.assign(g_sentinels.begin(), g_sentinels.end());
    }
    size_t detached = 0;
    for (uint64_t k : keys) {
        auto* r = reinterpret_cast<ID3D12Resource*>(uintptr_t(k));
        bool alive = false;
        {
            std::lock_guard<std::mutex> lk(g_sentMutex);  // a firing sentinel erases under this lock
            if (g_sentinels.count(k)) alive = r->AddRef() > 1;
        }
        if (!alive) continue;
        r->SetPrivateDataInterface(kSentinelGuid, nullptr);
        r->Release();
        ++detached;
    }
    {
        std::lock_guard<std::mutex> lk(g_hookMutex);
        for (void* t : g_hookTargets) MH_RemoveHook(t);
        g_hookTargets.clear();
    }
    // MinHook itself is left initialised: io_hooks (or the overlay) owns it and uninitialises it last.
    if (g_stopEvent) CloseHandle(g_stopEvent);
    g_stopEvent = nullptr;
    g_lib.reset();
    QS_INFO("gpu: stopped (%zu sentinels detached)", detached);
}

}  // namespace qs

// ------------------------------------------------------------------ demo exports (tools/gpu_demo)
// Start without io packs: explicit stage control, packs and originals from the given folders.
extern "C" __declspec(dllexport) int QuattroStagioniGpuSwapStart(const wchar_t* gameRoot, const wchar_t* packsDir,
                                                                const char* stage) {
    qs::GpuSwapConfig c;
    c.gameRoot = gameRoot ? gameRoot : L"";
    c.packsDir = packsDir ? packsDir : L"";
    c.followServed = false;
    c.initialStage = stage ? stage : "";
    c.verifyPacks = false;              // synthetic game folder: no exe, no pamt
    c.budgetBytesPerFrame = 16u << 10;  // small on purpose: the demo checks that swaps span several frames
    c.assumeCommonState = true;         // the demo's textures stay in COMMON
    c.logLevel = 2;
    return qs::gpuSwapStart(c) ? 1 : 0;
}

extern "C" __declspec(dllexport) void QuattroStagioniGpuSwapSetStage(const char* stage) {
    qs::gpuSwapSetStage(stage ? stage : "");
}

// Writes a one-line summary into buf; returns 1 when the swap is idle (stage applied, nothing pending).
extern "C" __declspec(dllexport) int QuattroStagioniGpuSwapStats(char* buf, int size) {
    qs::GpuSwapStats s = qs::gpuSwapStats();
    if (buf && size > 0)
        snprintf(buf, size_t(size),
                 "hooks=%d queue=%d disabled=%d stage=%s copies=%llu candidates=%llu matched=%llu unknown=%llu ambiguous=%llu "
                 "tracked=%llu sentinels=%llu destroyed=%llu jobs=%llu/%llu skipped=%llu failed=%llu bytes=%llu batches=%llu "
                 "barriers=%llu index=%llu/%llu idle=%d",
                 int(s.hooksInstalled), int(s.queueReady), int(s.disabled), s.activeStage.c_str(),
                 (unsigned long long)s.copiesSeen, (unsigned long long)s.copiesCandidate, (unsigned long long)s.printsMatched,
                 (unsigned long long)s.printsUnknown, (unsigned long long)s.printsAmbiguous, (unsigned long long)s.tracked,
                 (unsigned long long)s.sentinelsAttached, (unsigned long long)s.destroyed, (unsigned long long)s.jobsDone,
                 (unsigned long long)s.jobsQueued, (unsigned long long)s.jobsSkipped, (unsigned long long)s.jobsFailed,
                 (unsigned long long)s.bytesUploaded, (unsigned long long)s.batches, (unsigned long long)s.barriersSeen,
                 (unsigned long long)s.indexTextures, (unsigned long long)s.indexPrints, int(s.idle));
    return s.idle ? 1 : 0;
}

// Stops the module (demo: checks that the sentinels detach cleanly).
extern "C" __declspec(dllexport) void QuattroStagioniGpuSwapStop() { qs::gpuSwapStop(); }
