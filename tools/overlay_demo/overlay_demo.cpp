// QuattroStagioni - standalone overlay demo (Windows, or Linux with wine + vkd3d).
//
// Loads QuattroStagioni.asi like Ultimate ASI Loader would (LoadLibrary + InitializeASI), starts the
// overlay through its demo export, then creates a D3D12 window that presents a fake "landscape" so the
// F5 menu can be tried outside the game. The plugin's version guard fails here (no game exe), so the
// file I/O hooks stay off: only the overlay runs.
//
// Usage: overlay_demo.exe [path\to\QuattroStagioni.asi] [--open] [--frames N] [--click X,Y@FRAME]...
//   --open        press F5 automatically after 30 frames
//   --frames N    exit after N frames (default: run until the window is closed)
//   --click X,Y@F left click at client coordinates X,Y on frame F (to switch tabs in scripted runs)
//   --late        create the swapchain BEFORE starting the overlay (tests the late-start fallback)
#include <windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

constexpr UINT kW = 1600, kH = 900, kBuffers = 3;

LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

void sendKey(WORD vk, bool up) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
    SendInput(1, &in, sizeof in);
}

void sendClick(HWND hwnd, int x, int y, int phase) {
    POINT p{x, y};
    ClientToScreen(hwnd, &p);
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dx = LONG(p.x * 65535 / (GetSystemMetrics(SM_CXSCREEN) - 1));
    in.mi.dy = LONG(p.y * 65535 / (GetSystemMetrics(SM_CYSCREEN) - 1));
    in.mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE;
    if (phase == 1) in.mi.dwFlags |= MOUSEEVENTF_LEFTDOWN;
    if (phase == 2) in.mi.dwFlags |= MOUSEEVENTF_LEFTUP;
    SendInput(1, &in, sizeof in);
}

struct Click {
    int x, y, frame;
};

struct Gfx {
    ID3D12Device* dev = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    IDXGISwapChain3* sc = nullptr;
    ID3D12DescriptorHeap* rtvHeap = nullptr;
    ID3D12Resource* bb[kBuffers] = {};
    ID3D12CommandAllocator* alloc[kBuffers] = {};
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    UINT64 fv[kBuffers] = {}, last = 0;
    HANDLE ev = nullptr;
};

bool createGfx(Gfx& g, HWND hwnd) {
    IDXGIFactory4* f = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) return false;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g.dev)))) return false;
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(g.dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&g.queue)))) return false;
    DXGI_SWAP_CHAIN_DESC1 d{};
    d.Width = kW;
    d.Height = kH;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.BufferCount = kBuffers;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain1* sc1 = nullptr;
    if (FAILED(f->CreateSwapChainForHwnd(g.queue, hwnd, &d, nullptr, nullptr, &sc1))) return false;
    sc1->QueryInterface(IID_PPV_ARGS(&g.sc));
    sc1->Release();
    f->Release();
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = kBuffers;
    g.dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g.rtvHeap));
    UINT inc = g.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for (UINT i = 0; i < kBuffers; ++i) {
        g.sc->GetBuffer(i, IID_PPV_ARGS(&g.bb[i]));
        D3D12_CPU_DESCRIPTOR_HANDLE h = g.rtvHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += SIZE_T(i) * inc;
        g.dev->CreateRenderTargetView(g.bb[i], nullptr, h);
        g.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g.alloc[i]));
    }
    g.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.alloc[0], nullptr, IID_PPV_ARGS(&g.list));
    g.list->Close();
    g.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence));
    g.ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    return true;
}

// A blocky "landscape" (sky bands, hills, a lake) made only of cleared rectangles, so the demo needs
// no shaders: enough to judge the menu's contrast and transparency over a game-like picture.
void drawScene(Gfx& g, UINT frame) {
    UINT i = g.sc->GetCurrentBackBufferIndex();
    if (g.fence->GetCompletedValue() < g.fv[i]) {
        g.fence->SetEventOnCompletion(g.fv[i], g.ev);
        WaitForSingleObject(g.ev, INFINITE);
    }
    g.alloc[i]->Reset();
    g.list->Reset(g.alloc[i], nullptr);
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = g.bb[i];
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    g.list->ResourceBarrier(1, &b);
    D3D12_CPU_DESCRIPTOR_HANDLE h = g.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += SIZE_T(i) * g.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    const int bands = 36;
    for (int k = 0; k < bands; ++k) {  // sky: deep blue to warm horizon
        float t = float(k) / (bands - 1);
        float c[4] = {0.20f + 0.65f * t, 0.32f + 0.40f * t, 0.55f + 0.05f * t, 1};
        D3D12_RECT r{0, LONG(k * kH * 0.55f / bands), LONG(kW), LONG((k + 1) * kH * 0.55f / bands + 1)};
        g.list->ClearRenderTargetView(h, c, 1, &r);
    }
    float sway = std::sin(frame * 0.01f) * 6;
    for (int x = 0; x < int(kW); x += 8) {  // hills, three layers
        for (int layer = 0; layer < 3; ++layer) {
            float y = kH * (0.50f + 0.08f * layer) + std::sin(x * (0.004f + layer * 0.002f) + layer * 1.7f) * (40 - layer * 8) + sway * (layer == 2);
            float c[4] = {0.18f - layer * 0.04f, 0.36f - layer * 0.06f, 0.16f - layer * 0.03f, 1};
            D3D12_RECT r{x, LONG(y), x + 8, LONG(kH)};
            g.list->ClearRenderTargetView(h, c, 1, &r);
        }
    }
    float lake[4] = {0.20f, 0.35f, 0.45f, 1};
    D3D12_RECT lr{LONG(kW * 0.55f), LONG(kH * 0.80f), LONG(kW * 0.95f), LONG(kH * 0.88f)};
    g.list->ClearRenderTargetView(h, lake, 1, &lr);
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    g.list->ResourceBarrier(1, &b);
    g.list->Close();
    ID3D12CommandList* lists[] = {g.list};
    g.queue->ExecuteCommandLists(1, lists);
    g.queue->Signal(g.fence, ++g.last);
    g.fv[i] = g.last;
}

}  // namespace

int main(int argc, char** argv) {
    std::string asi = "QuattroStagioni.asi";
    bool autoOpen = false, late = false;
    long frames = -1;
    std::vector<Click> clicks;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--open") autoOpen = true;
        else if (a == "--late") late = true;
        else if (a == "--frames" && i + 1 < argc) frames = atol(argv[++i]);
        else if (a == "--click" && i + 1 < argc) {
            Click c{};
            if (sscanf(argv[++i], "%d,%d@%d", &c.x, &c.y, &c.frame) == 3) clicks.push_back(c);
        } else asi = a;
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"QSOverlayDemo";
    RegisterClassExW(&wc);
    RECT rc{0, 0, LONG(kW), LONG(kH)};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"QuattroStagioni overlay demo", WS_OVERLAPPEDWINDOW, 0, 0,
                                rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);

    Gfx g;
    auto startOverlay = [&]() -> bool {
        HMODULE m = LoadLibraryA(asi.c_str());
        if (!m) {
            printf("cannot load %s (error %lu)\n", asi.c_str(), GetLastError());
            return false;
        }
        auto init = reinterpret_cast<void (*)()>(reinterpret_cast<void*>(GetProcAddress(m, "InitializeASI")));
        auto start = reinterpret_cast<int (*)()>(reinterpret_cast<void*>(GetProcAddress(m, "QuattroStagioniOverlayStart")));
        if (init) init();
        if (start) start();
        printf("plugin loaded: InitializeASI %s, overlay start %s\n", init ? "called" : "missing", start ? "called" : "missing");
        Sleep(1500);  // let the overlay worker hook the DXGI factory
        return true;
    };
    if (late && !createGfx(g, hwnd)) return 2;
    if (!startOverlay()) return 1;
    if (!late && !createGfx(g, hwnd)) {
        printf("D3D12 init failed\n");
        return 2;
    }
    printf("D3D12 ready, presenting%s\n", late ? " (late start)" : "");
    fflush(stdout);

    MSG msg{};
    for (UINT frame = 0; frames < 0 || long(frame) < frames; ++frame) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) return 0;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (autoOpen && frame == 30) sendKey(VK_F5, false);
        if (autoOpen && frame == 34) sendKey(VK_F5, true);
        for (const Click& c : clicks) {
            if (int(frame) == c.frame - 2) sendClick(hwnd, c.x, c.y, 0);
            if (int(frame) == c.frame) sendClick(hwnd, c.x, c.y, 1);
            if (int(frame) == c.frame + 2) sendClick(hwnd, c.x, c.y, 2);
        }
        drawScene(g, frame);
        g.sc->Present(1, 0);
        if (frame % 60 == 0) {
            printf("frame %u\n", frame);
            fflush(stdout);
        }
    }
    printf("done\n");
    return 0;
}
