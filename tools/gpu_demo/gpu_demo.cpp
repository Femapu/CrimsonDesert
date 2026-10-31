// QuattroStagioni - GPU live swap demo and end-to-end check (Windows, or Linux with wine + vkd3d).
//
// Plays the role of the game: writes a synthetic game folder (one .paz with BC1/BC3 textures) and two
// season packs ("A" and "W", built with the pack writer of src/core/pack), loads QuattroStagioni.asi like
// Ultimate ASI Loader, starts the overlay (it captures the DIRECT queue) and the GPU swap through its
// demo export, then creates textures through the usual D3D12 upload path (upload buffer +
// CopyTextureRegion, one texture on a COPY queue relying on state promotion). After every stage change
// it reads the textures back (CopyTextureRegion into a readback buffer) and checks, byte for byte, that
// the resident content became the new stage's data. Exit code 0 = every check passed.
//
// Usage: gpu_demo.exe [path\to\QuattroStagioni.asi] [--keep]   (--keep: leave gpu_demo_data\ behind)
#include <windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "../../src/core/gpu/fingerprint.h"
#include "../../tests/gpu_synth.h"

namespace {

constexpr UINT kW = 640, kH = 360, kBuffers = 3;
int g_fail = 0, g_pass = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    std::fflush(stdout);
    ok ? ++g_pass : ++g_fail;
}

LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

using StartFn = int (*)(const wchar_t*, const wchar_t*, const char*);
using SetStageFn = void (*)(const char*);
using StatsFn = int (*)(char*, int);
using StopFn = void (*)();
StatsFn g_stats = nullptr;

struct Gfx {
    ID3D12Device* dev = nullptr;
    ID3D12CommandQueue* queue = nullptr;      // DIRECT, owns the swapchain
    ID3D12CommandQueue* copyQueue = nullptr;  // COPY, for the promotion case
    IDXGISwapChain3* sc = nullptr;
    ID3D12DescriptorHeap* rtvHeap = nullptr;
    ID3D12Resource* bb[kBuffers] = {};
    ID3D12CommandAllocator* frameAlloc[kBuffers] = {};
    ID3D12GraphicsCommandList* frameList = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;   // one-shot work on the DIRECT queue
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12CommandAllocator* copyAlloc = nullptr;
    ID3D12GraphicsCommandList* copyList = nullptr;
    ID3D12Fence* fence = nullptr;
    UINT64 fv[kBuffers] = {}, last = 0;
    HANDLE ev = nullptr;
    UINT frame = 0;
} G;

void waitFence(UINT64 v) {
    if (G.fence->GetCompletedValue() < v) {
        G.fence->SetEventOnCompletion(v, G.ev);
        WaitForSingleObject(G.ev, 10000);
    }
}

UINT64 submit(ID3D12CommandQueue* q, ID3D12GraphicsCommandList* l) {
    l->Close();
    ID3D12CommandList* lists[] = {l};
    q->ExecuteCommandLists(1, lists);
    q->Signal(G.fence, ++G.last);
    return G.last;
}

bool createGfx(HWND hwnd) {
    IDXGIFactory4* f = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) return false;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&G.dev)))) return false;
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(G.dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&G.queue)))) return false;
    qd.Type = D3D12_COMMAND_LIST_TYPE_COPY;
    if (FAILED(G.dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&G.copyQueue)))) return false;
    DXGI_SWAP_CHAIN_DESC1 d{};
    d.Width = kW;
    d.Height = kH;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.BufferCount = kBuffers;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain1* sc1 = nullptr;
    if (FAILED(f->CreateSwapChainForHwnd(G.queue, hwnd, &d, nullptr, nullptr, &sc1))) return false;
    sc1->QueryInterface(IID_PPV_ARGS(&G.sc));
    sc1->Release();
    f->Release();
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = kBuffers;
    G.dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&G.rtvHeap));
    UINT inc = G.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for (UINT i = 0; i < kBuffers; ++i) {
        G.sc->GetBuffer(i, IID_PPV_ARGS(&G.bb[i]));
        D3D12_CPU_DESCRIPTOR_HANDLE h = G.rtvHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += SIZE_T(i) * inc;
        G.dev->CreateRenderTargetView(G.bb[i], nullptr, h);
        G.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&G.frameAlloc[i]));
    }
    G.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, G.frameAlloc[0], nullptr, IID_PPV_ARGS(&G.frameList));
    G.frameList->Close();
    G.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&G.alloc));
    G.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, G.alloc, nullptr, IID_PPV_ARGS(&G.list));
    G.list->Close();
    G.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&G.copyAlloc));
    G.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, G.copyAlloc, nullptr, IID_PPV_ARGS(&G.copyList));
    G.copyList->Close();
    G.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&G.fence));
    G.ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    return true;
}

void pumpMessages() {
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

// One frame: a coloured clear (the colour tells the stage in screenshots) and a present.
void frame(const float* colour) {
    pumpMessages();
    UINT i = G.sc->GetCurrentBackBufferIndex();
    waitFence(G.fv[i]);
    G.frameAlloc[i]->Reset();
    G.frameList->Reset(G.frameAlloc[i], nullptr);
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = G.bb[i];
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    G.frameList->ResourceBarrier(1, &b);
    D3D12_CPU_DESCRIPTOR_HANDLE h = G.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += SIZE_T(i) * G.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    G.frameList->ClearRenderTargetView(h, colour, 0, nullptr);
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    G.frameList->ResourceBarrier(1, &b);
    G.fv[i] = submit(G.queue, G.frameList);
    G.sc->Present(1, 0);
    ++G.frame;
}

std::string stats() {
    char buf[1024] = {};
    if (g_stats) g_stats(buf, sizeof buf);
    return buf;
}

unsigned long long statValue(const std::string& s, const char* key) {
    size_t p = s.find(std::string(" ") + key + "=");
    if (p == std::string::npos) p = s.rfind(std::string(key) + "=", 0) == 0 ? 0 : std::string::npos;
    if (p == std::string::npos) return 0;
    p = s.find('=', p) + 1;
    return std::strtoull(s.c_str() + p, nullptr, 10);
}

// Presents frames until cond() holds or the timeout expires.
template <class F>
bool runUntil(F cond, DWORD ms, const float* colour) {
    const DWORD t0 = GetTickCount();
    while (GetTickCount() - t0 < ms) {
        frame(colour);
        if (cond()) return true;
    }
    return false;
}

// ---------------------------------------------------------------- textures, game-style upload
struct Tex {
    const char* name;
    ID3D12Resource* res = nullptr;
    DXGI_FORMAT format;
    uint32_t width, height;   // resource dims (may skip DDS mips)
    uint16_t mips;
    uint32_t firstDdsMip;
    D3D12_RESOURCE_STATES state;  // state the "game" leaves it in
    bool copyQueue;
};

bool createTexture(Tex& t) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = t.width;
    d.Height = t.height;
    d.DepthOrArraySize = 1;
    d.MipLevels = t.mips;
    d.Format = t.format;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    return SUCCEEDED(G.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                                    t.copyQueue ? D3D12_RESOURCE_STATE_COMMON : D3D12_RESOURCE_STATE_COPY_DEST,
                                                    nullptr, IID_PPV_ARGS(&t.res)));
}

ID3D12Resource* makeBuffer(D3D12_HEAP_TYPE type, UINT64 size) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = type;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = size;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* r = nullptr;
    G.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                   type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST,
                                   nullptr, IID_PPV_ARGS(&r));
    return r;
}

// Uploads subresources [firstSub, firstSub + count) from a stored per-mip DDS, like a streaming engine:
// one upload buffer, one CopyTextureRegion per subresource, then the transition to the final state.
bool upload(Tex& t, const std::vector<uint8_t>& stored, UINT firstSub, UINT count, bool firstTime) {
    std::vector<uint8_t> dds;
    qs::gpu::TexturePrints tp;
    if (!qs::gpu::computePrintsStored(stored.data(), stored.size(), tp, nullptr, &dds)) return false;
    D3D12_RESOURCE_DESC desc = t.res->GetDesc();
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> fp(count);
    std::vector<UINT> rows(count);
    std::vector<UINT64> rowSize(count);
    UINT64 total = 0;
    G.dev->GetCopyableFootprints(&desc, firstSub, count, 0, fp.data(), rows.data(), rowSize.data(), &total);
    ID3D12Resource* up = makeBuffer(D3D12_HEAP_TYPE_UPLOAD, total);
    uint8_t* p = nullptr;
    up->Map(0, nullptr, reinterpret_cast<void**>(&p));
    for (UINT i = 0; i < count; ++i) {
        const uint32_t m = firstSub + i + t.firstDdsMip;
        const uint8_t* src = dds.data() + tp.headerSize + tp.mipOffset[m];
        for (UINT r = 0; r < rows[i]; ++r)
            std::memcpy(p + fp[i].Offset + UINT64(r) * fp[i].Footprint.RowPitch, src + UINT64(r) * rowSize[i], size_t(rowSize[i]));
    }
    up->Unmap(0, nullptr);
    ID3D12CommandAllocator* a = t.copyQueue ? G.copyAlloc : G.alloc;
    ID3D12GraphicsCommandList* l = t.copyQueue ? G.copyList : G.list;
    a->Reset();
    l->Reset(a, nullptr);
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = t.res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    if (!t.copyQueue && !firstTime) {  // re-upload: back to COPY_DEST first
        b.Transition.StateBefore = t.state;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        l->ResourceBarrier(1, &b);
    }
    for (UINT i = 0; i < count; ++i) {
        D3D12_TEXTURE_COPY_LOCATION dl{}, sl{};
        dl.pResource = t.res;
        dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dl.SubresourceIndex = firstSub + i;
        sl.pResource = up;
        sl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        sl.PlacedFootprint = fp[i];
        l->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
    }
    if (!t.copyQueue) {  // the copy queue relies on promotion/decay: no barrier at all
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = t.state;
        l->ResourceBarrier(1, &b);
    }
    waitFence(submit(t.copyQueue ? G.copyQueue : G.queue, l));
    up->Release();
    return true;
}

// Reads every subresource back and compares it with the mips of a stored DDS.
bool matches(Tex& t, const std::vector<uint8_t>& stored) {
    std::vector<uint8_t> dds;
    qs::gpu::TexturePrints tp;
    if (!qs::gpu::computePrintsStored(stored.data(), stored.size(), tp, nullptr, &dds)) return false;
    D3D12_RESOURCE_DESC desc = t.res->GetDesc();
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> fp(t.mips);
    std::vector<UINT> rows(t.mips);
    std::vector<UINT64> rowSize(t.mips);
    UINT64 total = 0;
    G.dev->GetCopyableFootprints(&desc, 0, t.mips, 0, fp.data(), rows.data(), rowSize.data(), &total);
    ID3D12Resource* rb = makeBuffer(D3D12_HEAP_TYPE_READBACK, total);
    G.alloc->Reset();
    G.list->Reset(G.alloc, nullptr);
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = t.res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = t.state;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    const bool explicitState = t.state != D3D12_RESOURCE_STATE_COMMON;  // COMMON: implicit promotion + decay
    if (explicitState) G.list->ResourceBarrier(1, &b);
    for (UINT s = 0; s < t.mips; ++s) {
        D3D12_TEXTURE_COPY_LOCATION dl{}, sl{};
        dl.pResource = rb;
        dl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dl.PlacedFootprint = fp[s];
        sl.pResource = t.res;
        sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        sl.SubresourceIndex = s;
        G.list->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
    }
    if (explicitState) {
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.StateAfter = t.state;
        G.list->ResourceBarrier(1, &b);
    }
    waitFence(submit(G.queue, G.list));
    uint8_t* p = nullptr;
    D3D12_RANGE all{0, SIZE_T(total)};
    rb->Map(0, &all, reinterpret_cast<void**>(&p));
    bool ok = p != nullptr;
    int firstBad = -1;
    for (UINT s = 0; ok && s < t.mips; ++s) {
        const uint8_t* exp = dds.data() + tp.headerSize + tp.mipOffset[s + t.firstDdsMip];
        for (UINT r = 0; r < rows[s]; ++r)
            if (std::memcmp(p + fp[s].Offset + UINT64(r) * fp[s].Footprint.RowPitch, exp + UINT64(r) * rowSize[s], size_t(rowSize[s])) != 0) {
                ok = false;
                firstBad = int(s);
                break;
            }
    }
    D3D12_RANGE none{0, 0};
    rb->Unmap(0, &none);
    rb->Release();
    if (!ok && firstBad >= 0) std::printf("  %s: subresource %d differs\n", t.name, firstBad);
    return ok;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace qs::gpusynth;
    namespace fs = std::filesystem;
    std::string asi = "QuattroStagioni.asi";
    bool keep = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--keep") keep = true;
        else asi = a;
    }

    // ---- synthetic game folder and packs (no game data)
    const fs::path work = fs::absolute("gpu_demo_data");
    fs::remove_all(work);
    fs::create_directories(work / "game" / "0001");
    fs::create_directories(work / "packs");
    std::vector<SynthTexture> tex = {
        {"DXT1", 256, 256, 9, 21, "leaf_t0.dds", 0, {}},   // T0: BC1, direct queue, PIXEL|NON_PIXEL
        {"DXT5", 128, 128, 8, 22, "leaf_t1.dds", 0, {}},   // T1: BC3, copy queue, COMMON, top mip skipped
        {"DXT1", 64, 64, 7, 23, "leaf_t2.dds", 0, {}},     // T2: loaded later while stage A is active
        {"DXT5", 128, 64, 8, 24, "rock_t3.dds", 0, {}},    // T3: in no pack (decoy)
    };
    std::vector<uint8_t> paz = buildPaz(tex);
    if (!qs::pack::writeWholeFile((work / "game" / "0001" / "0.paz").string(), paz.data(), paz.size())) return 3;
    std::vector<std::vector<uint8_t>> A = {makeStageVariant(tex[0].original, "DXT1", 256, 256, 9, 121, 6),
                                           makeStageVariant(tex[1].original, "DXT5", 128, 128, 8, 122),
                                           makeStageVariant(tex[2].original, "DXT1", 64, 64, 7, 123), {}};
    std::vector<std::vector<uint8_t>> W = {makeStageVariant(tex[0].original, "DXT1", 256, 256, 9, 221),
                                           makeStageVariant(tex[1].original, "DXT5", 128, 128, 8, 222), {}, {}};
    std::string err;
    if (!writePack((work / "packs" / "A.qsp").string(), "A", "0001", 0, paz.size(), tex, A, &err) ||
        !writePack((work / "packs" / "W.qsp").string(), "W", "0001", 0, paz.size(), tex, W, &err)) {
        std::printf("pack write failed: %s\n", err.c_str());
        return 3;
    }
    std::printf("synthetic game folder and packs A, W written in %s\n", work.string().c_str());

    // ---- plugin, as Ultimate ASI Loader would load it
    HMODULE m = LoadLibraryA(asi.c_str());
    if (!m) {
        std::printf("cannot load %s (error %lu)\n", asi.c_str(), GetLastError());
        return 1;
    }
    auto init = reinterpret_cast<void (*)()>(reinterpret_cast<void*>(GetProcAddress(m, "InitializeASI")));
    auto overlay = reinterpret_cast<int (*)()>(reinterpret_cast<void*>(GetProcAddress(m, "QuattroStagioniOverlayStart")));
    auto start = reinterpret_cast<StartFn>(reinterpret_cast<void*>(GetProcAddress(m, "QuattroStagioniGpuSwapStart")));
    auto setStage = reinterpret_cast<SetStageFn>(reinterpret_cast<void*>(GetProcAddress(m, "QuattroStagioniGpuSwapSetStage")));
    auto stop = reinterpret_cast<StopFn>(reinterpret_cast<void*>(GetProcAddress(m, "QuattroStagioniGpuSwapStop")));
    g_stats = reinterpret_cast<StatsFn>(reinterpret_cast<void*>(GetProcAddress(m, "QuattroStagioniGpuSwapStats")));
    if (!start || !setStage || !g_stats || !stop) {
        std::printf("the ASI has no GPU swap exports\n");
        return 1;
    }
    if (init) init();
    if (overlay) overlay();
    start((work / "game").wstring().c_str(), (work / "packs").wstring().c_str(), "");
    Sleep(1500);  // let the workers hook DXGI and D3D12CreateDevice

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"QSGpuDemo";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"QuattroStagioni GPU swap demo", WS_OVERLAPPEDWINDOW, 0, 0, kW, kH, nullptr,
                                nullptr, wc.hInstance, nullptr);
    ShowWindow(hwnd, SW_SHOW);
    if (!createGfx(hwnd)) {
        std::printf("D3D12 init failed\n");
        return 2;
    }
    const float summer[4] = {0.2f, 0.5f, 0.2f, 1}, autumn[4] = {0.7f, 0.4f, 0.1f, 1}, winter[4] = {0.8f, 0.85f, 0.9f, 1};

    check(runUntil([] { return statValue(stats(), "hooks") == 1 && statValue(stats(), "queue") == 1; }, 30000, summer),
          "hooks installed and game queue captured through the overlay");
    check(runUntil([] { return statValue(stats(), "index") >= 3; }, 30000, summer), "packs A and W indexed (3 textures)");
    std::printf("  %s\n", stats().c_str());

    // ---- the "game" loads its textures (vanilla data: no pack was active)
    Tex t0{"T0", nullptr, DXGI_FORMAT_BC1_UNORM, 256, 256, 9, 0,
           D3D12_RESOURCE_STATES(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE), false};
    Tex t1{"T1", nullptr, DXGI_FORMAT_BC3_UNORM_SRGB, 64, 64, 7, 1, D3D12_RESOURCE_STATE_COMMON, true};
    Tex t2{"T2", nullptr, DXGI_FORMAT_BC1_UNORM_SRGB, 64, 64, 7, 0, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, false};
    Tex t3{"T3", nullptr, DXGI_FORMAT_BC3_UNORM, 128, 64, 8, 0, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, false};
    bool ok = createTexture(t0) && createTexture(t1) && createTexture(t3);
    ok = ok && upload(t0, tex[0].original, 0, 9, true) && upload(t1, tex[1].original, 0, 7, true) &&
         upload(t3, tex[3].original, 0, 8, true);
    check(ok, "textures created and uploaded (upload buffer + CopyTextureRegion)");
    check(runUntil([] { return statValue(stats(), "tracked") == 2 && statValue(stats(), "idle") == 1; }, 10000, summer),
          "T0 and T1 identified from their uploads, decoy T3 ignored");
    check(matches(t0, tex[0].original) && matches(t1, tex[1].original) && matches(t3, tex[3].original), "vanilla content resident");

    // ---- stage A
    setStage("A");
    const unsigned long long batches0 = statValue(stats(), "batches");
    check(runUntil([] { return statValue(stats(), "idle") == 1 && statValue(stats(), "jobs") > 0; }, 20000, autumn), "stage A applied");
    check(matches(t0, A[0]), "T0 now holds stage A data (mips 6-8, shared by A and the original, need no copy)");
    check(matches(t1, A[1]), "T1 (copy queue, COMMON, top mip skipped) now holds stage A data");
    check(matches(t3, tex[3].original), "decoy T3 untouched");
    std::printf("  %s\n", stats().c_str());
    check(statValue(stats(), "batches") - batches0 >= 2, "swap spread over several ticks by the 16 KB budget");

    // ---- a texture streamed in while A is active carries A's bytes (file hook): recognised by A's prints
    ok = createTexture(t2) && upload(t2, A[2], 0, 7, true);
    check(ok && runUntil([] { return statValue(stats(), "tracked") == 3 && statValue(stats(), "idle") == 1; }, 10000, autumn),
          "T2 loaded with stage A bytes is identified (replacement prints)");
    check(matches(t2, A[2]), "T2 keeps stage A data (no needless swap)");

    // ---- stage W: T2 is not in W -> back to the original read from the .paz
    setStage("W");
    check(runUntil([] { return stats().find("stage=W") != std::string::npos && statValue(stats(), "idle") == 1; }, 20000, winter),
          "stage W applied");
    check(matches(t0, W[0]) && matches(t1, W[1]), "T0 and T1 hold stage W data");
    check(matches(t2, tex[2].original), "T2 (no W data) restored to the original from the .paz");
    check(matches(t3, tex[3].original), "decoy T3 still untouched");

    // ---- stale upload: the game re-uploads T0 mip 0 from a cached stage A copy -> fixed after a few ticks
    const unsigned long long done0 = statValue(stats(), "jobs");
    ok = upload(t0, A[0], 0, 1, false);
    check(ok && runUntil([&] { return statValue(stats(), "jobs") > done0 && statValue(stats(), "idle") == 1; }, 10000, winter),
          "stale stage A upload into T0 detected");
    check(matches(t0, W[0]), "T0 mip 0 rewritten with stage W data");

    // ---- back to vanilla
    setStage("U");
    check(runUntil([] { return stats().find("stage=U") != std::string::npos && statValue(stats(), "idle") == 1; }, 20000, summer),
          "vanilla stage applied");
    check(matches(t0, tex[0].original) && matches(t1, tex[1].original) && matches(t2, tex[2].original), "all textures vanilla again");

    // ---- lifetime: releasing a texture untracks it through the private-data sentinel
    const unsigned long long destroyed0 = statValue(stats(), "destroyed");
    waitFence(G.last);
    t2.res->Release();
    t2.res = nullptr;
    check(runUntil([&] { return statValue(stats(), "destroyed") == destroyed0 + 1 && statValue(stats(), "tracked") == 2; }, 5000, summer),
          "released texture untracked by its sentinel");
    std::printf("  %s\n", stats().c_str());

    // ---- unload path: sentinels detached, textures can be released afterwards without our code
    stop();
    t0.res->Release();
    t1.res->Release();
    t3.res->Release();
    for (int i = 0; i < 10; ++i) frame(summer);
    check(true, "module stopped, remaining textures released without a crash");

    std::printf("gpu_demo: %d passed, %d failed\n", g_pass, g_fail);
    std::fflush(stdout);
    if (!keep) {
        std::error_code ec;
        fs::remove_all(work, ec);
    }
    // skip process-exit destructors of the loaded plugin (see docs/dev/gpu_swap.md, open points)
    TerminateProcess(GetCurrentProcess(), g_fail ? 4 : 0);
    return g_fail ? 4 : 0;
}
