// SkyrimSongs - play the lute and flute in Skyrim.
// Copyright (C) 2026 evodube. GPL-3.0 (LICENSE.txt).
#pragma once

// Text a player sees, by key, with its English. The plugin sets the translator (its translation files); without one
// (the tests) it's the English. Every Tr("Key", "English") in the code is a line of
// Data\Interface\Translations\SkyrimSongs_ENGLISH.txt ($SkyrimSongs_Key<tab>English), made by tools/extract_strings.py.
namespace music
{
	using Translator = const char* (*)(const char* a_key, const char* a_english);
	inline Translator g_translator = nullptr;

	inline const char* Tr(const char* a_key, const char* a_english) { return g_translator ? g_translator(a_key, a_english) : a_english; }
}
