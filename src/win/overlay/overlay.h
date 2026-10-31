// QuattroStagioni - D3D12 overlay and F5 menu (contract for the integration stream).
//
// The overlay draws the in-game menu with Dear ImGui on top of the game's swapchain. It hooks only
// system DLL code (dxgi.dll / d3d12 runtime, user32 input functions), never the game exe, and any
// failure disables the overlay (the game keeps running untouched). Design notes: overlay.cpp header.
//
// Wiring (src/win/main.cpp, owned by integration):
//   - call overlayStart() once, AFTER qs::io::install() (both use MinHook; io_hooks.cpp calls
//     MH_Initialize itself and fails if someone initialised it first). It is cheap and returns
//     immediately: the hooks are installed by a worker thread.
//   - call overlayStop() on FreeLibrary only (DLL_PROCESS_DETACH with reserved == NULL), BEFORE
//     qs::io::uninstall() (which calls MH_Uninitialize and would pull our hooks from under us).
//   - menuSetSaveCallback(): the "Salva" button calls it on the game's render thread, without
//     holding shared().mutex. It should write the ini quickly and return false on error.
#pragma once
#include <string>

struct ID3D12CommandQueue;
struct ID3D12CommandList;
struct ID3D12Device;

namespace qs {

// Starts the overlay worker (idempotent). Returns false only if the worker could not be started.
bool overlayStart();
// Removes the overlay hooks and frees GPU objects. Safe to call if overlayStart() was never called.
void overlayStop();
// Virtual-key code that opens/closes the menu (default VK_F5). Can be called at any time.
// TODO: read it from QuattroStagioni.ini ([Menu] ToggleKey) once integration adds the field.
void overlaySetToggleKey(int vk);
// True while the overlay is bound to a swapchain and drawing (for the status line / diagnostics).
bool overlayActive();
// The game's DIRECT command queue the overlay is bound to (the queue that owns the swapchain), with a
// reference added (the caller releases it), or nullptr while not bound. Used by the GPU swap module
// (src/win/gpu/) to submit its texture uploads in the game's queue order.
ID3D12CommandQueue* overlayAcquireGameQueue();
// Called on the game's present thread inside the overlay's Present hook, after the overlay has drawn,
// with the queue the overlay is bound to (the GPU swap submits its batch there, in the game's own
// submission order). Runs under the overlay render lock: it must not block and must not call
// overlayAcquireGameQueue(). Setting nullptr returns only when no call is running any more.
using PresentCallback = void (*)(ID3D12CommandQueue* queue);
void overlaySetPresentCallback(PresentCallback cb);
// Called inside the ExecuteCommandLists hook, before the lists reach the queue (any queue, any thread).
// Used by the GPU swap to read the game's upload buffers when they are complete (at CopyTextureRegion
// time the game may still be filling them). Installs the hook with `device` if needed; false if it
// could not be installed. Must not block.
using ExecuteObserver = void (*)(unsigned n, ID3D12CommandList* const* lists);
bool overlayObserveExecute(ID3D12Device* device, ExecuteObserver ob);
void overlayStopObservingExecute();

// Save request from the menu ("Salva" button). The callback returns true on success; `error` is
// shown in the menu when it returns false.
using MenuSaveFn = bool (*)(std::string& error);
void menuSetSaveCallback(MenuSaveFn fn);
// Invokes the save callback (returns false and sets `error` if none is registered).
bool menuRequestSave(std::string& error);
// Mod version shown in the "Info" tab (main.cpp kVersion).
void menuSetVersionInfo(const char* modVersion);

}  // namespace qs
