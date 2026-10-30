// QuattroStagioni - user interface language.
//
// The source strings are Italian (the author's language). Other languages come from plain text
// files shipped with the mod (bin64\scripts\QuattroStagioni\lang\<code>.txt): blocks separated by a
// blank line, first line the Italian text, second line the translation; '#' starts a comment.
// Translators only need a text editor, and a missing line simply shows the Italian text.
// The table is set once at startup, before any UI thread runs, then only read.
#pragma once
#include <string>

namespace qs::i18n {

// Steam language name ("italian", "english", "german", "schinese", ...) -> code ("it", "en", "de", "zh-hans").
std::string codeFromSteamLanguage(const std::string& steamName);

// Parses a language file. Returns the number of entries; malformed blocks are skipped.
int loadText(const std::string& code, const std::string& text);
void reset();                      // back to Italian (no table)
const std::string& language();     // current code, "it" by default

// Translation of an Italian source string (format strings included). Leading/trailing spaces of the
// source are kept around the translation. Returns a pointer valid for the life of the process.
const char* tr(const char* it);

}  // namespace qs::i18n

