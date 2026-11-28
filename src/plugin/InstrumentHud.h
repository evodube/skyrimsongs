// SkyrimSongs - play the lute and flute in Skyrim.
// Copyright (C) 2026 evodube. GPL-3.0 (LICENSE.txt).
#pragma once

#ifndef IMGUI_DEFINE_MATH_OPERATORS
#	define IMGUI_DEFINE_MATH_OPERATORS
#endif
#include <imgui.h>

#include <vector>

// The key guide drawn as the instrument itself, with ImGui's draw list only (no textures):
//  - the lute: its neck in carved wood, frets of wire, three courses of strings (the keyboard's three rows) that
//    shimmer when plucked, the key letters on the frets and the rosette of the body at the end
//  - the flute: a silver body with its lip plate, seven tone holes (each with its three letters, one per
//    register), the hole being played glowing warm, a breath of air at the mouthpiece and the register lit
namespace InstrumentHud
{
	struct Sounding
	{
		int   pitch;
		float age;  // seconds since it was struck
	};

	struct Fonts
	{
		ImFont* key;    // the key letters
		ImFont* sm;     // note names
		ImFont* label;  // engraved labels
	};

	// show the controller's buttons instead of the keyboard's letters (the last notes came from a controller)
	inline bool g_pad = false;

	// a_base: the pitch of the bottom row's first key (Z); notes outside the 37 are shown an octave in
	void DrawLute(ImDrawList* a_dl, ImVec2 a_pos, ImVec2 a_size, float a_scale, int a_base, const std::vector<Sounding>& a_notes, double a_now,
		const Fonts& a_fonts);
	void DrawFlute(ImDrawList* a_dl, ImVec2 a_pos, ImVec2 a_size, float a_scale, int a_base, const std::vector<Sounding>& a_notes, double a_now,
		const Fonts& a_fonts);
}
