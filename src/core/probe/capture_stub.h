// QuattroStagioni - machine code for the generic "capture this" detour (portable byte generator).
//
// A capture probe redirects a vtable slot (or a MinHook detour on a code-section function) to a
// small per-probe stub that:
//   1. saves every argument register (rcx, rdx, r8, r9, xmm0-xmm3) plus rax, r10, r11
//   2. calls  void callback(void* ctx, void* rcx, void* returnAddress, void* rdx)  (MS x64 ABI)
//   3. restores the registers and jumps to the original function through an absolute pointer
// so the game function runs unchanged with its original arguments. The callback only records the
// pointers (lock-free, no allocation, no logging) and returns.
// The bytes are generated here so the encoding is unit tested natively; src/win/probe/ copies
// them into executable memory.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace qs::probe {

struct CaptureStub {
    std::vector<uint8_t> code;
    size_t originalSlotOffset = 0;  // offset of the 8-byte absolute address of the original function
};

CaptureStub buildCaptureStub(uint64_t ctx, uint64_t callback, uint64_t original);
// Rewrites the original-function pointer inside an already generated stub buffer.
void setStubOriginal(uint8_t* stubCode, const CaptureStub& layout, uint64_t original);

}  // namespace qs::probe
