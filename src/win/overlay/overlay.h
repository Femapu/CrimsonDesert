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

// Save request from the menu ("Salva" button). The callback returns true on success; `error` is
// shown in the menu when it returns false.
using MenuSaveFn = bool (*)(std::string& error);
void menuSetSaveCallback(MenuSaveFn fn);
// Invokes the save callback (returns false and sets `error` if none is registered).
bool menuRequestSave(std::string& error);
// Mod version shown in the "Info" tab (main.cpp kVersion).
void menuSetVersionInfo(const char* modVersion);

}  // namespace qs
