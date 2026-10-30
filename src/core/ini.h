// QuattroStagioni - tiny INI reader shared by the config file and the substitution manifest.
// Sections and keys are case-insensitive; ';' and '#' start comments; values are trimmed.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace qs {

class Ini {
public:
    void parse(const std::string& text);
    bool has(const std::string& section, const std::string& key) const;
    std::string get(const std::string& section, const std::string& key, const std::string& def = "") const;
    // Accepts decimal or 0x-prefixed hex. Returns `def` if missing or malformed.
    uint64_t getU64(const std::string& section, const std::string& key, uint64_t def = 0) const;
    long long getInt(const std::string& section, const std::string& key, long long def = 0) const;
    std::vector<std::string> sections() const;  // in file order

private:
    std::map<std::string, std::map<std::string, std::string>> data_;
    std::vector<std::string> order_;
};

bool parseU64(const std::string& s, uint64_t& out);
bool parseHex(const std::string& s, std::vector<uint8_t>& out);  // "0a1b..." -> bytes

}  // namespace qs
