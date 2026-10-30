// QuattroStagioni - in-process view of the game's main module.
#include "module.h"

#include <cstring>

#include "memory.h"

namespace qs::probe {

bool describeModule(HMODULE module, ModuleInfo& out, std::string* err) {
    out = ModuleInfo();
    out.handle = module ? module : GetModuleHandleW(nullptr);
    out.base = reinterpret_cast<uintptr_t>(out.handle);
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(out.handle, buf, DWORD(sizeof(buf) / sizeof(buf[0])));
    out.path.assign(buf, n);
    // SizeOfImage from the headers (read safely: headers are one committed page)
    uint8_t hdr[0x400];
    if (!mem::read(out.base, hdr, sizeof(hdr))) {
        if (err) *err = "module headers unreadable";
        return false;
    }
    uint32_t peOff = 0, sizeOfImage = 0;
    std::memcpy(&peOff, hdr + 0x3C, 4);
    if (peOff > sizeof(hdr) - 0x60) {
        if (err) *err = "unexpected PE header offset";
        return false;
    }
    std::memcpy(&sizeOfImage, hdr + peOff + 24 + 56, 4);
    if (!parseMappedImage(reinterpret_cast<const uint8_t*>(out.base), sizeOfImage, out.base, out.view, err)) return false;
    for (Section& s : out.view.sections) {
        if (s.kind == SectionKind::Obfuscated) {
            s.accessible = false;  // never touch protector pages
            continue;
        }
        s.accessible = s.vsize && mem::isReadable(reinterpret_cast<const void*>(out.base + s.rva), s.vsize);
    }
    return true;
}

std::vector<uint64_t> scanModule(const ModuleInfo& m, const Pattern& p, bool inData, size_t maxHits) {
    std::vector<uint64_t> out;
    for (const Section& s : m.view.sections) {
        if (!s.accessible) continue;
        if (inData ? s.executable() : s.kind != SectionKind::Code) continue;
        std::vector<size_t> h = findAll(m.view.bytes + s.rva, s.vsize, p, maxHits - out.size());
        for (size_t o : h) out.push_back(s.rva + o);
        if (out.size() >= maxHits) break;
    }
    return out;
}

}  // namespace qs::probe
