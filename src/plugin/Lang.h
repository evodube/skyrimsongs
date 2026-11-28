// SkyrimSongs - play the lute and flute in Skyrim.
// Copyright (C) 2026 evodube. GPL-3.0 (LICENSE.txt).
#pragma once

#include "../music/Text.h"

// Translations: Data\Interface\Translations\SkyrimSongs_<LANGUAGE>.txt, the language being the game's (sLanguage) or
// [Display] Language in the INI. Skyrim's usual format: one "$SkyrimSongs_Key<tab>Text" per line, UTF-16 LE (as the
// game's own) or UTF-8. A line it lacks stays English. Text with {} or {0} has values put in there: keep them.
namespace Lang
{
	void Load();                           // on kDataLoaded, before the interface is made
	const std::string& Language();         // "ENGLISH", "RUSSIAN", ...
	const std::string& FontFile();         // $SkyrimSongs_Font: a font the translation wants for its letters ("" none)
	const std::u32string& AllText();       // every character the translation uses (the fonts are made with them)

	inline const char* T(const char* a_key, const char* a_english) { return music::Tr(a_key, a_english); }

	// with values put in ({} / {0} / {:.0f}...). A translation that breaks the format falls back to the English
	template <class... A>
	std::string TF(const char* a_key, const char* a_english, const A&... a_args)
	{
		try {
			return fmt::vformat(T(a_key, a_english), fmt::make_format_args(a_args...));
		} catch (...) {
		}
		try {
			return fmt::vformat(a_english, fmt::make_format_args(a_args...));
		} catch (...) {
			return a_english;
		}
	}

	// a widget's label, translated, with its key as its ImGui ID (so two labels translated alike don't collide)
	inline std::string TL(const char* a_key, const char* a_english) { return std::string(T(a_key, a_english)) + "##" + a_key; }
}
