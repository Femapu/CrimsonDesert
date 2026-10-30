// QuattroStagioni - change detection for probe snapshots.
#include "diff.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace qs::probe {
namespace {

template <class T>
T load(const uint8_t* p) {
    T v;
    std::memcpy(&v, p, sizeof(T));
    return v;
}

std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

bool plausibleFloat(float f) { return std::isfinite(f) && std::fabs(f) < 1e7f; }

}  // namespace

bool parseFieldType(const std::string& sIn, FieldType& out) {
    const std::string s = lower(sIn);
    struct { const char* n; FieldType t; } map[] = {
        {"u8", FieldType::U8},   {"i8", FieldType::I8},   {"u16", FieldType::U16}, {"i16", FieldType::I16},
        {"u32", FieldType::U32}, {"i32", FieldType::I32}, {"u64", FieldType::U64}, {"i64", FieldType::I64},
        {"f32", FieldType::F32}, {"float", FieldType::F32}, {"f64", FieldType::F64}, {"double", FieldType::F64},
        {"ptr", FieldType::Ptr}, {"bool", FieldType::U8},
    };
    for (const auto& m : map)
        if (s == m.n) {
            out = m.t;
            return true;
        }
    return false;
}

const char* toString(FieldType t) {
    switch (t) {
        case FieldType::U8: return "u8";
        case FieldType::I8: return "i8";
        case FieldType::U16: return "u16";
        case FieldType::I16: return "i16";
        case FieldType::U32: return "u32";
        case FieldType::I32: return "i32";
        case FieldType::U64: return "u64";
        case FieldType::I64: return "i64";
        case FieldType::F32: return "f32";
        case FieldType::F64: return "f64";
        case FieldType::Ptr: return "ptr";
    }
    return "?";
}

size_t fieldSize(FieldType t) {
    switch (t) {
        case FieldType::U8: case FieldType::I8: return 1;
        case FieldType::U16: case FieldType::I16: return 2;
        case FieldType::U32: case FieldType::I32: case FieldType::F32: return 4;
        default: return 8;
    }
}

double fieldAsDouble(FieldType t, const uint8_t* p) {
    switch (t) {
        case FieldType::U8: return load<uint8_t>(p);
        case FieldType::I8: return load<int8_t>(p);
        case FieldType::U16: return load<uint16_t>(p);
        case FieldType::I16: return load<int16_t>(p);
        case FieldType::U32: return load<uint32_t>(p);
        case FieldType::I32: return load<int32_t>(p);
        case FieldType::U64: case FieldType::Ptr: return double(load<uint64_t>(p));
        case FieldType::I64: return double(load<int64_t>(p));
        case FieldType::F32: return load<float>(p);
        case FieldType::F64: return load<double>(p);
    }
    return 0;
}

std::string formatField(FieldType t, const uint8_t* p) {
    char b[64];
    switch (t) {
        case FieldType::F32: std::snprintf(b, sizeof(b), "%.6g", double(load<float>(p))); break;
        case FieldType::F64: std::snprintf(b, sizeof(b), "%.9g", load<double>(p)); break;
        case FieldType::Ptr: std::snprintf(b, sizeof(b), "0x%llX", (unsigned long long)load<uint64_t>(p)); break;
        case FieldType::U64: std::snprintf(b, sizeof(b), "%llu", (unsigned long long)load<uint64_t>(p)); break;
        case FieldType::I64: std::snprintf(b, sizeof(b), "%lld", (long long)load<int64_t>(p)); break;
        default: std::snprintf(b, sizeof(b), "%.0f", fieldAsDouble(t, p)); break;
    }
    return b;
}

bool fieldChanged(FieldType t, const uint8_t* a, const uint8_t* b, double eps) {
    if (t == FieldType::F32 || t == FieldType::F64) {
        double x = fieldAsDouble(t, a), y = fieldAsDouble(t, b);
        if (std::isnan(x) || std::isnan(y)) return std::isnan(x) != std::isnan(y);
        return std::fabs(x - y) > eps;
    }
    return std::memcmp(a, b, fieldSize(t)) != 0;
}

std::vector<ChangedRange> diffBytes(const uint8_t* a, const uint8_t* b, size_t n, size_t mergeGap) {
    std::vector<ChangedRange> out;
    size_t i = 0;
    while (i < n) {
        if (a[i] == b[i]) {
            ++i;
            continue;
        }
        size_t start = i, lastDiff = i;
        for (++i; i < n; ++i) {
            if (a[i] != b[i]) lastDiff = i;
            else if (i - lastDiff > mergeGap) break;
        }
        out.push_back({start, lastDiff - start + 1});
        i = lastDiff + 1;
    }
    return out;
}

std::vector<ChangedRange> alignRanges(const std::vector<ChangedRange>& in, size_t n, size_t align) {
    std::vector<ChangedRange> out;
    for (const ChangedRange& r : in) {
        size_t s = r.offset / align * align;
        size_t e = std::min(n, (r.offset + r.length + align - 1) / align * align);
        if (!out.empty() && s <= out.back().offset + out.back().length) {
            out.back().length = std::max(out.back().offset + out.back().length, e) - out.back().offset;
        } else {
            out.push_back({s, e - s});
        }
    }
    return out;
}

std::string hexBytes(const uint8_t* p, size_t n) {
    static const char* hx = "0123456789abcdef";
    std::string s;
    s.reserve(n * 3);
    for (size_t i = 0; i < n; ++i) {
        if (i) s += ' ';
        s += hx[p[i] >> 4];
        s += hx[p[i] & 15];
    }
    return s;
}

std::string describeDwords(size_t base, const uint8_t* p, size_t n) {
    std::string out;
    char b[160];
    for (size_t i = 0; i + 4 <= n; i += 4) {
        float f = load<float>(p + i);
        uint32_t u = load<uint32_t>(p + i);
        if (plausibleFloat(f) && (u == 0 || std::fabs(f) > 1e-6f))
            std::snprintf(b, sizeof(b), "%s+0x%zX: %s (f32 %.6g, u32 %u)", out.empty() ? "" : "; ", base + i,
                          hexBytes(p + i, 4).c_str(), double(f), u);
        else
            std::snprintf(b, sizeof(b), "%s+0x%zX: %s (u32 %u)", out.empty() ? "" : "; ", base + i,
                          hexBytes(p + i, 4).c_str(), u);
        out += b;
    }
    if (n % 4) out += std::string(out.empty() ? "" : "; ") + "tail " + hexBytes(p + n / 4 * 4, n % 4);
    return out;
}

void FieldStats::reset(size_t size) {
    size_ = size;
    samples_ = 0;
    last_.assign(size, 0);
    cands_.assign(size / 4, Candidate());
    for (size_t i = 0; i < cands_.size(); ++i) cands_[i].offset = i * 4;
}

void FieldStats::add(const uint8_t* snap) {
    for (Candidate& c : cands_) {
        const uint8_t* p = snap + c.offset;
        float f = load<float>(p);
        uint32_t u = load<uint32_t>(p);
        if (samples_ == 0) {
            c.minF = c.maxF = f;
            c.minU = c.maxU = u;
            c.floatValid = plausibleFloat(f);
        } else {
            if (u != c.lastU) ++c.changes;
            c.minF = std::min(c.minF, f);
            c.maxF = std::max(c.maxF, f);
            c.minU = std::min(c.minU, u);
            c.maxU = std::max(c.maxU, u);
            c.floatValid = c.floatValid && plausibleFloat(f);
        }
        c.lastF = f;
        c.lastU = u;
    }
    std::memcpy(last_.data(), snap, size_);
    ++samples_;
}

std::vector<FieldStats::Candidate> FieldStats::floatsInRange(float lo, float hi, uint32_t minChanges) const {
    std::vector<Candidate> out;
    for (const Candidate& c : cands_)
        if (c.floatValid && c.changes >= minChanges && c.minF >= lo && c.maxF <= hi) out.push_back(c);
    return out;
}

std::vector<FieldStats::Candidate> FieldStats::mostActive(size_t maxResults, uint32_t minChanges) const {
    std::vector<Candidate> out;
    for (const Candidate& c : cands_)
        if (c.changes >= minChanges) out.push_back(c);
    std::stable_sort(out.begin(), out.end(), [](const Candidate& a, const Candidate& b) { return a.changes > b.changes; });
    if (out.size() > maxResults) out.resize(maxResults);
    return out;
}

}  // namespace qs::probe
