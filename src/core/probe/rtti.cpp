// QuattroStagioni - MSVC x64 RTTI resolver.
#include "rtti.h"

#include <algorithm>

#include "pattern.h"

namespace qs::probe {
namespace {

char lowerc(char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; }

bool globRec(const char* p, const char* t) {
    while (*p) {
        if (*p == '*') {
            while (*p == '*') ++p;
            if (!*p) return true;
            for (; *t; ++t)
                if (globRec(p, t)) return true;
            return false;
        }
        if (!*t || lowerc(*p) != lowerc(*t)) return false;
        ++p, ++t;
    }
    return *t == 0;
}

// Layout constants (MSVC x64, image-relative RTTI).
constexpr uint32_t kColSize = 24;
constexpr uint32_t kTdNameOffset = 16;

}  // namespace

std::string mangleClassName(const std::string& name, bool isStruct) {
    if (name.compare(0, 3, ".?A") == 0) return name;
    std::vector<std::string> parts;
    size_t p = 0;
    while (true) {
        size_t c = name.find("::", p);
        parts.push_back(name.substr(p, c == std::string::npos ? std::string::npos : c - p));
        if (c == std::string::npos) break;
        p = c + 2;
    }
    std::string out = isStruct ? ".?AU" : ".?AV";
    for (size_t i = parts.size(); i-- > 0;) out += parts[i] + "@";
    out += "@";
    return out;
}

std::string demangleClassName(const std::string& m) {
    if (m.size() < 7 || m.compare(0, 3, ".?A") != 0 || (m[3] != 'V' && m[3] != 'U')) return m;
    if (m.find("?$") != std::string::npos || m.compare(m.size() - 2, 2, "@@") != 0) return m;
    std::string body = m.substr(4, m.size() - 6);
    std::vector<std::string> parts;
    size_t p = 0;
    while (true) {
        size_t c = body.find('@', p);
        parts.push_back(body.substr(p, c == std::string::npos ? std::string::npos : c - p));
        if (c == std::string::npos) break;
        p = c + 1;
    }
    std::string out;
    for (size_t i = parts.size(); i-- > 0;) {
        out += parts[i];
        if (i) out += "::";
    }
    return out;
}

bool globMatch(const std::string& pattern, const std::string& text) { return globRec(pattern.c_str(), text.c_str()); }

size_t RttiIndex::build(const ImageView& v) {
    view_ = &v;
    cols_.clear();
    byTd_.clear();
    byVtable_.clear();
    // 1. COLs: signature 1, pSelf == own RVA, TypeDescriptor name starts with ".?A".
    for (const Section& s : v.sections) {
        if (s.executable() || !s.accessible || s.vsize < kColSize) continue;
        const uint32_t start = (s.rva + 3) & ~3u;
        for (uint32_t r = start; r + kColSize <= s.end(); r += 4) {
            const uint8_t* p = v.bytes + r;
            if (readU32(p) != 1 || readU32(p + 20) != r) continue;
            ColInfo c;
            c.rva = r;
            c.offset = readU32(p + 4);
            c.cdOffset = readU32(p + 8);
            c.tdRva = readU32(p + 12);
            c.chdRva = readU32(p + 16);
            char head[4];
            if (!v.readable(uint64_t(c.tdRva) + kTdNameOffset, 4) || !v.readable(c.chdRva, 16)) continue;
            std::memcpy(head, v.bytes + c.tdRva + kTdNameOffset, 4);
            if (head[0] != '.' || head[1] != '?' || head[2] != 'A') continue;
            cols_.push_back(c);
        }
    }
    std::sort(cols_.begin(), cols_.end(), [](const ColInfo& a, const ColInfo& b) { return a.rva < b.rva; });
    // 2. vtables: an 8-aligned qword equal to (ptrBase + colRva); slot 0 follows it.
    std::vector<uint32_t> colRvas;
    colRvas.reserve(cols_.size());
    for (const ColInfo& c : cols_) colRvas.push_back(c.rva);
    for (const Section& s : v.sections) {
        if (s.executable() || !s.accessible || s.vsize < 16) continue;
        const uint32_t start = (s.rva + 7) & ~7u;
        for (uint32_t r = start; r + 16 <= s.end(); r += 8) {
            uint64_t q = readU64(v.bytes + r);
            if (q < v.ptrBase || q >= v.ptrBase + v.size) continue;
            uint32_t target = uint32_t(q - v.ptrBase);
            auto it = std::lower_bound(colRvas.begin(), colRvas.end(), target);
            if (it == colRvas.end() || *it != target) continue;
            ColInfo& c = cols_[size_t(it - colRvas.begin())];
            // slot 0 must point into an executable section, otherwise it is just a COL reference
            uint64_t slot0 = readU64(v.bytes + r + 8);
            const Section* ss = v.sectionOf(v.toRva(slot0));
            if (!ss || !ss->executable()) continue;
            if (!c.vtableRefs) c.vtableRva = r + 8;
            ++c.vtableRefs;
        }
    }
    for (uint32_t i = 0; i < cols_.size(); ++i) {
        byTd_[cols_[i].tdRva].push_back(i);
        if (cols_[i].vtableRva) byVtable_[cols_[i].vtableRva] = i;
    }
    return cols_.size();
}

std::string RttiIndex::typeName(uint32_t tdRva) const {
    return view_ ? view_->cstr(uint64_t(tdRva) + kTdNameOffset, 1024) : std::string();
}

RttiClass RttiIndex::makeClass(uint32_t tdRva) const {
    RttiClass rc;
    rc.tdRva = tdRva;
    rc.mangled = typeName(tdRva);
    rc.pretty = demangleClassName(rc.mangled);
    auto it = byTd_.find(tdRva);
    if (it != byTd_.end())
        for (uint32_t i : it->second) rc.cols.push_back(cols_[i]);
    std::stable_sort(rc.cols.begin(), rc.cols.end(), [](const ColInfo& a, const ColInfo& b) { return a.offset < b.offset; });
    return rc;
}

bool RttiIndex::find(const std::string& name, RttiClass& out) const {
    if (!view_) return false;
    std::vector<std::string> wanted;
    if (name.compare(0, 3, ".?A") == 0) {
        wanted.push_back(name);
    } else {
        wanted.push_back(mangleClassName(name, false));
        wanted.push_back(mangleClassName(name, true));
    }
    for (const std::string& w : wanted)
        for (const auto& kv : byTd_)
            if (typeName(kv.first) == w) {
                out = makeClass(kv.first);
                return true;
            }
    return false;
}

std::vector<RttiClass> RttiIndex::findGlob(const std::string& glob, size_t maxResults) const {
    std::vector<RttiClass> out;
    if (!view_) return out;
    const bool mangledGlob = glob.compare(0, 3, ".?A") == 0;
    std::vector<uint32_t> tds;
    for (const auto& kv : byTd_) tds.push_back(kv.first);
    std::sort(tds.begin(), tds.end());
    for (uint32_t td : tds) {
        std::string m = typeName(td);
        if (!globMatch(glob, mangledGlob ? m : demangleClassName(m))) continue;
        out.push_back(makeClass(td));
        if (out.size() >= maxResults) break;
    }
    return out;
}

std::vector<BaseClass> RttiIndex::baseClasses(const ColInfo& col) const {
    std::vector<BaseClass> out;
    if (!view_) return out;
    uint32_t num = 0, arr = 0;
    if (!view_->read(uint64_t(col.chdRva) + 8, num) || !view_->read(uint64_t(col.chdRva) + 12, arr)) return out;
    if (num > 256) return out;  // corrupt
    for (uint32_t i = 0; i < num; ++i) {
        uint32_t bcd = 0, td = 0;
        if (!view_->read(uint64_t(arr) + 4 * i, bcd) || !view_->read(bcd, td)) break;
        BaseClass b;
        b.mangled = typeName(td);
        view_->read(uint64_t(bcd) + 4, b.containedBases);
        view_->read(uint64_t(bcd) + 8, b.mdisp);
        out.push_back(b);
    }
    return out;
}

uint32_t RttiIndex::vtableSlots(uint32_t vt, uint32_t maxSlots) const {
    if (!view_) return 0;
    uint32_t n = 0;
    for (; n < maxSlots; ++n) {
        uint64_t q;
        if (!view_->read(uint64_t(vt) + 8ull * n, q)) break;
        const Section* s = view_->sectionOf(view_->toRva(q));
        if (!s || !s->executable()) break;
    }
    return n;
}

const ColInfo* RttiIndex::colForVtable(uint32_t vt) const {
    auto it = byVtable_.find(vt);
    return it == byVtable_.end() ? nullptr : &cols_[it->second];
}

}  // namespace qs::probe
