// QuattroStagioni - D3D12 overlay: swapchain hooks, ImGui renderer, input capture for the F5 menu.
//
// Why it is built this way:
//   - Only system DLL code is hooked (dxgi.dll, the D3D12 runtime, user32). The game exe is Denuvo
//     protected and never touched. dxgi.dll / d3d12.dll are loaded by FULL system path, so a local
//     proxy (ReShade as dxgi.dll, Streamline's sl.interposer) never gives us its wrapper objects: we
//     always hook the real DXGI code, which every proxy ends up calling exactly once per real present.
//   - Main path: IDXGIFactory::CreateSwapChain / IDXGIFactory2::CreateSwapChainForHwnd are hooked
//     from a dummy factory (no device needed). When the game (or its proxy) creates the real
//     swapchain we learn its command queue (the pDevice argument) and hook Present / Present1 /
//     ResizeBuffers / ResizeBuffers1 from that real object's vtable.
//   - Late start fallback (swapchain created before our hooks): after a delay we create a dummy
//     device + swapchain only to read the vtables, and capture the game's DIRECT queue by hooking
//     ID3D12CommandQueue::ExecuteCommandLists (installed lazily, only if the queue is unknown).
//   - Rendering happens inside the real Present, on the presenting thread, with our own descriptor
//     heaps, per-backbuffer command allocators and a fence, submitted on the queue that owns the
//     swapchain. Nothing is drawn (no command list at all) while the menu and the toast are hidden.
//   - Input: F5 is polled with GetAsyncKeyState (edge, game window focused). While the menu is open
//     the window procedure is subclassed (fed to ImGui, game input blocked) and a few user32 input
//     functions (raw input, GetAsyncKeyState, SetCursorPos, ClipCursor) are neutralised for the game.
//   - Any failure logs and disables the overlay; device removal, resize, fullscreen toggles and
//     swapchain recreation release our references first so the game's own calls always succeed.
#include <windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "MinHook.h"
#include "imgui.h"
#include "imgui_impl_dx12.h"
#include "imgui_impl_win32.h"
#include "log.h"
#include "menu.h"
#include "overlay.h"
#include "qs_state.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace qs {
namespace {

// ------------------------------------------------------------------ vtable slots (COM ABI, stable)
// IDXGIFactory / IDXGIFactory2
constexpr int kVtFactoryCreateSwapChain = 10;
constexpr int kVtFactory2CreateSwapChainForHwnd = 15;
// IDXGISwapChain / 1 / 3
constexpr int kVtScPresent = 8;
constexpr int kVtScResizeBuffers = 13;
constexpr int kVtSc1Present1 = 22;
constexpr int kVtSc3SetColorSpace1 = 38;
constexpr int kVtSc3ResizeBuffers1 = 39;
// ID3D12CommandQueue
constexpr int kVtQueueExecuteCommandLists = 10;

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using ResizeBuffers1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT,
                                                     const UINT*, IUnknown* const*);
using CreateSwapChainFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using CreateSwapChainForHwndFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
                                                             const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*,
                                                             IDXGISwapChain1**);
using SetColorSpace1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, DXGI_COLOR_SPACE_TYPE);
using ExecuteCommandListsFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
using GetRawInputDataFn = UINT(WINAPI*)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
using GetRawInputBufferFn = UINT(WINAPI*)(PRAWINPUT, PUINT, UINT);
using GetAsyncKeyStateFn = SHORT(WINAPI*)(int);
using SetCursorPosFn = BOOL(WINAPI*)(int, int);
using ClipCursorFn = BOOL(WINAPI*)(const RECT*);
using CreateDXGIFactory1Fn = HRESULT(WINAPI*)(REFIID, void**);
using D3D12CreateDeviceFn = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);

PresentFn oPresent = nullptr;
Present1Fn oPresent1 = nullptr;
ResizeBuffersFn oResizeBuffers = nullptr;
ResizeBuffers1Fn oResizeBuffers1 = nullptr;
CreateSwapChainFn oCreateSwapChain = nullptr;
CreateSwapChainForHwndFn oCreateSwapChainForHwnd = nullptr;
SetColorSpace1Fn oSetColorSpace1 = nullptr;
ExecuteCommandListsFn oExecuteCommandLists = nullptr;
GetRawInputDataFn oGetRawInputData = nullptr;
GetRawInputBufferFn oGetRawInputBuffer = nullptr;
GetAsyncKeyStateFn oGetAsyncKeyState = nullptr;
SetCursorPosFn oSetCursorPos = nullptr;
ClipCursorFn oClipCursor = nullptr;

// ------------------------------------------------------------------ global state
enum class State : int { Stopped = 0, Starting, Running, Disabled };
std::atomic<int> g_state{int(State::Stopped)};
std::atomic<int> g_inFlight{0};         // detours currently executing (overlayStop waits for 0)
std::atomic<bool> g_swapchainSeen{false};
std::atomic<int> g_toggleVk{VK_F5};
std::atomic<bool> g_menuOpen{false};     // input capture active (mirrors shared().menuOpen)
HANDLE g_worker = nullptr;
HANDLE g_stopEvent = nullptr;
bool g_weInitialisedMinHook = false;
IDXGIFactory2* g_factory = nullptr;  // from hookFactory(), reused by hookFromDummy()

std::mutex g_hookMutex;
std::vector<void*> g_hookTargets;

// swapchain -> command queue, learned in the creation hooks (raw pointers, compared only)
SRWLOCK g_mapLock = SRWLOCK_INIT;
std::unordered_map<void*, ID3D12CommandQueue*> g_queueBySwapchain;
thread_local bool t_creatingDummy = false;
// colour space set by the game on each swapchain (HDR10 needs PQ-encoded UI colours)
std::unordered_map<void*, DXGI_COLOR_SPACE_TYPE> g_colorSpaceBySwapchain;

// fallback queue capture (ExecuteCommandLists)
std::atomic<ID3D12CommandQueue*> g_lastDirectQueue{nullptr};
std::atomic<ID3D12CommandQueue*> g_presentThreadQueue{nullptr};
std::atomic<DWORD> g_presentThreadId{0};
thread_local bool t_ourSubmit = false;
thread_local bool t_inPresent = false;
thread_local bool t_bypassInput = false;  // our own ImGui code may call the hooked user32 functions

SRWLOCK g_renderLock = SRWLOCK_INIT;  // renderer state (Present, ResizeBuffers, creation hooks)
SRWLOCK g_imguiLock = SRWLOCK_INIT;   // ImGui context (render thread vs window thread)

struct FrameCtx {
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12Resource* backbuffer = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
    UINT64 fenceValue = 0;
};

struct Renderer {
    IDXGISwapChain* sc = nullptr;     // not owned (holding a reference would keep a dead swapchain alive)
    IDXGISwapChain3* sc3 = nullptr;   // not owned, same object
    ID3D12Device* device = nullptr;   // owned
    ID3D12CommandQueue* queue = nullptr;  // owned
    HWND hwnd = nullptr;
    UINT bufferCount = 0, width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    ID3D12DescriptorHeap* rtvHeap = nullptr;
    std::vector<FrameCtx> frames;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE fenceEvent = nullptr;
    UINT64 fenceLast = 0;
    bool buffersReady = false;
    bool dx12Ready = false;
    DXGI_FORMAT dx12Format = DXGI_FORMAT_UNKNOWN;
    bool win32Ready = false;
    DWORD lastPresentTick = 0;
    DWORD boundTick = 0;
};
Renderer R;
IDXGISwapChain* g_rejectedSc = nullptr;  // last swapchain that cannot be ours (D3D11, no queue...)
int g_failures = 0;
bool g_imguiCreated = false;
float g_uiScale = 0.0f;
bool g_keyWasDown = false;
DWORD g_menuOpenTick = 0;
bool g_menuVisible = false;  // drawn (stays true during fade-out)
DWORD g_menuCloseTick = 0;
int g_queueWaitFrames = 0;
LONG_PTR g_prevWndProc = 0;
HWND g_subclassedHwnd = nullptr;

// SRV descriptor heap for ImGui textures (1.92 backends allocate one descriptor per texture).
struct SrvAllocator {
    ID3D12DescriptorHeap* heap = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
    UINT inc = 0;
    std::vector<int> freeList;
} g_srv;
constexpr int kSrvCount = 64;

struct InFlight {
    InFlight() { g_inFlight.fetch_add(1); }
    ~InFlight() { g_inFlight.fetch_sub(1); }
};

bool running() { return g_state.load() == int(State::Running); }

template <class T>
void safeRelease(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

void** vtableOf(void* obj) { return *reinterpret_cast<void***>(obj); }

// ------------------------------------------------------------------ hook helpers
bool hookTarget(void* target, void* detour, void** original, const char* name) {
    std::lock_guard<std::mutex> lk(g_hookMutex);
    for (void* t : g_hookTargets)
        if (t == target) return true;  // already hooked (same code shared by every object)
    MH_STATUS st = MH_CreateHook(target, detour, original);
    if (st != MH_OK) {
        QS_ERR("overlay: MH_CreateHook(%s @%p) failed: %s", name, target, MH_StatusToString(st));
        return false;
    }
    st = MH_EnableHook(target);
    if (st != MH_OK) {
        QS_ERR("overlay: MH_EnableHook(%s) failed: %s", name, MH_StatusToString(st));
        MH_RemoveHook(target);
        return false;
    }
    g_hookTargets.push_back(target);
    QS_INFO("overlay: hooked %s @%p", name, target);
    return true;
}

HMODULE loadSystemDll(const wchar_t* name) {
    wchar_t dir[MAX_PATH];
    UINT n = GetSystemDirectoryW(dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return nullptr;
    std::wstring path = std::wstring(dir) + L"\\" + name;
    return LoadLibraryW(path.c_str());  // kept loaded: our hooks live in it
}

void disableOverlay(const char* why);
void logCoexistingModules(const char* when);
LRESULT CALLBACK hkWndProc(HWND, UINT, WPARAM, LPARAM);
void STDMETHODCALLTYPE hkExecuteCommandLists(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

// ------------------------------------------------------------------ fences and teardown
void waitGpuIdle() {
    if (!R.fence || !R.queue || !R.fenceEvent) return;
    if (R.fence->GetCompletedValue() >= R.fenceLast) return;
    if (FAILED(R.fence->SetEventOnCompletion(R.fenceLast, R.fenceEvent))) return;
    if (WaitForSingleObject(R.fenceEvent, 2000) == WAIT_TIMEOUT)
        QS_ERR("overlay: GPU wait timed out (fence %llu, completed %llu)", (unsigned long long)R.fenceLast,
               (unsigned long long)R.fence->GetCompletedValue());
}

void releaseBuffers(bool wait) {
    if (wait) waitGpuIdle();
    for (FrameCtx& f : R.frames) safeRelease(f.backbuffer);
    R.buffersReady = false;
}

void restoreWndProc() {
    if (!g_subclassedHwnd) return;
    // Only restore if nobody subclassed after us (Steam overlay, ReShade...); otherwise our procedure
    // stays in the chain in pass-through mode, which is harmless.
    if (GetWindowLongPtrW(g_subclassedHwnd, GWLP_WNDPROC) == reinterpret_cast<LONG_PTR>(&hkWndProc)) {
        SetWindowLongPtrW(g_subclassedHwnd, GWLP_WNDPROC, g_prevWndProc);
        g_subclassedHwnd = nullptr;
        g_prevWndProc = 0;
    }
}

void setMenuOpen(bool open);

// Releases every GPU object (render lock held). `wait` = false after device removal.
void teardown(bool wait) {
    if (!R.sc && !R.device) return;
    if (g_menuOpen.load()) setMenuOpen(false);
    restoreWndProc();
    releaseBuffers(wait);
    AcquireSRWLockExclusive(&g_imguiLock);
    if (R.dx12Ready) ImGui_ImplDX12_Shutdown();
    if (R.win32Ready) ImGui_ImplWin32_Shutdown();
    ReleaseSRWLockExclusive(&g_imguiLock);
    for (FrameCtx& f : R.frames) safeRelease(f.alloc);
    safeRelease(R.list);
    safeRelease(R.fence);
    if (R.fenceEvent) CloseHandle(R.fenceEvent);
    safeRelease(R.rtvHeap);
    safeRelease(g_srv.heap);
    g_srv.freeList.clear();
    safeRelease(R.queue);
    safeRelease(R.device);
    R = Renderer();
    g_menuVisible = false;
}

void disableOverlay(const char* why) {
    QS_ERR("overlay disabled: %s (the game is not affected)", why);
    g_state.store(int(State::Disabled));
    qs::shared().menuOpen.store(false);
}

// ------------------------------------------------------------------ ImGui backend plumbing
void srvAlloc(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu) {
    if (g_srv.freeList.empty()) {  // should never happen with 64 slots (font atlas + a few)
        QS_ERR("overlay: SRV heap exhausted");
        *cpu = g_srv.cpu;
        *gpu = g_srv.gpu;
        return;
    }
    int i = g_srv.freeList.back();
    g_srv.freeList.pop_back();
    cpu->ptr = g_srv.cpu.ptr + SIZE_T(i) * g_srv.inc;
    gpu->ptr = g_srv.gpu.ptr + UINT64(i) * g_srv.inc;
}

void srvFree(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE) {
    if (!g_srv.inc) return;
    g_srv.freeList.push_back(int((cpu.ptr - g_srv.cpu.ptr) / g_srv.inc));
}

void ensureImGuiContext() {
    if (g_imguiCreated) return;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // never write imgui.ini into the game folder
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    menu::loadFonts();
    g_imguiCreated = true;
}

bool createBuffers() {
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(R.sc->GetDesc(&desc))) return false;
    if (desc.BufferCount != R.bufferCount) {  // ResizeBuffers may change the count: rebuild per-frame objects
        waitGpuIdle();
        for (FrameCtx& f : R.frames) safeRelease(f.alloc);
        safeRelease(R.rtvHeap);
        R.frames.assign(desc.BufferCount, FrameCtx());
        R.bufferCount = desc.BufferCount;
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = R.bufferCount;
        if (FAILED(R.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&R.rtvHeap)))) return false;
        for (FrameCtx& f : R.frames)
            if (FAILED(R.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&f.alloc))))
                return false;
        if (R.dx12Ready) {  // NumFramesInFlight changed: the backend must be re-created
            ImGui_ImplDX12_Shutdown();
            R.dx12Ready = false;
        }
    }
    R.format = desc.BufferDesc.Format;
    R.width = desc.BufferDesc.Width;
    R.height = desc.BufferDesc.Height;
    R.hwnd = desc.OutputWindow;
    UINT inc = R.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE h = R.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < R.bufferCount; ++i) {
        FrameCtx& f = R.frames[i];
        if (FAILED(R.sc->GetBuffer(i, IID_PPV_ARGS(&f.backbuffer)))) return false;
        D3D12_RENDER_TARGET_VIEW_DESC rd{};
        rd.Format = R.format;
        rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        f.rtv.ptr = h.ptr + SIZE_T(i) * inc;
        R.device->CreateRenderTargetView(f.backbuffer, &rd, f.rtv);
    }
    if (R.dx12Ready && R.dx12Format != R.format) {  // format changed (HDR toggle): new PSO needed
        ImGui_ImplDX12_Shutdown();
        R.dx12Ready = false;
    }
    if (!R.dx12Ready) {
        QS_DBG("overlay: ImGui DX12 init");
        AcquireSRWLockExclusive(&g_imguiLock);
        ensureImGuiContext();
        ReleaseSRWLockExclusive(&g_imguiLock);
        ImGui_ImplDX12_InitInfo ii;
        ii.Device = R.device;
        ii.CommandQueue = R.queue;
        ii.NumFramesInFlight = int(R.bufferCount);
        ii.RTVFormat = R.format;
        ii.DSVFormat = DXGI_FORMAT_UNKNOWN;
        ii.SrvDescriptorHeap = g_srv.heap;
        ii.SrvDescriptorAllocFn = &srvAlloc;
        ii.SrvDescriptorFreeFn = &srvFree;
        AcquireSRWLockExclusive(&g_imguiLock);
        bool ok = ImGui_ImplDX12_Init(&ii);
        ReleaseSRWLockExclusive(&g_imguiLock);
        if (!ok) {
            QS_ERR("overlay: ImGui_ImplDX12_Init failed (format %d)", int(R.format));
            return false;
        }
        R.dx12Ready = true;
        R.dx12Format = R.format;
    }
    R.buffersReady = true;
    QS_INFO("overlay: backbuffers ready %ux%u, %u buffers, format %d, hwnd %p", R.width, R.height, R.bufferCount,
            int(R.format), (void*)R.hwnd);
    return true;
}

void ensureQueueCaptureHook(ID3D12Device* device) {
    if (oExecuteCommandLists) return;
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* tmp = nullptr;
    if (FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&tmp)))) return;
    void* target = vtableOf(tmp)[kVtQueueExecuteCommandLists];
    tmp->Release();
    hookTarget(target, reinterpret_cast<void*>(&hkExecuteCommandLists), reinterpret_cast<void**>(&oExecuteCommandLists),
               "ID3D12CommandQueue::ExecuteCommandLists");
}

ID3D12CommandQueue* findQueue(IDXGISwapChain* sc, ID3D12Device* device) {
    ID3D12CommandQueue* q = nullptr;
    AcquireSRWLockShared(&g_mapLock);
    auto it = g_queueBySwapchain.find(sc);
    if (it != g_queueBySwapchain.end()) q = it->second;
    ReleaseSRWLockShared(&g_mapLock);
    if (q) return q;
    // Fallback: a DIRECT queue seen in ExecuteCommandLists, preferring the presenting thread's one.
    ensureQueueCaptureHook(device);
    ID3D12CommandQueue* cands[2] = {g_presentThreadQueue.load(), g_lastDirectQueue.load()};
    for (ID3D12CommandQueue* c : cands) {
        if (!c) continue;
        ID3D12Device* d = nullptr;
        if (SUCCEEDED(c->GetDevice(IID_PPV_ARGS(&d)))) {
            bool same = d == device;
            d->Release();
            if (same) return c;
        }
    }
    return nullptr;
}

// Binds the renderer to `sc` (render lock held). Returns false if this swapchain is not usable.
bool bind(IDXGISwapChain* sc) {
    QS_DBG("overlay: binding swapchain %p", (void*)sc);
    ID3D12Device* device = nullptr;
    if (FAILED(sc->GetDevice(IID_PPV_ARGS(&device)))) {
        QS_DBG("overlay: swapchain %p is not D3D12, ignored", (void*)sc);
        g_rejectedSc = sc;
        return false;
    }
    ID3D12CommandQueue* queue = findQueue(sc, device);
    if (!queue) {
        if (g_queueWaitFrames++ == 0) QS_INFO("overlay: waiting for the game's command queue (fallback capture)");
        if (g_queueWaitFrames > 600) {
            device->Release();
            disableOverlay("command queue of the swapchain not found");
            return false;
        }
        device->Release();
        return false;
    }
    IDXGISwapChain3* sc3 = nullptr;
    if (FAILED(sc->QueryInterface(IID_PPV_ARGS(&sc3)))) {
        device->Release();
        g_rejectedSc = sc;
        QS_ERR("overlay: swapchain without IDXGISwapChain3, ignored");
        return false;
    }
    sc3->Release();  // keep the raw pointer only (same object as sc)
    queue->AddRef();
    R.sc = sc;
    R.sc3 = sc3;
    R.device = device;
    R.queue = queue;
    R.boundTick = GetTickCount();

    bool ok = SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&R.fence)));
    R.fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ok = ok && R.fenceEvent;
    if (ok) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = kSrvCount;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ok = SUCCEEDED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_srv.heap)));
        if (ok) {
            g_srv.cpu = g_srv.heap->GetCPUDescriptorHandleForHeapStart();
            g_srv.gpu = g_srv.heap->GetGPUDescriptorHandleForHeapStart();
            g_srv.inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            g_srv.freeList.clear();
            for (int i = kSrvCount - 1; i >= 0; --i) g_srv.freeList.push_back(i);
        }
    }
    QS_DBG("overlay: fence/heap %d", int(ok));
    ok = ok && createBuffers();
    QS_DBG("overlay: buffers %d", int(ok));
    if (ok)
        ok = SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, R.frames[0].alloc, nullptr,
                                                 IID_PPV_ARGS(&R.list))) &&
             SUCCEEDED(R.list->Close());
    if (ok) {
        AcquireSRWLockExclusive(&g_imguiLock);
        ensureImGuiContext();
        R.win32Ready = ImGui_ImplWin32_Init(R.hwnd);
        ReleaseSRWLockExclusive(&g_imguiLock);
        ok = R.win32Ready;
    }
    if (!ok) {
        QS_ERR("overlay: renderer setup failed for swapchain %p", (void*)sc);
        teardown(true);
        g_rejectedSc = sc;
        if (++g_failures >= 3) disableOverlay("renderer setup failed 3 times");
        return false;
    }
    QS_INFO("overlay: bound to swapchain %p (queue %p, thread %lu)", (void*)sc, (void*)queue, GetCurrentThreadId());
    logCoexistingModules("bind");
    return true;
}

// ------------------------------------------------------------------ input
LRESULT CALLBACK hkWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    InFlight g;
    WNDPROC prev = reinterpret_cast<WNDPROC>(g_prevWndProc);
    if (!g_menuOpen.load() || !prev) return prev ? CallWindowProcW(prev, hwnd, msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
    // Feed ImGui. TryAcquire: never block the window thread (and never deadlock if a message arrives
    // re-entrantly while the render path holds the lock); a dropped event is harmless.
    if (TryAcquireSRWLockExclusive(&g_imguiLock)) {
        t_bypassInput = true;
        if (ImGui::GetCurrentContext()) ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp);
        t_bypassInput = false;
        ReleaseSRWLockExclusive(&g_imguiLock);
    }
    switch (msg) {
        case WM_INPUT:
            return DefWindowProcW(hwnd, msg, wp, lp);  // required cleanup, the game does not see it
        case WM_KEYUP:
        case WM_SYSKEYUP:
        case WM_LBUTTONUP:
        case WM_RBUTTONUP:
        case WM_MBUTTONUP:
        case WM_XBUTTONUP:
            break;  // releases reach the game, so no key stays stuck after the menu closes
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                SetCursor(nullptr);  // ImGui draws its own cursor
                return TRUE;
            }
            break;
        default:
            if ((msg >= WM_KEYFIRST && msg <= WM_KEYLAST) || (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) ||
                msg == WM_MOUSEHOVER || msg == WM_MOUSELEAVE)
                return 0;
            break;
    }
    return CallWindowProcW(prev, hwnd, msg, wp, lp);
}

void neutraliseRawInput(RAWINPUT* ri) {
    if (ri->header.dwType == RIM_TYPEMOUSE) {
        ri->data.mouse.lLastX = 0;
        ri->data.mouse.lLastY = 0;
        // keep button releases, drop presses and wheel
        USHORT keep = ri->data.mouse.usButtonFlags &
                      (RI_MOUSE_LEFT_BUTTON_UP | RI_MOUSE_RIGHT_BUTTON_UP | RI_MOUSE_MIDDLE_BUTTON_UP |
                       RI_MOUSE_BUTTON_4_UP | RI_MOUSE_BUTTON_5_UP);
        ri->data.mouse.usButtonFlags = keep;
        ri->data.mouse.usButtonData = 0;
    } else if (ri->header.dwType == RIM_TYPEKEYBOARD) {
        if (!(ri->data.keyboard.Flags & RI_KEY_BREAK)) {  // key down: turn into an ignorable key
            ri->data.keyboard.VKey = 0xFF;
            ri->data.keyboard.MakeCode = 0;
        }
    }
}

UINT WINAPI hkGetRawInputData(HRAWINPUT h, UINT cmd, LPVOID data, PUINT size, UINT hdr) {
    InFlight g;
    UINT r = oGetRawInputData(h, cmd, data, size, hdr);
    if (g_menuOpen.load() && !t_bypassInput && cmd == RID_INPUT && data && r != UINT(-1) && r >= sizeof(RAWINPUTHEADER))
        neutraliseRawInput(static_cast<RAWINPUT*>(data));
    return r;
}

UINT WINAPI hkGetRawInputBuffer(PRAWINPUT data, PUINT size, UINT hdr) {
    InFlight g;
    UINT r = oGetRawInputBuffer(data, size, hdr);
    if (g_menuOpen.load() && !t_bypassInput && data && r != UINT(-1)) {
        RAWINPUT* ri = data;
        for (UINT i = 0; i < r; ++i) {
            neutraliseRawInput(ri);
            // NEXTRAWINPUTBLOCK, spelled out (the mingw macro uses an undefined QWORD); x64 = 8-byte aligned
            ri = reinterpret_cast<RAWINPUT*>((reinterpret_cast<ULONG_PTR>(ri) + ri->header.dwSize + 7) & ~ULONG_PTR(7));
        }
    }
    return r;
}

SHORT WINAPI hkGetAsyncKeyState(int vk) {
    InFlight g;
    if (g_menuOpen.load() && !t_bypassInput) return 0;
    return oGetAsyncKeyState(vk);
}

BOOL WINAPI hkSetCursorPos(int x, int y) {
    InFlight g;
    if (g_menuOpen.load() && !t_bypassInput) return TRUE;  // the game must not re-centre the cursor
    return oSetCursorPos(x, y);
}

BOOL WINAPI hkClipCursor(const RECT* rc) {
    InFlight g;
    if (g_menuOpen.load() && !t_bypassInput) return oClipCursor(nullptr);
    return oClipCursor(rc);
}

void installInputHooks() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) user32 = LoadLibraryW(L"user32.dll");
    if (!user32) return;
    struct Spec {
        const char* name;
        void* detour;
        void** original;
    } specs[] = {
        {"GetRawInputData", reinterpret_cast<void*>(&hkGetRawInputData), reinterpret_cast<void**>(&oGetRawInputData)},
        {"GetRawInputBuffer", reinterpret_cast<void*>(&hkGetRawInputBuffer), reinterpret_cast<void**>(&oGetRawInputBuffer)},
        {"GetAsyncKeyState", reinterpret_cast<void*>(&hkGetAsyncKeyState), reinterpret_cast<void**>(&oGetAsyncKeyState)},
        {"SetCursorPos", reinterpret_cast<void*>(&hkSetCursorPos), reinterpret_cast<void**>(&oSetCursorPos)},
        {"ClipCursor", reinterpret_cast<void*>(&hkClipCursor), reinterpret_cast<void**>(&oClipCursor)},
    };
    for (Spec& s : specs) {
        void* p = reinterpret_cast<void*>(GetProcAddress(user32, s.name));
        if (p) hookTarget(p, s.detour, s.original, s.name);
    }
    // TODO: DirectInput8 / XInput are not blocked yet (the game seems to use raw input for mouse/keyboard).
}

SHORT asyncKey(int vk) { return oGetAsyncKeyState ? oGetAsyncKeyState(vk) : GetAsyncKeyState(vk); }

void setMenuOpen(bool open) {
    if (open == g_menuOpen.load()) return;
    if (open) {
        if (!g_subclassedHwnd && R.hwnd) {
            g_prevWndProc = SetWindowLongPtrW(R.hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&hkWndProc));
            if (g_prevWndProc) g_subclassedHwnd = R.hwnd;
            else QS_ERR("overlay: window subclass failed (%lu), menu input may reach the game", GetLastError());
        }
        g_menuOpen.store(true);
        if (oClipCursor) oClipCursor(nullptr);
        g_menuOpenTick = GetTickCount();
        g_menuVisible = true;
    } else {
        g_menuOpen.store(false);
        restoreWndProc();
        g_menuCloseTick = GetTickCount();
    }
    qs::shared().menuOpen.store(open);
    if (ImGui::GetCurrentContext()) ImGui::GetIO().MouseDrawCursor = open;
    QS_DBG("overlay: menu %s", open ? "open" : "closed");
}

// ------------------------------------------------------------------ HDR backbuffers
// ImGui colours are sRGB. On an scRGB (R16G16B16A16_FLOAT, linear) backbuffer they are linearised,
// on an HDR10 backbuffer (R10G10B10A2 + ST.2084 colour space) converted to Rec.2020 and PQ-encoded at
// a fixed UI brightness. Done on the vertex colours (texture is white + alpha), so the backend's
// shaders stay unchanged. Blending happens in the encoded space: slightly different edges, readable.
enum class ColorMode { Srgb, Linear, Pq };
constexpr float kHdrUiNits = 203.0f;  // ITU-R BT.2408 reference white

float srgbToLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }

float pqEncode(float nits) {
    const float m1 = 0.1593017578125f, m2 = 78.84375f, c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
    float y = std::pow(std::max(0.0f, nits) / 10000.0f, m1);
    return std::pow((c1 + c2 * y) / (1.0f + c3 * y), m2);
}

ImU32 encodeColor(ImU32 col, ColorMode mode) {
    float r = srgbToLinear(float((col >> IM_COL32_R_SHIFT) & 0xFF) / 255.0f);
    float g = srgbToLinear(float((col >> IM_COL32_G_SHIFT) & 0xFF) / 255.0f);
    float b = srgbToLinear(float((col >> IM_COL32_B_SHIFT) & 0xFF) / 255.0f);
    if (mode == ColorMode::Pq) {  // Rec.709 -> Rec.2020 primaries, then PQ
        float r2 = 0.6274f * r + 0.3293f * g + 0.0433f * b;
        float g2 = 0.0691f * r + 0.9195f * g + 0.0114f * b;
        float b2 = 0.0164f * r + 0.0880f * g + 0.8956f * b;
        r = pqEncode(r2 * kHdrUiNits);
        g = pqEncode(g2 * kHdrUiNits);
        b = pqEncode(b2 * kHdrUiNits);
    }
    auto q = [](float v) { return ImU32(std::lround(std::max(0.0f, std::min(1.0f, v)) * 255.0f)); };
    return (col & IM_COL32_A_MASK) | (q(r) << IM_COL32_R_SHIFT) | (q(g) << IM_COL32_G_SHIFT) | (q(b) << IM_COL32_B_SHIFT);
}

ColorMode colorModeFor(void* sc, void* sc3, DXGI_FORMAT fmt) {
    if (fmt == DXGI_FORMAT_R16G16B16A16_FLOAT) return ColorMode::Linear;
    DXGI_COLOR_SPACE_TYPE cs = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    AcquireSRWLockShared(&g_mapLock);
    auto it = g_colorSpaceBySwapchain.find(sc3);  // the hook sees the IDXGISwapChain3 pointer
    if (it == g_colorSpaceBySwapchain.end()) it = g_colorSpaceBySwapchain.find(sc);
    if (it != g_colorSpaceBySwapchain.end()) cs = it->second;
    ReleaseSRWLockShared(&g_mapLock);
    if (fmt == DXGI_FORMAT_R10G10B10A2_UNORM && cs == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) return ColorMode::Pq;
    return ColorMode::Srgb;
}

void encodeDrawData(ImDrawData* dd, ColorMode mode) {
    if (mode == ColorMode::Srgb) return;
    ImU32 lastIn = 0, lastOut = encodeColor(0, mode);
    for (ImDrawList* l : dd->CmdLists)
        for (ImDrawVert& v : l->VtxBuffer) {
            if (v.col != lastIn) {
                lastIn = v.col;
                lastOut = encodeColor(v.col, mode);
            }
            v.col = lastOut;
        }
}

// ------------------------------------------------------------------ per-frame rendering
constexpr DWORD kToastMs = 9000, kFadeMs = 160;

void renderFrame() {
    const DWORD now = GetTickCount();
    const DWORD sinceBind = now - R.boundTick;
    const bool toast = sinceBind < kToastMs;
    float fade = 1.0f;
    if (g_menuOpen.load()) {
        fade = std::min(1.0f, float(now - g_menuOpenTick) / kFadeMs);
    } else if (g_menuVisible) {
        fade = 1.0f - float(now - g_menuCloseTick) / kFadeMs;
        if (fade <= 0.0f) g_menuVisible = false;
    }
    if (!g_menuVisible && !toast) return;  // nothing to draw: no ImGui frame, no command list

    // CPU side: build the ImGui frame (ImGui lock held, no blocking calls inside).
    AcquireSRWLockExclusive(&g_imguiLock);
    t_bypassInput = true;
    RECT rc{};
    GetClientRect(R.hwnd, &rc);
    float cw = float(rc.right - rc.left), ch = float(rc.bottom - rc.top);
    if (cw < 1 || ch < 1) {
        cw = float(R.width);
        ch = float(R.height);
    }
    float scale = std::max(0.75f, std::min(3.0f, ch / 1080.0f));
    if (std::fabs(scale - g_uiScale) > 0.01f) {
        menu::applyStyle(scale);
        g_uiScale = scale;
    }
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(cw, ch);
    io.DisplayFramebufferScale = ImVec2(float(R.width) / cw, float(R.height) / ch);
    io.MouseDrawCursor = g_menuOpen.load();
    ImGui::NewFrame();
    if (g_menuVisible) {
        bool open = true;
        menu::draw(&open, std::max(0.0f, fade));
        if (g_menuOpen.load() && (!open || ImGui::IsKeyPressed(ImGuiKey_Escape, false))) setMenuOpen(false);
    }
    if (toast && !g_menuOpen.load()) {
        float a = 1.0f;
        if (sinceBind < 400) a = sinceBind / 400.0f;
        if (sinceBind > kToastMs - 800) a = (kToastMs - sinceBind) / 800.0f;
        char key[32];
        int vk = g_toggleVk.load();
        if (vk >= VK_F1 && vk <= VK_F24) snprintf(key, sizeof key, "F%d", vk - VK_F1 + 1);
        else snprintf(key, sizeof key, "0x%02X", vk);
        menu::drawToast(a, key);
    }
    ImGui::Render();
    ImDrawData* dd = ImGui::GetDrawData();
    if (dd) {
        ColorMode cm = colorModeFor(R.sc, R.sc3, R.format);
        static int loggedMode = -1;
        if (int(cm) != loggedMode) {
            loggedMode = int(cm);
            QS_INFO("overlay: colour mode %s (format %d)", cm == ColorMode::Pq ? "HDR10 PQ" : cm == ColorMode::Linear ? "scRGB linear" : "sRGB", int(R.format));
        }
        encodeDrawData(dd, cm);
    }

    // GPU side.
    bool submitted = false;
    UINT idx = R.sc3->GetCurrentBackBufferIndex();
    if (idx < R.frames.size() && dd && dd->CmdListsCount > 0) {
        FrameCtx& f = R.frames[idx];
        bool ready = true;
        if (R.fence->GetCompletedValue() < f.fenceValue) {
            R.fence->SetEventOnCompletion(f.fenceValue, R.fenceEvent);
            ready = WaitForSingleObject(R.fenceEvent, 1000) == WAIT_OBJECT_0;
        }
        if (ready && SUCCEEDED(f.alloc->Reset()) && SUCCEEDED(R.list->Reset(f.alloc, nullptr))) {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = f.backbuffer;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            R.list->ResourceBarrier(1, &b);
            R.list->OMSetRenderTargets(1, &f.rtv, FALSE, nullptr);
            R.list->SetDescriptorHeaps(1, &g_srv.heap);
            ImGui_ImplDX12_RenderDrawData(dd, R.list);
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
            R.list->ResourceBarrier(1, &b);
            if (SUCCEEDED(R.list->Close())) {
                ID3D12CommandList* lists[] = {R.list};
                t_ourSubmit = true;
                R.queue->ExecuteCommandLists(1, lists);
                t_ourSubmit = false;
                R.queue->Signal(R.fence, ++R.fenceLast);
                f.fenceValue = R.fenceLast;
                submitted = true;
            }
        }
    }
    t_bypassInput = false;
    ReleaseSRWLockExclusive(&g_imguiLock);
    (void)submitted;
}

void onPresent(IDXGISwapChain* sc) {
    if (!running()) return;
    g_presentThreadId.store(GetCurrentThreadId());
    AcquireSRWLockExclusive(&g_renderLock);
    const DWORD now = GetTickCount();
    if (R.sc && R.sc != sc) {
        // Another swapchain presents. Keep ours unless it has been silent for 2 s (destroyed/replaced).
        if (now - R.lastPresentTick < 2000) {
            ReleaseSRWLockExclusive(&g_renderLock);
            return;
        }
        QS_INFO("overlay: swapchain %p silent, switching to %p", (void*)R.sc, (void*)sc);
        teardown(true);
    }
    if (!R.sc) {
        if (sc == g_rejectedSc || !bind(sc)) {
            ReleaseSRWLockExclusive(&g_renderLock);
            return;
        }
    }
    R.lastPresentTick = now;
    HRESULT removed = R.device->GetDeviceRemovedReason();
    if (removed != S_OK) {
        QS_ERR("overlay: device removed (0x%08lX)", (unsigned long)removed);
        teardown(false);
        disableOverlay("D3D12 device removed");
        ReleaseSRWLockExclusive(&g_renderLock);
        return;
    }
    if (!R.buffersReady && !createBuffers()) {
        releaseBuffers(true);
        if (++g_failures >= 3) {
            teardown(true);
            disableOverlay("backbuffer setup failed 3 times");
        }
        ReleaseSRWLockExclusive(&g_renderLock);
        return;
    }
    // F5 edge, only while the game window is in the foreground.
    const bool down = (asyncKey(g_toggleVk.load()) & 0x8000) != 0;
    if (down && !g_keyWasDown) {
        HWND fg = GetForegroundWindow();
        if (fg == R.hwnd || IsChild(fg, R.hwnd) || IsChild(R.hwnd, fg)) setMenuOpen(!g_menuOpen.load());
    }
    g_keyWasDown = down;
    renderFrame();
    ReleaseSRWLockExclusive(&g_renderLock);
}

void afterPresent(HRESULT hr) {
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        AcquireSRWLockExclusive(&g_renderLock);
        if (running()) {
            teardown(false);
            disableOverlay("Present reported device removed/reset");
        }
        ReleaseSRWLockExclusive(&g_renderLock);
    }
}

// ------------------------------------------------------------------ detours
HRESULT STDMETHODCALLTYPE hkPresent(IDXGISwapChain* sc, UINT sync, UINT flags) {
    InFlight g;
    if (t_inPresent) return oPresent(sc, sync, flags);  // nested present inside DXGI: draw once only
    t_inPresent = true;
    if (!(flags & DXGI_PRESENT_TEST)) onPresent(sc);
    HRESULT hr = oPresent(sc, sync, flags);
    t_inPresent = false;
    afterPresent(hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE hkPresent1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* pp) {
    InFlight g;
    if (t_inPresent) return oPresent1(sc, sync, flags, pp);
    t_inPresent = true;
    if (!(flags & DXGI_PRESENT_TEST)) onPresent(sc);
    HRESULT hr = oPresent1(sc, sync, flags, pp);
    t_inPresent = false;
    afterPresent(hr);
    return hr;
}

void beforeResize(IDXGISwapChain* sc) {
    AcquireSRWLockExclusive(&g_renderLock);
    if (R.sc == sc) releaseBuffers(true);  // the game's ResizeBuffers fails while we hold backbuffers
    ReleaseSRWLockExclusive(&g_renderLock);
}

HRESULT STDMETHODCALLTYPE hkResizeBuffers(IDXGISwapChain* sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT fl) {
    InFlight g;
    beforeResize(sc);
    HRESULT hr = oResizeBuffers(sc, n, w, h, f, fl);
    QS_DBG("overlay: ResizeBuffers(%u, %ux%u, fmt %d) -> 0x%08lX", n, w, h, int(f), (unsigned long)hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE hkResizeBuffers1(IDXGISwapChain3* sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT fl,
                                           const UINT* mask, IUnknown* const* queues) {
    InFlight g;
    beforeResize(sc);
    HRESULT hr = oResizeBuffers1(sc, n, w, h, f, fl, mask, queues);
    QS_DBG("overlay: ResizeBuffers1(%u, %ux%u, fmt %d) -> 0x%08lX", n, w, h, int(f), (unsigned long)hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE hkSetColorSpace1(IDXGISwapChain3* sc, DXGI_COLOR_SPACE_TYPE cs) {
    InFlight g;
    HRESULT hr = oSetColorSpace1(sc, cs);
    if (SUCCEEDED(hr)) {
        AcquireSRWLockExclusive(&g_mapLock);
        g_colorSpaceBySwapchain[sc] = cs;
        ReleaseSRWLockExclusive(&g_mapLock);
        QS_INFO("overlay: swapchain %p colour space %d", (void*)sc, int(cs));
    }
    return hr;
}

void hookSwapchain(IDXGISwapChain* sc) {
    hookTarget(vtableOf(sc)[kVtScPresent], reinterpret_cast<void*>(&hkPresent), reinterpret_cast<void**>(&oPresent),
               "IDXGISwapChain::Present");
    hookTarget(vtableOf(sc)[kVtScResizeBuffers], reinterpret_cast<void*>(&hkResizeBuffers),
               reinterpret_cast<void**>(&oResizeBuffers), "IDXGISwapChain::ResizeBuffers");
    IDXGISwapChain1* sc1 = nullptr;
    if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc1)))) {
        hookTarget(vtableOf(sc1)[kVtSc1Present1], reinterpret_cast<void*>(&hkPresent1),
                   reinterpret_cast<void**>(&oPresent1), "IDXGISwapChain1::Present1");
        sc1->Release();
    }
    IDXGISwapChain3* sc3 = nullptr;
    if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc3)))) {
        hookTarget(vtableOf(sc3)[kVtSc3ResizeBuffers1], reinterpret_cast<void*>(&hkResizeBuffers1),
                   reinterpret_cast<void**>(&oResizeBuffers1), "IDXGISwapChain3::ResizeBuffers1");
        hookTarget(vtableOf(sc3)[kVtSc3SetColorSpace1], reinterpret_cast<void*>(&hkSetColorSpace1),
                   reinterpret_cast<void**>(&oSetColorSpace1), "IDXGISwapChain3::SetColorSpace1");
        sc3->Release();
    }
}

// Before the game creates a swapchain for `hwnd`: drop ours if it was on that window (a dead swapchain
// kept alive by our backbuffer references would make the creation fail with E_ACCESSDENIED).
void beforeCreate(HWND hwnd) {
    AcquireSRWLockExclusive(&g_renderLock);
    if (R.sc && (R.hwnd == hwnd || !hwnd)) {
        QS_INFO("overlay: new swapchain for window %p, releasing ours", (void*)hwnd);
        teardown(true);
    }
    g_rejectedSc = nullptr;
    ReleaseSRWLockExclusive(&g_renderLock);
}

void afterCreate(IUnknown* device, IDXGISwapChain* sc) {
    if (!sc || t_creatingDummy) return;
    ID3D12CommandQueue* q = nullptr;
    if (device && SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&q)))) {
        AcquireSRWLockExclusive(&g_mapLock);
        g_queueBySwapchain[sc] = q;  // raw pointer: the swapchain itself keeps its queue alive
        g_colorSpaceBySwapchain.erase(sc);  // new object (maybe same address): default colour space
        ReleaseSRWLockExclusive(&g_mapLock);
        q->Release();
        QS_INFO("overlay: game created D3D12 swapchain %p on queue %p", (void*)sc, (void*)q);
    } else {
        QS_DBG("overlay: non-D3D12 swapchain %p created", (void*)sc);
    }
    hookSwapchain(sc);
    g_swapchainSeen.store(true);
}

HRESULT STDMETHODCALLTYPE hkCreateSwapChain(IDXGIFactory* f, IUnknown* dev, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** out) {
    InFlight g;
    if (!t_creatingDummy && running()) beforeCreate(desc ? desc->OutputWindow : nullptr);
    HRESULT hr = oCreateSwapChain(f, dev, desc, out);
    if (SUCCEEDED(hr) && out && running()) afterCreate(dev, *out);
    return hr;
}

HRESULT STDMETHODCALLTYPE hkCreateSwapChainForHwnd(IDXGIFactory2* f, IUnknown* dev, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* d,
                                                   const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* o,
                                                   IDXGISwapChain1** out) {
    InFlight g;
    if (!t_creatingDummy && running()) beforeCreate(hwnd);
    HRESULT hr = oCreateSwapChainForHwnd(f, dev, hwnd, d, fs, o, out);
    if (SUCCEEDED(hr) && out && running()) afterCreate(dev, *out);
    return hr;
}

void STDMETHODCALLTYPE hkExecuteCommandLists(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists) {
    InFlight g;
    if (!t_ourSubmit && running()) {
        static thread_local ID3D12CommandQueue* lastSeen = nullptr;
        if (q != lastSeen) {  // GetDesc only when the queue changes on this thread
            lastSeen = q;
            D3D12_COMMAND_QUEUE_DESC d = q->GetDesc();
            if (d.Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
                g_lastDirectQueue.store(q);
                if (GetCurrentThreadId() == g_presentThreadId.load()) g_presentThreadQueue.store(q);
            }
        }
    }
    oExecuteCommandLists(q, n, lists);
}

// ------------------------------------------------------------------ worker
// Logs other overlays, upscaler proxies and loaders sharing the process: they are the usual cause of
// overlay problems, and a log line saves a lot of guessing in bug reports.
void logCoexistingModules(const char* when) {
    struct Known {
        const wchar_t* name;
        const char* what;
    } known[] = {
        {L"sl.interposer.dll", "NVIDIA Streamline interposer"},
        {L"sl.dlss_g.dll", "Streamline DLSS frame generation"},
        {L"nvngx_dlssg.dll", "DLSS frame generation"},
        {L"libxess_fg.dll", "XeSS frame generation"},
        {L"amd_fidelityfx_dx12.dll", "FidelityFX (FSR frame generation)"},
        {L"gameoverlayrenderer64.dll", "Steam overlay"},
        {L"ReShade64.dll", "ReShade"},
        {L"OptiScaler.dll", "OptiScaler"},
        {L"RTSSHooks64.dll", "RivaTuner overlay"},
        {L"DiscordHook64.dll", "Discord overlay"},
        {L"Seasons.asi", "old Seasons of Pywel mod"},
    };
    for (const Known& k : known)
        if (GetModuleHandleW(k.name)) QS_INFO("overlay [%s]: found %s (%ls)", when, k.what, k.name);
    // dxgi.dll / d3d12.dll / winmm.dll / version.dll from outside System32 = proxy (ReShade, OptiScaler,
    // another ASI loader...).
    wchar_t sys[MAX_PATH];
    UINT n = GetSystemDirectoryW(sys, MAX_PATH);
    const wchar_t* proxies[] = {L"dxgi.dll", L"d3d12.dll", L"winmm.dll", L"version.dll", L"dinput8.dll", L"d3d11.dll"};
    for (const wchar_t* p : proxies) {
        HMODULE m = GetModuleHandleW(p);
        wchar_t path[MAX_PATH];
        if (!m || !GetModuleFileNameW(m, path, MAX_PATH) || n == 0) continue;
        if (_wcsnicmp(path, sys, n) != 0) {
            bool reshade = GetProcAddress(m, "ReShadeRegisterAddon") != nullptr;
            bool asiLoader = GetProcAddress(m, "IsUltimateASILoader") != nullptr;
            QS_INFO("overlay [%s]: %ls is a proxy: %ls%s", when, p, path,
                    reshade ? " (ReShade)" : asiLoader ? " (Ultimate ASI Loader)" : "");
        }
    }
}

LRESULT CALLBACK dummyWndProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

bool hookFactory(HMODULE dxgi) {
    auto create = reinterpret_cast<CreateDXGIFactory1Fn>((void*)GetProcAddress(dxgi, "CreateDXGIFactory1"));
    if (!create) return false;
    IDXGIFactory2* f = nullptr;
    if (FAILED(create(IID_PPV_ARGS(&f)))) {
        QS_ERR("overlay: CreateDXGIFactory1(IDXGIFactory2) failed");
        return false;
    }
    bool ok = hookTarget(vtableOf(f)[kVtFactoryCreateSwapChain], reinterpret_cast<void*>(&hkCreateSwapChain),
                         reinterpret_cast<void**>(&oCreateSwapChain), "IDXGIFactory::CreateSwapChain");
    ok &= hookTarget(vtableOf(f)[kVtFactory2CreateSwapChainForHwnd], reinterpret_cast<void*>(&hkCreateSwapChainForHwnd),
                     reinterpret_cast<void**>(&oCreateSwapChainForHwnd), "IDXGIFactory2::CreateSwapChainForHwnd");
    g_factory = f;  // kept for the late-start fallback (a second factory creation can stall under wine)
    return ok;
}

// Late start: read the swapchain vtable from a throw-away device + swapchain. D3D12 devices are
// per-adapter singletons, so if the game already has one this just borrows it for a moment.
bool hookFromDummy(HMODULE dxgi, HMODULE d3d12) {
    auto createFactory = reinterpret_cast<CreateDXGIFactory1Fn>((void*)GetProcAddress(dxgi, "CreateDXGIFactory1"));
    auto createDevice = d3d12 ? reinterpret_cast<D3D12CreateDeviceFn>((void*)GetProcAddress(d3d12, "D3D12CreateDevice")) : nullptr;
    if (!createFactory || !createDevice) return false;
    (void)createFactory;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = dummyWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"QuattroStagioniOverlayDummy";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr,
                                wc.hInstance, nullptr);
    IDXGIFactory2* f = nullptr;
    ID3D12Device* dev = nullptr;
    ID3D12CommandQueue* q = nullptr;
    IDXGISwapChain1* sc = nullptr;
    if (g_factory) {
        f = g_factory;
        f->AddRef();
    }
    bool ok = hwnd && (f || SUCCEEDED(createFactory(IID_PPV_ARGS(&f)))) &&
              SUCCEEDED(createDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)));
    if (ok) {
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ok = SUCCEEDED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q)));
    }
    if (ok) {
        DXGI_SWAP_CHAIN_DESC1 d{};
        d.Width = 64;
        d.Height = 64;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        d.BufferCount = 2;
        d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        t_creatingDummy = true;
        ok = SUCCEEDED(f->CreateSwapChainForHwnd(q, hwnd, &d, nullptr, nullptr, &sc));
        t_creatingDummy = false;
    }
    QS_DBG("overlay: dummy objects %s", ok ? "created" : "failed");
    if (ok) hookSwapchain(sc);
    else QS_ERR("overlay: dummy device/swapchain creation failed");
    safeRelease(sc);
    safeRelease(q);
    safeRelease(dev);
    safeRelease(f);
    if (hwnd) DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return ok;
}

DWORD WINAPI workerMain(void*) {
    QS_INFO("overlay: worker started (ImGui %s, toggle key 0x%02X)", IMGUI_VERSION, g_toggleVk.load());
    MH_STATUS st = MH_Initialize();
    if (st == MH_OK) g_weInitialisedMinHook = true;
    else if (st != MH_ERROR_ALREADY_INITIALIZED) {
        QS_ERR("overlay: MH_Initialize failed: %s", MH_StatusToString(st));
        g_state.store(int(State::Disabled));
        return 0;
    }
    HMODULE dxgi = loadSystemDll(L"dxgi.dll");
    HMODULE d3d12 = loadSystemDll(L"d3d12.dll");
    if (!dxgi || !d3d12) {
        QS_ERR("overlay: system dxgi.dll/d3d12.dll not loadable (%p, %p)", (void*)dxgi, (void*)d3d12);
        g_state.store(int(State::Disabled));
        return 0;
    }
    g_state.store(int(State::Running));
    logCoexistingModules("start");
    installInputHooks();
    if (!hookFactory(dxgi)) QS_ERR("overlay: factory hooks failed, relying on the late-start fallback");
    qs::log::flush();

    // Wait for the game's swapchain. If it was created before our hooks (late start), fall back to
    // the dummy device once a D3D12 device exists in the process (D3D12Core loaded) or after 2 min.
    // QS_OVERLAY_FALLBACK_MS (environment) shortens the 2 min cap, for tools/overlay_demo --late.
    DWORD cap = 120000;
    char env[16];
    if (GetEnvironmentVariableA("QS_OVERLAY_FALLBACK_MS", env, sizeof env) > 0) cap = DWORD(strtoul(env, nullptr, 10));
    const DWORD t0 = GetTickCount();
    while (WaitForSingleObject(g_stopEvent, 250) == WAIT_TIMEOUT) {
        if (g_swapchainSeen.load() || !running()) break;
        DWORD el = GetTickCount() - t0;
        bool core = GetModuleHandleW(L"D3D12Core.dll") != nullptr;
        if ((el > 10000 && core) || el > cap) {
            QS_INFO("overlay: no swapchain creation seen after %lu ms, hooking from a dummy device", el);
            hookFromDummy(dxgi, d3d12);
            break;
        }
    }
    qs::log::flush();
    return 0;
}

}  // namespace

// ------------------------------------------------------------------ public API
bool overlayStart() {
    int expected = int(State::Stopped);
    if (!g_state.compare_exchange_strong(expected, int(State::Starting))) return true;  // already started
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_worker = g_stopEvent ? CreateThread(nullptr, 0, workerMain, nullptr, 0, nullptr) : nullptr;
    if (!g_worker) {
        QS_ERR("overlay: cannot start worker thread (%lu)", GetLastError());
        g_state.store(int(State::Disabled));
        return false;
    }
    return true;
}

void overlayStop() {
    if (g_state.load() == int(State::Stopped)) return;
    if (g_stopEvent) SetEvent(g_stopEvent);
    if (g_worker) {
        WaitForSingleObject(g_worker, 3000);
        CloseHandle(g_worker);
        g_worker = nullptr;
    }
    g_state.store(int(State::Disabled));  // detours become pass-through
    {
        std::lock_guard<std::mutex> lk(g_hookMutex);
        for (void* t : g_hookTargets) MH_DisableHook(t);
    }
    for (int i = 0; i < 200 && g_inFlight.load() > 0; ++i) Sleep(10);
    AcquireSRWLockExclusive(&g_renderLock);
    teardown(true);
    ReleaseSRWLockExclusive(&g_renderLock);
    {
        std::lock_guard<std::mutex> lk(g_hookMutex);
        for (void* t : g_hookTargets) MH_RemoveHook(t);
        g_hookTargets.clear();
    }
    if (g_imguiCreated) {
        ImGui::DestroyContext();
        g_imguiCreated = false;
    }
    if (g_factory) {
        g_factory->Release();
        g_factory = nullptr;
    }
    if (g_weInitialisedMinHook) MH_Uninitialize();
    if (g_stopEvent) CloseHandle(g_stopEvent);
    g_stopEvent = nullptr;
    QS_INFO("overlay: stopped");
}

void overlaySetToggleKey(int vk) {
    if (vk > 0 && vk < 256) g_toggleVk.store(vk);
}

bool overlayActive() { return running() && R.buffersReady; }

}  // namespace qs

// Used by tools/overlay_demo to start the overlay before integration wires overlayStart() in main.cpp.
extern "C" __declspec(dllexport) int QuattroStagioniOverlayStart() { return qs::overlayStart() ? 1 : 0; }
