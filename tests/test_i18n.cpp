// QuattroStagioni - tests for the interface language table (src/core/i18n.*).
#include <cstdio>
#include <cstring>
#include <string>

#include "../src/core/i18n.h"

static int g_pass = 0, g_fail = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

int main(int, char**) {
    using qs::i18n::tr;
    CHECK(std::strcmp(tr("Autunno"), "Autunno") == 0);  // no table: Italian
    int n = qs::i18n::loadText("en", "\xEF\xBB\xBF# comment\r\nAutunno\r\nAutumn\r\n\r\n%s, giorno %d di %d\n%s, day %d of %d\n\n"
                                     "Salvataggio non riuscito:\nSaving failed:\n\nsolo chiave\n");
    CHECK(n == 3);
    CHECK(qs::i18n::language() == "en");
    CHECK(std::strcmp(tr("Autunno"), "Autumn") == 0);
    CHECK(std::strcmp(tr("  Autunno  "), "  Autumn  ") == 0);  // padding kept (tab labels)
    CHECK(std::strcmp(tr("Salvataggio non riuscito: "), "Saving failed: ") == 0);
    CHECK(std::strcmp(tr("%s, giorno %d di %d"), "%s, day %d of %d") == 0);
    CHECK(std::strcmp(tr("Inverno"), "Inverno") == 0);  // missing -> Italian
    CHECK(tr("Autunno") == tr("Autunno"));               // stable pointer
    CHECK(qs::i18n::codeFromSteamLanguage("Italian") == "it");
    CHECK(qs::i18n::codeFromSteamLanguage("schinese") == "zh-hans");
    CHECK(qs::i18n::codeFromSteamLanguage("klingon") == "en");
    qs::i18n::reset();
    CHECK(std::strcmp(tr("Autunno"), "Autunno") == 0);
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
