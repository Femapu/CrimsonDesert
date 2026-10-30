// QuattroStagioni - user interface language (see i18n.h).
#include "i18n.h"

#include <cctype>
#include <memory>
#include <mutex>
#include <sstream>
#include <unordered_map>

namespace qs::i18n {
namespace {

std::string g_lang = "it";
std::unordered_map<std::string, std::string> g_table;  // trimmed Italian -> translation
// Results with the original padding, keyed by the full source text. Values are never erased, so the
// returned pointers stay valid; the mutex only guards insertion (tr is called from UI threads).
std::unordered_map<std::string, std::unique_ptr<std::string>> g_cache;
std::mutex g_mutex;

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

}  // namespace

std::string codeFromSteamLanguage(const std::string& steamName) {
    std::string n;
    for (char c : steamName) n += char(std::tolower(static_cast<unsigned char>(c)));
    static const struct { const char* steam; const char* code; } map[] = {
        {"italian", "it"}, {"english", "en"}, {"german", "de"}, {"french", "fr"}, {"spanish", "es"},
        {"latam", "es-419"}, {"brazilian", "pt-br"}, {"portuguese", "pt"}, {"russian", "ru"}, {"polish", "pl"},
        {"turkish", "tr"}, {"japanese", "ja"}, {"koreana", "ko"}, {"schinese", "zh-hans"}, {"tchinese", "zh-hant"},
        {"thai", "th"}, {"ukrainian", "uk"}, {"czech", "cs"}, {"dutch", "nl"}, {"arabic", "ar"},
    };
    for (const auto& m : map)
        if (n == m.steam) return m.code;
    return n.empty() ? std::string() : "en";
}

int loadText(const std::string& code, const std::string& text) {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_table.clear();
    g_cache.clear();
    g_lang = code;
    std::istringstream in(text);
    std::string line, key;
    int n = 0;
    bool haveKey = false;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() >= 3 && (unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB &&
            (unsigned char)line[2] == 0xBF)
            line.erase(0, 3);  // UTF-8 BOM
        std::string t = trim(line);
        if (t.empty()) { haveKey = false; continue; }
        if (t[0] == '#') continue;
        if (!haveKey) { key = t; haveKey = true; continue; }
        g_table[key] = t;
        ++n;
        haveKey = false;
    }
    return n;
}

void reset() { loadText("it", std::string()); }

const std::string& language() { return g_lang; }

const char* tr(const char* it) {
    if (!it) return "";
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_table.empty()) return it;
    auto c = g_cache.find(it);
    if (c != g_cache.end()) return c->second->c_str();
    std::string src(it), key = trim(src);
    std::string out = src;
    auto f = g_table.find(key);
    if (f != g_table.end() && !key.empty()) {
        size_t a = src.find(key);
        out = src.substr(0, a) + f->second + src.substr(a + key.size());
    }
    auto& slot = g_cache[src];
    slot.reset(new std::string(out));
    return slot->c_str();
}

}  // namespace qs::i18n
