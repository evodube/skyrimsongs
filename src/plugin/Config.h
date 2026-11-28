// SkyrimSongs - play the lute and flute in Skyrim.
// Copyright (C) 2026 evodube. GPL-3.0 (LICENSE.txt).
#pragma once

// Data\SKSE\Plugins\SkyrimSongs.ini
namespace Config
{
	struct Values
	{
		// Playing
		int   hotkey = 0x2B;              // \: take out your instrument / put it away, close the songbook (DirectInput scan code)
		std::string padHotkey = "LB+Down";  // on a controller: hold the first, press the second (the second never reaches the game). "" off
		bool  requireInstrument = true;   // you need a lute or flute in your pack
		bool  thirdPerson = true;         // switch to third person while you play (and back after)
		bool  muteMusic = true;           // quiet the game's music while you play
		std::string musicCategory = "Music";  // the sound category the game's music plays in (its name in the audio settings)
		float volume = 0.9f;
		float reverb = 0.16f;
		int   latencyMs = 0;              // play along: added to your timing if notes register late on your setup
		float speed = 1.0f;               // songs: how fast they play (0.5 - 2.0; 1 = as written)
		float uiScale = 1.0f;
		bool  showKeyboard = true;        // the key guide at the bottom of the screen
		int   songSort = 0;               // the songbook: 0 name, 1 length, 2 notes, 3 most played, 4 newest
		std::string language;             // "" the game's (sLanguage); or e.g. RUSSIAN: SkyrimSongs_RUSSIAN.txt in Interface\\Translations
		bool  upscalerFix = true;         // draw the interface to match an upscaler's render scale (dynamic resolution)
		bool  hideHud = false;            // play with nothing on screen (Backspace while playing toggles it)
		float bookX = 0.5f;               // where the songbook sits: its middle, as a share of the screen (drag it to move it)
		float bookY = 0.45f;

		// The crowd
		float listenRange = 700.0f;   // how far you can be heard (game units; 70 is about a metre, so about 10 m)
		float tipRate = 1.0f;
		float tipSize = 1.0f;
		int   maxTipPerListener = 40;  // gold one patron gives in one performance at most
		float patience = 1.0f;         // how quickly a bad set turns them
		bool  throwOut = true;         // a hostile crowd in an inn gets you thrown out (and banned there for the day)
		float speechXp = 0.5f;         // Speech experience per septim earned
		bool  reactions = true;        // listeners clap, dance, drink, hand over their tip, scratch their heads
		int   maxVisitors = 4;         // happy listeners who come over to stand in front of you (0-4)

		// Animation: the idles' form IDs in Skyrim.esm (0: the game's bard idles, which put the instrument in your hands)
		RE::FormID luteIdle = 0;
		RE::FormID fluteIdle = 0;
		RE::FormID stopIdle = 0;

		// Diagnostics
		bool logInput = false;
	};

	const Values& Get();
	Values&       Edit();  // the in-game settings page changes these live
	void          Load();
	void          Save();
}
