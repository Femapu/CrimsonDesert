// QuattroStagioni - read-only view of a PE32+ image laid out as mapped in memory (RVA addressing).
//
// Portable: the runtime probes build it over the live game module (bytes = module base), the unit
// tests over a synthetic buffer. Section names are NOT trusted: Denuvo scrambles them per build
// (recon C: game code was ".text1" in 2.04.00 and ".impdata" in the 30/10 hotfix, the protector
// section ".code" then ".00cfg"). Sections are classified by characteristics and position:
//   Code           the primary executable section (lowest RVA, not writable, no entry point when
//                  there is an alternative): plain MSVC game code, safe to scan and hook
//   Obfuscated     every other executable section (protector VM, relocated/mutated functions,
//                  RWX, the entry point): never scanned for signatures, never patched or hooked
//   ReadOnlyData   initialised read-only data (RTTI, vtables, strings)
//   Data           writable data (globals, singletons, TypeDescriptors)
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace qs::probe {

enum class SectionKind : uint8_t { Code, Obfuscated, ReadOnlyData, Data, Other };
const char* toString(SectionKind k);

constexpr uint32_t kScnCntCode = 0x00000020;
constexpr uint32_t kScnMemExecute = 0x20000000;
constexpr uint32_t kScnMemRead = 0x40000000;
constexpr uint32_t kScnMemWrite = 0x80000000;
constexpr uint32_t kScnMemDiscardable = 0x02000000;

struct Section {
    std::string name;       // as found (8 chars max), informative only
    uint32_t rva = 0;
    uint32_t vsize = 0;
    uint32_t rawSize = 0;
    uint32_t characteristics = 0;
    SectionKind kind = SectionKind::Other;
    bool accessible = true;  // the in-process view clears it if VirtualQuery says the range is not readable

    bool executable() const { return (characteristics & (kScnMemExecute | kScnCntCode)) != 0; }
    bool writable() const { return (characteristics & kScnMemWrite) != 0; }
    uint32_t end() const { return rva + vsize; }
    bool contains(uint64_t r, uint64_t n = 1) const { return r >= rva && r - rva < vsize && n <= vsize - (r - rva); }
};

// Classifies in place (see header comment). Exposed for tests.
void classifySections(std::vector<Section>& sections, uint32_t entryRva);

struct ImageView {
    const uint8_t* bytes = nullptr;  // image base (mapped layout)
    uint64_t size = 0;               // SizeOfImage
    uint64_t ptrBase = 0;            // base used by absolute pointers in the image (runtime base in-process)
    uint64_t preferredBase = 0;      // OptionalHeader.ImageBase
    uint32_t entryRva = 0;
    uint32_t timeDateStamp = 0;
    uint32_t exceptionDirRva = 0, exceptionDirSize = 0;  // .pdata (RUNTIME_FUNCTION array)
    std::vector<Section> sections;

    const Section* sectionOf(uint64_t rva) const;
    SectionKind kindAt(uint64_t rva) const;
    // True if [rva, rva+n) lies inside one accessible section.
    bool readable(uint64_t rva, uint64_t n) const;
    template <class T>
    bool read(uint64_t rva, T& out) const {
        if (!readable(rva, sizeof(T))) return false;
        std::memcpy(&out, bytes + rva, sizeof(T));
        return true;
    }
    // Pointer value (absolute) -> RVA, or UINT64_MAX if outside the image.
    uint64_t toRva(uint64_t ptr) const { return ptr >= ptrBase && ptr < ptrBase + size ? ptr - ptrBase : UINT64_MAX; }
    const Section* primaryCode() const;  // nullptr if none
    // NUL-terminated string at rva (bounded), empty if unreadable.
    std::string cstr(uint64_t rva, size_t maxLen = 512) const;
    // "0x1234 [Code +0x234]" style description for logs.
    std::string describe(uint64_t rva) const;
};

// Parses DOS/NT headers and the section table from a mapped image. ptrBase: the base pointers in
// the image are relative to (module base in-process; tests pass the preferred base).
bool parseMappedImage(const uint8_t* image, uint64_t size, uint64_t ptrBase, ImageView& out, std::string* err = nullptr);

// x64 code helpers ---------------------------------------------------------------------------
// If the bytes at rva are "E9 rel32" or "FF 25 disp32" (jmp [rip+x], pointer readable in the view)
// returns true and the destination RVA (UINT64_MAX if the destination is outside the image).
bool decodeJump(const ImageView& v, uint64_t rva, uint64_t& destRva);

}  // namespace qs::probe
