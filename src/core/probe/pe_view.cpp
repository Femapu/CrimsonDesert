// QuattroStagioni - read-only view of a mapped PE32+ image.
#include "pe_view.h"

#include <cstdio>

namespace qs::probe {

const char* toString(SectionKind k) {
    switch (k) {
        case SectionKind::Code: return "code";
        case SectionKind::Obfuscated: return "OBFUSCATED";
        case SectionKind::ReadOnlyData: return "rdata";
        case SectionKind::Data: return "data";
        case SectionKind::Other: return "other";
    }
    return "?";
}

void classifySections(std::vector<Section>& secs, uint32_t entryRva) {
    // Game code comes first in the image (RVA 0x1000 in every build seen so far); the protector
    // appends its sections after it. The entry point is not used as a criterion (it lies in the
    // protector for the game, in the code section for a normal exe) but is kept for logging.
    (void)entryRva;
    size_t primary = SIZE_MAX;
    for (size_t i = 0; i < secs.size(); ++i)
        if (secs[i].executable() && !secs[i].writable() && secs[i].vsize && (primary == SIZE_MAX || secs[i].rva < secs[primary].rva))
            primary = i;
    for (size_t i = 0; i < secs.size(); ++i) {
        Section& s = secs[i];
        if (s.executable()) s.kind = (i == primary) ? SectionKind::Code : SectionKind::Obfuscated;
        else if (s.writable()) s.kind = SectionKind::Data;
        else if (s.characteristics & kScnMemRead) s.kind = SectionKind::ReadOnlyData;
        else s.kind = SectionKind::Other;
    }
}

const Section* ImageView::sectionOf(uint64_t rva) const {
    for (const Section& s : sections)
        if (s.contains(rva)) return &s;
    return nullptr;
}

SectionKind ImageView::kindAt(uint64_t rva) const {
    const Section* s = sectionOf(rva);
    return s ? s->kind : SectionKind::Other;
}

bool ImageView::readable(uint64_t rva, uint64_t n) const {
    if (!bytes || rva + n > size || rva + n < rva) return false;
    const Section* s = sectionOf(rva);
    return s && s->accessible && s->contains(rva, n);
}

const Section* ImageView::primaryCode() const {
    for (const Section& s : sections)
        if (s.kind == SectionKind::Code) return &s;
    return nullptr;
}

std::string ImageView::cstr(uint64_t rva, size_t maxLen) const {
    std::string out;
    const Section* s = sectionOf(rva);
    if (!s || !s->accessible) return out;
    for (uint64_t r = rva; r < s->end() && out.size() < maxLen; ++r) {
        char c = char(bytes[r]);
        if (!c) break;
        out += c;
    }
    return out;
}

std::string ImageView::describe(uint64_t rva) const {
    char buf[96];
    const Section* s = sectionOf(rva);
    if (!s) std::snprintf(buf, sizeof(buf), "0x%llX [outside sections]", (unsigned long long)rva);
    else
        std::snprintf(buf, sizeof(buf), "0x%llX [%s %s+0x%llX]", (unsigned long long)rva, toString(s->kind),
                      s->name.c_str(), (unsigned long long)(rva - s->rva));
    return buf;
}

bool parseMappedImage(const uint8_t* img, uint64_t size, uint64_t ptrBase, ImageView& out, std::string* err) {
    auto bad = [&](const char* m) {
        if (err) *err = m;
        return false;
    };
    out = ImageView();
    if (!img || size < 0x400) return bad("image too small");
    if (img[0] != 'M' || img[1] != 'Z') return bad("no MZ header");
    uint32_t peOff;
    std::memcpy(&peOff, img + 0x3C, 4);
    if (peOff > size - 0x108 || std::memcmp(img + peOff, "PE\0\0", 4) != 0) return bad("no PE header");
    const uint8_t* fh = img + peOff + 4;  // IMAGE_FILE_HEADER
    uint16_t machine, nsec, optSize;
    std::memcpy(&machine, fh, 2);
    std::memcpy(&nsec, fh + 2, 2);
    std::memcpy(&out.timeDateStamp, fh + 4, 4);
    std::memcpy(&optSize, fh + 16, 2);
    if (machine != 0x8664) return bad("not an x64 image");
    const uint8_t* oh = fh + 20;  // IMAGE_OPTIONAL_HEADER64
    uint16_t magic;
    std::memcpy(&magic, oh, 2);
    if (magic != 0x20B || optSize < 112) return bad("not PE32+");
    uint32_t sizeOfImage, numDirs;
    std::memcpy(&out.entryRva, oh + 16, 4);
    std::memcpy(&out.preferredBase, oh + 24, 8);
    std::memcpy(&sizeOfImage, oh + 56, 4);
    std::memcpy(&numDirs, oh + 108, 4);
    if (numDirs > 3 && optSize >= 112 + 4 * 8) {
        std::memcpy(&out.exceptionDirRva, oh + 112 + 3 * 8, 4);
        std::memcpy(&out.exceptionDirSize, oh + 112 + 3 * 8 + 4, 4);
    }
    const uint8_t* sh = oh + optSize;
    if (uint64_t(sh - img) + uint64_t(nsec) * 40 > size) return bad("section table out of range");
    out.bytes = img;
    out.size = sizeOfImage < size ? sizeOfImage : size;
    out.ptrBase = ptrBase;
    for (uint16_t i = 0; i < nsec; ++i, sh += 40) {
        Section s;
        char name[9] = {};
        std::memcpy(name, sh, 8);
        s.name = name;
        std::memcpy(&s.vsize, sh + 8, 4);
        std::memcpy(&s.rva, sh + 12, 4);
        std::memcpy(&s.rawSize, sh + 16, 4);
        std::memcpy(&s.characteristics, sh + 36, 4);
        if (s.vsize == 0) s.vsize = s.rawSize;
        if (uint64_t(s.rva) + s.vsize > out.size) {  // clamp, never trust the table blindly
            s.vsize = s.rva < out.size ? uint32_t(out.size - s.rva) : 0;
        }
        out.sections.push_back(s);
    }
    classifySections(out.sections, out.entryRva);
    return true;
}

bool decodeJump(const ImageView& v, uint64_t rva, uint64_t& dest) {
    uint8_t b[6];
    if (!v.readable(rva, 5)) return false;
    std::memcpy(b, v.bytes + rva, 5);
    if (b[0] == 0xE9) {
        int32_t d;
        std::memcpy(&d, b + 1, 4);
        uint64_t t = rva + 5 + uint64_t(int64_t(d));
        dest = t < v.size ? t : UINT64_MAX;
        return true;
    }
    if (b[0] == 0xFF && b[1] == 0x25 && v.readable(rva, 6)) {
        int32_t d;
        std::memcpy(&d, v.bytes + rva + 2, 4);
        uint64_t slot = rva + 6 + uint64_t(int64_t(d)), ptr;
        if (!v.read(slot, ptr)) return false;
        dest = v.toRva(ptr);
        return true;
    }
    return false;
}

}  // namespace qs::probe
