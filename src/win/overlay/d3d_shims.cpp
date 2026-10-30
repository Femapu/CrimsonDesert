// QuattroStagioni - runtime-loaded D3DCompile / CreateDXGIFactory1 for the ImGui DX12 backend.
//
// imgui_impl_dx12.cpp compiles its two tiny shaders with D3DCompile and creates a DXGI factory (tearing
// check) at init. Linking d3dcompiler/dxgi would add them to the ASI import table (loaded at game start,
// even with the overlay off, and dxgi.dll could resolve to a local proxy), so these definitions satisfy
// the symbols and load the SYSTEM DLLs only when the overlay initialises.
#include <windows.h>

#include <d3dcompiler.h>
#include <dxgi1_6.h>

namespace {

void* systemProc(const wchar_t* dll, const char* proc) {
    wchar_t path[MAX_PATH];
    UINT n = GetSystemDirectoryW(path, MAX_PATH);
    if (n == 0 || n + lstrlenW(dll) + 2 >= MAX_PATH) return nullptr;
    lstrcatW(path, L"\\");
    lstrcatW(path, dll);
    HMODULE m = LoadLibraryW(path);  // kept loaded for the process lifetime
    return m ? reinterpret_cast<void*>(GetProcAddress(m, proc)) : nullptr;
}

}  // namespace

extern "C" HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void** out) {
    using Fn = HRESULT(WINAPI*)(REFIID, void**);
    static Fn fn = reinterpret_cast<Fn>(systemProc(L"dxgi.dll", "CreateDXGIFactory1"));
    if (!fn) {
        if (out) *out = nullptr;
        return E_FAIL;
    }
    return fn(riid, out);
}

extern "C" HRESULT WINAPI D3DCompile(const void* data, SIZE_T size, const char* filename, const D3D_SHADER_MACRO* defines,
                                     ID3DInclude* include, const char* entry, const char* target, UINT sflags,
                                     UINT eflags, ID3DBlob** shader, ID3DBlob** errors) {
    static pD3DCompile fn = reinterpret_cast<pD3DCompile>(systemProc(L"d3dcompiler_47.dll", "D3DCompile"));
    if (!fn) return HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
    return fn(data, size, filename, defines, include, entry, target, sflags, eflags, shader, errors);
}
