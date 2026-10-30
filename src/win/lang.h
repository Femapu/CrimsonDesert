// QuattroStagioni - interface language that follows the game.
//
// Order: [General] Language= in QuattroStagioni.ini (code like "en", or "auto"), else the language
// Steam uses for Crimson Desert (steamapps\appmanifest_3321460.acf), else the Windows UI language.
// Italian is built in; other languages load QuattroStagioni\lang\<code>.txt, falling back to English.
#pragma once
#include <string>

namespace qs {

void initLanguage(const std::wstring& scriptsDir);  // call once at startup, before the overlay starts

}  // namespace qs
