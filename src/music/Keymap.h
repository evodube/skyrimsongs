// SkyrimSongs - play the lute and flute in Skyrim.
// Copyright (C) 2026 evodube. GPL-3.0 (LICENSE.txt).
#pragma once

#include <string>

// The computer keyboard as an instrument: three rows of seven keys, each row an octave of the white notes
// (C D E F G A B), the bottom row lowest, and I for the C on top. Hold Shift for the note a semitone up (the
// black keys). 37 notes: three full octaves and the top C, like a 37-key keyboard.
//
//   Q W E R T Y U I  high octave   (base + 24), I = base + 36
//   A S D F G H J    middle octave (base + 12)
//   Z X C V B N M    low octave    (base)
//
// Keys are DirectInput scan codes, which is what the game reports.
namespace music::keymap
{
	inline constexpr int kRows[3][7] = {
		{ 0x2C, 0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x32 },  // Z X C V B N M
		{ 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24 },  // A S D F G H J
		{ 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16 },  // Q W E R T Y U
	};
	inline constexpr char kLetters[3][7] = {
		{ 'Z', 'X', 'C', 'V', 'B', 'N', 'M' },
		{ 'A', 'S', 'D', 'F', 'G', 'H', 'J' },
		{ 'Q', 'W', 'E', 'R', 'T', 'Y', 'U' },
	};
	inline constexpr int kWhite[7] = { 0, 2, 4, 5, 7, 9, 11 };
	inline constexpr int kTopC = 0x17;  // I: the top C
	inline constexpr int kNotes = 37;   // base .. base + 36
	inline constexpr int kLeftShift = 0x2A, kRightShift = 0x36;

	// The key's row and column, or false if it isn't a note key
	inline bool Find(int a_scan, int& a_row, int& a_col)
	{
		for (int r = 0; r < 3; ++r) {
			for (int c = 0; c < 7; ++c) {
				if (kRows[r][c] == a_scan) {
					a_row = r;
					a_col = c;
					return true;
				}
			}
		}
		return false;
	}

	// The pitch a key plays, or -1
	inline int Pitch(int a_scan, int a_base, bool a_shift)
	{
		if (a_scan == kTopC) {
			return a_base + 36;  // nothing above it
		}
		int r = 0, c = 0;
		if (!Find(a_scan, r, c)) {
			return -1;
		}
		return a_base + r * 12 + kWhite[c] + (a_shift ? 1 : 0);
	}

	// Which key plays a pitch: false if it's outside the 37 notes. Sharps are the white key below with Shift; the
	// top C is I (row 2, column 7).
	struct Label
	{
		int  row = 0, col = 0;  // col 7: the top C
		bool shift = false;
		char letter = '?';
	};
	inline bool LabelFor(int a_pitch, int a_base, Label& a_out)
	{
		const int rel = a_pitch - a_base;
		if (rel < 0 || rel > 36) {
			return false;
		}
		if (rel == 36) {
			a_out = { 2, 7, false, 'I' };
			return true;
		}
		int row = rel / 12, pc = rel % 12;
		int  col = -1;
		bool shift = false;
		for (int c = 6; c >= 0; --c) {
			if (kWhite[c] == pc) {
				col = c;
				break;
			}
			if (kWhite[c] == pc - 1) {
				col = c;
				shift = true;
				break;
			}
		}
		if (col < 0) {
			return false;
		}
		a_out = { row, col, shift, kLetters[row][col] };
		return true;
	}
}

// A gamepad as an instrument: the four D-pad directions and the four face buttons are an octave of white notes and
// the C above it, going round the D-pad then round the buttons (down, left, up, right, A, X, Y, B: C D E F G A B C).
// Hold LT for the octave below, RT for the octave above, LB for the sharp. The same 37 notes as the keyboard:
// LT is the keyboard's bottom row, nothing held its middle row, RT its top row, and RT+B the top C.
namespace music::padmap
{
	// XInput's button masks (as the game reports them); the triggers come as their own codes
	inline constexpr int kButtons[8] = { 0x0002, 0x0004, 0x0001, 0x0008, 0x1000, 0x4000, 0x8000, 0x2000 };
	inline constexpr const char* kNames[8] = { "\xE2\x86\x93", "\xE2\x86\x90", "\xE2\x86\x91", "\xE2\x86\x92", "A", "X", "Y", "B" };  // arrows in UTF-8
	inline constexpr int kLeftTrigger = 0x0009, kRightTrigger = 0x000A, kLeftShoulder = 0x0100;

	// which of the eight note buttons, or -1
	inline int Index(int a_code)
	{
		for (int i = 0; i < 8; ++i) {
			if (kButtons[i] == a_code) {
				return i;
			}
		}
		return -1;
	}

	// the pitch a note button plays with the triggers and LB as held (both triggers: LT's), or -1 past the top C
	inline int Pitch(int a_index, int a_base, bool a_lower, bool a_higher, bool a_sharp)
	{
		if (a_index < 0 || a_index > 7) {
			return -1;
		}
		const int row = a_lower ? 0 : a_higher ? 2 : 1;
		const int rel = row * 12 + (a_index < 7 ? keymap::kWhite[a_index] : 12) + (a_sharp ? 1 : 0);
		return rel > 36 ? -1 : a_base + rel;
	}

	// what to press for a keyboard key (row 0..2, col 0..6, col 7 the top C): the trigger ("LT", "" or "RT") and
	// the button
	inline const char* Trigger(int a_row, int a_col) { return a_col == 7 || a_row == 2 ? "RT" : a_row == 0 ? "LT" : ""; }
	inline const char* Button(int a_col) { return a_col == 7 ? kNames[7] : kNames[a_col < 0 ? 0 : a_col > 6 ? 6 : a_col]; }

	// all of it: "LB+RT+X"
	inline std::string Text(int a_row, int a_col, bool a_sharp)
	{
		std::string out = a_sharp ? "LB+" : "";
		if (*Trigger(a_row, a_col)) {
			out += Trigger(a_row, a_col);
			out += '+';
		}
		return out + Button(a_col);
	}
}
