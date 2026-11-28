// SkyrimSongs - play the lute and flute in Skyrim.
// Copyright (C) 2026 evodube. GPL-3.0 (LICENSE.txt).
#pragma once

struct IDXGISwapChain;

// The ImGui layer drawn over the game (a Present hook), and the keyboard while you play. The same plumbing as
// SkyrimYacht and Hearthsworn: a hook on the call that hands the game its input each frame.
//  - Closed: everything goes to the game; we only watch for the hotkey.
//  - Playing: the keyboard is the instrument (none of it reaches the game, so the keys don't walk you about or
//    open menus); the mouse and sticks still go to the game, so you can look around while you play.
//  - Book: the songbook is open with a cursor; nothing reaches the game.
namespace Overlay
{
	enum class Mode
	{
		Closed,
		Playing,
		Book
	};

	void SetMode(Mode a_mode);
	Mode GetMode();

	// the controller's buttons held now (XInput masks; LT 0x10000, RT 0x20000)
	std::uint32_t PadHeld();
	inline constexpr std::uint32_t kPadLT = 0x10000, kPadRT = 0x20000;
	// "LB+Down" -> its buttons (0 if it names none, or something it doesn't know)
	std::uint32_t ParsePadButtons(const std::string& a_text);

	bool InstallInputSink();
	void OnPresent(IDXGISwapChain* a_swapChain);
	int  SehFilter(EXCEPTION_POINTERS* a_info, const char* a_where);
	void ScreenSize(float& a_w, float& a_h);
}
