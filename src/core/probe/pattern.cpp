// QuattroStagioni - IDA-style byte patterns and scanning.
#include "pattern.h"

#include <cstdlib>
#include <cstring>

namespace qs::probe {
namespace {

int hexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool fail(std::string* err, const std::string& msg) {
    if (err) *err = msg;
    return false;
}

void computeAnchor(Pattern& p) {
    size_t bestOff = 0, bestLen = 0, runOff = 0, runLen = 0;
    for (size_t i = 0; i < p.size(); ++i) {
        if (p.mask[i]) {
            if (runLen == 0) runOff = i;
            ++runLen;
            if (runLen > bestLen) {
                bestLen = runLen;
                bestOff = runOff;
            }
        } else {
            runLen = 0;
        }
    }
    p.anchorOffset = bestOff;
    p.anchorLength = bestLen;
}

}  // namespace

bool Pattern::matchesAt(const uint8_t* p) const {
    for (size_t i = 0; i < bytes.size(); ++i)
        if (mask[i] && p[i] != bytes[i]) return false;
    return true;
}

bool parsePattern(const std::string& text, Pattern& out, std::string* err) {
    out = Pattern();
    std::vector<std::string> tokens;
    std::string cur;
    bool hasSpace = false;
    for (char c : text) {
        if (c == ' ' || c == '\t' || c == ',') {
            hasSpace = true;
            if (!cur.empty()) tokens.push_back(cur), cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) tokens.push_back(cur);
    if (!hasSpace && tokens.size() == 1 && tokens[0].size() > 2) {
        // compact form: split into pairs, "??" for wildcards (a lone "?" is ambiguous here)
        std::string t = tokens[0];
        if (t.size() % 2) return fail(err, "compact pattern needs an even number of characters");
        tokens.clear();
        for (size_t i = 0; i < t.size(); i += 2) tokens.push_back(t.substr(i, 2));
    }
    for (const std::string& t : tokens) {
        if (t == "?" || t == "??") {
            out.bytes.push_back(0);
            out.mask.push_back(0);
            continue;
        }
        if (t.size() != 2) return fail(err, "bad token '" + t + "'");
        int a = hexNibble(t[0]), b = hexNibble(t[1]);
        if (a < 0 || b < 0) return fail(err, "bad token '" + t + "'");
        out.bytes.push_back(uint8_t(a * 16 + b));
        out.mask.push_back(1);
    }
    if (out.bytes.empty()) return fail(err, "empty pattern");
    computeAnchor(out);
    if (out.anchorLength == 0) return fail(err, "pattern has no fixed byte");
    return true;
}

std::string formatPattern(const Pattern& p) {
    static const char* hx = "0123456789ABCDEF";
    std::string s;
    for (size_t i = 0; i < p.size(); ++i) {
        if (i) s += ' ';
        if (!p.mask[i]) {
            s += "??";
        } else {
            s += hx[p.bytes[i] >> 4];
            s += hx[p.bytes[i] & 15];
        }
    }
    return s;
}

std::vector<size_t> findAll(const uint8_t* data, size_t size, const Pattern& p, size_t maxHits) {
    std::vector<size_t> hits;
    if (p.empty() || size < p.size() || maxHits == 0) return hits;
    const uint8_t* anchor = p.bytes.data() + p.anchorOffset;
    const size_t alen = p.anchorLength;
    // The anchor can only sit in [anchorOffset, size - (patternSize - anchorOffset)].
    const size_t first = p.anchorOffset;
    const size_t last = size - (p.size() - p.anchorOffset);  // inclusive start of anchor
    size_t pos = first;
    while (pos <= last) {
        const void* f = std::memchr(data + pos, anchor[0], last - pos + 1);
        if (!f) break;
        size_t a = size_t(static_cast<const uint8_t*>(f) - data);
        if (std::memcmp(data + a, anchor, alen) == 0) {
            size_t start = a - p.anchorOffset;
            if (p.matchesAt(data + start)) {
                hits.push_back(start);
                if (hits.size() >= maxHits) break;
            }
        }
        pos = a + 1;
    }
    return hits;
}

size_t findFirst(const uint8_t* data, size_t size, const Pattern& p) {
    std::vector<size_t> h = findAll(data, size, p, 1);
    return h.empty() ? SIZE_MAX : h[0];
}

int32_t readI32(const uint8_t* p) {
    int32_t v;
    std::memcpy(&v, p, 4);
    return v;
}
uint32_t readU32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}
uint64_t readU64(const uint8_t* p) {
    uint64_t v;
    std::memcpy(&v, p, 8);
    return v;
}

bool parseRipSpec(const std::string& textIn, RipSpec& out) {
    out = RipSpec();
    std::string t;
    for (char c : textIn)
        if (c != ' ' && c != '\t') t += char(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    if (t.empty() || t == "none" || t == "0") return true;
    if (t == "call" || t == "jmp") {
        out.enabled = true;
        out.dispOffset = 1;
        out.insnLen = 5;
        return true;
    }
    if (t.compare(0, 4, "rip:") != 0) return false;
    size_t c = t.find(':', 4);
    if (c == std::string::npos) return false;
    char* end = nullptr;
    unsigned long d = std::strtoul(t.c_str() + 4, &end, 0);
    if (end != t.c_str() + c) return false;
    unsigned long l = std::strtoul(t.c_str() + c + 1, &end, 0);
    if (*end != 0 || l < d + 4 || l > 15) return false;
    out.enabled = true;
    out.dispOffset = d;
    out.insnLen = l;
    return true;
}

}  // namespace qs::probe
