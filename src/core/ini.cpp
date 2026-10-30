// QuattroStagioni - tiny INI reader.
#include "ini.h"

#include <algorithm>

namespace qs {
namespace {

std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}

int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

bool parseU64(const std::string& sIn, uint64_t& out) {
    std::string s = trim(sIn);
    if (s.empty()) return false;
    uint64_t v = 0;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        for (size_t i = 2; i < s.size(); ++i) {
            int h = hexVal(s[i]);
            if (h < 0 || v > (UINT64_MAX >> 4)) return false;
            v = (v << 4) | uint64_t(h);
        }
    } else {
        for (char c : s) {
            if (c < '0' || c > '9') return false;
            uint64_t nv = v * 10 + uint64_t(c - '0');
            if (nv / 10 != v) return false;
            v = nv;
        }
    }
    out = v;
    return true;
}

bool parseHex(const std::string& sIn, std::vector<uint8_t>& out) {
    std::string s = trim(sIn);
    if (s.size() % 2) return false;
    out.clear();
    for (size_t i = 0; i < s.size(); i += 2) {
        int a = hexVal(s[i]), b = hexVal(s[i + 1]);
        if (a < 0 || b < 0) return false;
        out.push_back(uint8_t(a * 16 + b));
    }
    return true;
}

void Ini::parse(const std::string& text) {
    data_.clear();
    order_.clear();
    std::string section;
    size_t p = 0;
    while (p <= text.size()) {
        size_t e = text.find('\n', p);
        if (e == std::string::npos) e = text.size();
        std::string line = trim(text.substr(p, e - p));
        p = e + 1;
        if (line.size() >= 3 && uint8_t(line[0]) == 0xEF && uint8_t(line[1]) == 0xBB && uint8_t(line[2]) == 0xBF)
            line = trim(line.substr(3));  // UTF-8 BOM
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[') {
            size_t c = line.find(']');
            section = lower(trim(line.substr(1, c == std::string::npos ? std::string::npos : c - 1)));
            if (std::find(order_.begin(), order_.end(), section) == order_.end()) order_.push_back(section);
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = lower(trim(line.substr(0, eq)));
        std::string val = line.substr(eq + 1);
        size_t cpos = val.find(" ;");  // inline comment (" ; ...")
        if (cpos != std::string::npos) val = val.substr(0, cpos);
        data_[section][key] = trim(val);
    }
}

bool Ini::has(const std::string& section, const std::string& key) const {
    auto s = data_.find(lower(section));
    return s != data_.end() && s->second.count(lower(key));
}

std::string Ini::get(const std::string& section, const std::string& key, const std::string& def) const {
    auto s = data_.find(lower(section));
    if (s == data_.end()) return def;
    auto k = s->second.find(lower(key));
    return k == s->second.end() ? def : k->second;
}

uint64_t Ini::getU64(const std::string& section, const std::string& key, uint64_t def) const {
    uint64_t v;
    return parseU64(get(section, key), v) ? v : def;
}

long long Ini::getInt(const std::string& section, const std::string& key, long long def) const {
    std::string s = get(section, key);
    if (!s.empty() && s[0] == '-') {
        uint64_t v;
        return parseU64(s.substr(1), v) ? -(long long)v : def;
    }
    uint64_t v;
    return parseU64(s, v) ? (long long)v : def;
}

std::vector<std::string> Ini::sections() const { return order_; }

}  // namespace qs
