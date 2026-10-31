// QuattroStagioni - the only way the applier touches game memory.
//
// Reads: probe::mem::read (VirtualQuery + ReadProcessMemory, a wrong offset fails instead of
// crashing). Writes are refused unless EVERY page of the range is:
//   committed, not a guard page, and either private memory with protection exactly PAGE_READWRITE
//   (game heap; no execute) or a page of the main exe's writable DATA section (globals; image pages
//   are PAGE_WRITECOPY until first written). Code, read-only data (vtables, RTTI), the protector
//   section (RWX), mapped files and other modules are never written.
// The store is done with WriteProcessMemory on our own process (a page freed between the check and
// the store fails instead of faulting) and verified by reading it back.
#pragma once
#include <string>

#include "../../core/apply/applier.h"
#include "../probe/module.h"

namespace qs::apply {

enum class WriteCheck { Ok, Unmapped, NotReadWrite, Guard, ImageNotData, OtherModule, ForeignMapping };
const char* toString(WriteCheck c);
// Classifies [addr, addr+n) for a write (exposed for the fake-game self test).
WriteCheck checkWritable(const probe::ModuleInfo& m, uintptr_t addr, size_t n);

class GuardedMemory : public Memory {
public:
    explicit GuardedMemory(const probe::ModuleInfo& m) : mod_(m) {}
    bool read(uint64_t addr, void* out, size_t n) override;
    bool write(uint64_t addr, const void* in, size_t n, std::string* why) override;
    // Same checks and readback as write, for a block of up to kMaxBlock bytes (write takes 1..8:
    // the applier's scalars). Used by the climate maps (512x512 texels, 1 MB).
    static constexpr size_t kMaxBlock = 16u << 20;
    bool writeBlock(uint64_t addr, const void* in, size_t n, std::string* why);

private:
    const probe::ModuleInfo& mod_;
};

}  // namespace qs::apply
