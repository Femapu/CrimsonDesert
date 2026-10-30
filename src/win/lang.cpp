// QuattroStagioni - picks the interface language to match the game (see lang.h).
#include "lang.h"

#include <windows.h>

#include <cstdio>
#include <string>

#include "i18n.h"
#include "ini.h"
#include "known_builds.h"
#include "log.h"
#include "winutil.h"

namespace qs {
namespace {

std::string readSmall(const std::wstring& path) {
    std::string out;
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return out;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0 && out.size() < (1u << 20)) out.append(buf, n);
    fclose(f);
    return out;
}

// Steam writes the language chosen for the game in steamapps\appmanifest_<appid>.acf:
//   "UserConfig" { "language" "italian" }
std::string steamGameLanguage(const std::wstring& gameRoot) {
    std::wstring steamapps = dirName(dirName(gameRoot));
    std::string acf = readSmall(joinPath(steamapps, L"appmanifest_" + widen(builds::kSteamAppId) + L".acf"));
    size_t p = acf.find("\"language\"");
    if (p == std::string::npos) return std::string();
    size_t a = acf.find('"', p + 10);
    size_t b = a == std::string::npos ? a : acf.find('"', a + 1);
    return (a == std::string::npos || b == std::string::npos) ? std::string() : acf.substr(a + 1, b - a - 1);
}

std::string windowsLanguage() {
    switch (PRIMARYLANGID(GetUserDefaultUILanguage())) {
        case LANG_ITALIAN: return "italian";
        case LANG_GERMAN: return "german";
        case LANG_FRENCH: return "french";
        case LANG_SPANISH: return "spanish";
        case LANG_PORTUGUESE: return "brazilian";
        case LANG_RUSSIAN: return "russian";
        case LANG_POLISH: return "polish";
        case LANG_JAPANESE: return "japanese";
        case LANG_KOREAN: return "koreana";
        case LANG_CHINESE: return "schinese";
        default: return "english";
    }
}

}  // namespace

void initLanguage(const std::wstring& scriptsDir) {
    Ini ini;
    ini.parse(readSmall(joinPath(scriptsDir, L"QuattroStagioni.ini")));
    std::string want = ini.get("General", "Language", "auto");
    std::string code, source;
    if (!want.empty() && want != "auto") {
        code = want;
        source = "ini";
    } else {
        std::wstring gameRoot = dirName(dirName(scriptsDir));  // bin64\scripts -> game root
        std::string steam = steamGameLanguage(gameRoot);
        source = steam.empty() ? "Windows" : "Steam";
        code = i18n::codeFromSteamLanguage(steam.empty() ? windowsLanguage() : steam);
    }
    if (code.empty() || code == "it") {
        QS_INFO("language: it (%s)", source.c_str());
        return;
    }
    const std::wstring dir = joinPath(joinPath(scriptsDir, L"QuattroStagioni"), L"lang");
    std::string text = readSmall(joinPath(dir, widen(code) + L".txt"));
    std::string used = code;
    if (text.empty() && code.find('-') != std::string::npos) {  // "pt-br" -> "pt"
        used = code.substr(0, code.find('-'));
        text = readSmall(joinPath(dir, widen(used) + L".txt"));
    }
    if (text.empty()) {  // no translation for this language: English is understood more widely than Italian
        used = "en";
        text = readSmall(joinPath(dir, L"en.txt"));
    }
    int n = text.empty() ? 0 : i18n::loadText(used, text);
    QS_INFO("language: game %s (%s), interface %s, %d strings", code.c_str(), source.c_str(),
            n ? used.c_str() : "it (no translation file)", n);
}

}  // namespace qs
