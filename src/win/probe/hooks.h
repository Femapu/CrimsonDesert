// QuattroStagioni - guarded hook helpers for the runtime probes (and later the game-system hooks).
//
// Two mechanisms, both restricted by the section classification of module.h:
//   VtableSlotHook  swaps one pointer in a vtable (vtables live in read-only data; the page is made
//                   writable for the single 8-byte store, then protection is restored). No code byte
//                   of the game changes. restore() puts the original back only if the slot still
//                   holds our pointer.
//   GuardedMinHook  MinHook detour, allowed ONLY on targets inside the primary code section. A
//                   5-byte "jmp rel32" stub into the obfuscated section is accepted (MinHook relocates
//                   the jmp; the protected body is never touched), anything inside an obfuscated
//                   section is refused (recon C: Denuvo relocated/mutated functions).
// CaptureHook combines either one with the capture stub (core/probe/capture_stub.h) to record the
// `this` pointers (and callers) of a virtual or plain function without changing its behaviour.
#pragma once
#include <windows.h>

#include <cstdint>
#include <string>

#include "module.h"

namespace qs::probe {

enum class TargetCheck { Ok, OkJmpStub, Outside, Obfuscated, NotCode, Unreadable };
const char* toString(TargetCheck c);
// Classifies a hook target address. *stubDestRva receives the jmp destination for OkJmpStub.
TargetCheck checkHookTarget(const ModuleInfo& m, uintptr_t target, uint64_t* stubDestRva = nullptr);

class VtableSlotHook {
public:
    bool install(const ModuleInfo& m, uintptr_t vtable, int slot, void* replacement, std::string* why);
    bool restore(std::string* why);
    bool active() const { return slot_ != nullptr; }
    void* original() const { return original_; }

private:
    void** slot_ = nullptr;
    void* original_ = nullptr;
    void* replacement_ = nullptr;
};

class GuardedMinHook {
public:
    // detour receives control; *original (out) is the trampoline to call the game function.
    bool install(const ModuleInfo& m, uintptr_t target, void* detour, void** original, std::string* why);
    bool remove();
    bool active() const { return target_ != 0; }

    // Shared MinHook lifetime: initialise if nobody did (io_hooks may already own it).
    static bool ensureInitialized();
    static void releaseIfOwned();  // uninitialises only if we initialised and no probe hook is left

private:
    uintptr_t target_ = 0;
};

// Lock-free record of what a hooked function was called with.
struct CaptureSink {
    static constexpr int kMaxObjects = 64;
    static constexpr int kMaxCallers = 16;
    volatile LONG64 calls = 0;
    volatile LONG64 objects[kMaxObjects] = {};
    volatile LONG64 callers[kMaxCallers] = {};
    LONG maxObjects = 4;
    bool useRdx = false;
};

class CaptureHook {
public:
    // Vtable slot capture.
    bool installVtable(const ModuleInfo& m, uintptr_t vtable, int slot, CaptureSink* sink, std::string* why);
    // MinHook capture on a code-section function.
    bool installFunction(const ModuleInfo& m, uintptr_t target, CaptureSink* sink, std::string* why);
    void remove();
    bool active() const { return vt_.active() || mh_.active(); }

private:
    uint8_t* makeStub(CaptureSink* sink, uint64_t original, size_t* slotOffset);
    VtableSlotHook vt_;
    GuardedMinHook mh_;
    // Stubs are intentionally never freed: a game thread may still be executing one after removal.
};

}  // namespace qs::probe
