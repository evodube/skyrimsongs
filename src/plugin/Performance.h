// SkyrimSongs - play the lute and flute in Skyrim.
// Copyright (C) 2026 evodube. GPL-3.0 (LICENSE.txt).
#pragma once

// A performance: take out your instrument anywhere (the hotkey), play it with the keyboard or a MIDI keyboard,
// and whoever's in earshot listens. Tips, heckles, and in an inn, the door if you're bad enough.
namespace Performance
{
	void OnDataLoaded();
	void OnGameLoaded();
	void FrameTick();  // main thread, once a frame

	// from the input hook (main thread)
	void OnHotkey(int a_scan);           // a key went down while you're not playing
	void OnPadHotkey();                  // the controller's hotkey, while you're not playing
	void OnPad(int a_code, bool a_down); // a controller button while you play (not in the songbook)
	void OnKey(int a_scan, bool a_down); // any key while you play

	// the overlay (Present)
	void SetupUi(float a_screenH);
	void DrawUi();

	// the SKSE co-save: bans and totals
	void Save(SKSE::SerializationInterface* a_intfc);
	void Load(SKSE::SerializationInterface* a_intfc);
	void Revert(SKSE::SerializationInterface* a_intfc);
}
