// SkyrimSongs - play the lute and flute in Skyrim.
// Copyright (C) 2026 evodube. GPL-3.0 (LICENSE.txt).
//
// The settings page in SKSE Menu Framework (if it's installed): the INI's settings, changed live, saved to the INI.
#include "Menu.h"
#include "Audio.h"
#include "Config.h"
#include "Lang.h"

#include "../music/Keymap.h"

#define _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING
#include "SKSEMenuFramework.h"

namespace Menu
{
	namespace
	{
		namespace UI = ImGuiMCP;
		using Lang::T;
		using Lang::TF;
		using Lang::TL;

		// a slider's number format with a translated word after it (a % in the word kept literal)
		std::string Units(const char* a_num, const char* a_word)
		{
			std::string w = a_word, out = a_num;
			out += ' ';
			for (char c : w) {
				out += c;
				if (c == '%') {
					out += '%';
				}
			}
			return out;
		}

		bool g_dirty = false;

		// ---- the hotkey: "Press a key", read straight from Windows (so no other input hook can hide it)
		bool             g_capturing = false;
		std::array<bool, 256> g_wasDown{};

		// a DirectInput scan code (what the game and the INI use) for a Windows virtual key; 0 if none
		int ScanFor(int a_vk)
		{
			const UINT sc = ::MapVirtualKeyA(static_cast<UINT>(a_vk), 4 /* MAPVK_VK_TO_VSC_EX */);
			if (!sc) {
				return 0;
			}
			return (sc & 0xFF) | (((sc >> 8) == 0xE0 || (sc >> 8) == 0xE1) ? 0x80 : 0);
		}

		std::string KeyName(int a_scan)
		{
			char       buf[64]{};
			const LONG lp = ((a_scan & 0x7F) << 16) | ((a_scan & 0x80) ? (1 << 24) : 0);
			if (::GetKeyNameTextA(lp, buf, sizeof(buf)) > 0) {
				return buf;
			}
			return fmt::format("0x{:02X}", a_scan);
		}

		// the keys you play with can't also be the hotkey
		bool UsedWhilePlaying(int a_scan)
		{
			static constexpr int kTaken[] = { 0x01, 0x0E, 0x0F, 0x19, 0x2A, 0x36, 0xC8, 0xD0, 0x17 };
			if (std::find(std::begin(kTaken), std::end(kTaken), a_scan) != std::end(kTaken)) {
				return true;
			}
			for (int r = 0; r < 3; ++r) {
				for (int c = 0; c < 7; ++c) {
					if (music::keymap::kRows[r][c] == a_scan) {
						return true;
					}
				}
			}
			return false;
		}

		std::string g_captureNote;

		void HotkeyRow()
		{
			auto& v = Config::Edit();
			UI::TextUnformatted(TF("MenuHotkey", "Hotkey: {}", KeyName(v.hotkey)).c_str());
			UI::SameLine();
			if (!g_capturing) {
				if (UI::Button(TL("MenuChangeKey", "Change...").c_str())) {
					g_capturing = true;
					g_captureNote.clear();
					for (int vk = 0; vk < 256; ++vk) {
						g_wasDown[vk] = (::GetAsyncKeyState(vk) & 0x8000) != 0;  // what's already held doesn't count
					}
				}
			} else {
				UI::TextColored(UI::ImVec4{ 0.95f, 0.8f, 0.4f, 1.0f }, "%s", T("MenuPressKey", "Press a key... (Esc cancels)"));
				for (int vk = 8; vk < 256; ++vk) {
					if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU) {
						continue;  // the left/right versions are reported too
					}
					const bool down = (::GetAsyncKeyState(vk) & 0x8000) != 0;
					const bool pressed = down && !g_wasDown[vk];
					g_wasDown[vk] = down;
					if (!pressed) {
						continue;
					}
					if (vk == VK_ESCAPE) {
						g_capturing = false;
						break;
					}
					const int scan = ScanFor(vk);
					if (!scan) {
						continue;
					}
					if (UsedWhilePlaying(scan)) {
						g_captureNote = TF("MenuKeyTaken", "{} is used while you play - pick another key.", KeyName(scan));
						continue;
					}
					v.hotkey = scan;
					g_capturing = false;
					g_captureNote.clear();
					Config::Save();  // straight to the INI
					spdlog::info("Hotkey changed in game to {} (0x{:02X})", KeyName(scan), scan);
					break;
				}
			}
			if (!g_captureNote.empty()) {
				UI::TextDisabled("%s", g_captureNote.c_str());
			}
		}

		void Hint(const char* a_text)
		{
			UI::TextDisabled("%s", a_text);
			UI::Spacing();
		}
		void Changed(bool a_changed) { g_dirty |= a_changed; }

		void SaveRow()
		{
			UI::Spacing();
			UI::Separator();
			if (UI::Button(TL("MenuSave", "Save to SkyrimSongs.ini").c_str())) {
				Config::Save();
				g_dirty = false;
			}
			UI::SameLine();
			if (UI::Button(TL("MenuUndo", "Undo changes").c_str())) {
				Config::Load();
				g_dirty = false;
			}
			if (g_dirty) {
				UI::SameLine();
				UI::TextDisabled("%s", T("MenuNotSaved", "(not saved yet: changes last until you quit)"));
			}
		}

		void __stdcall Playing()
		{
			auto& v = Config::Edit();
			UI::SeparatorText(T("MenuHeadInstrument", "Taking out your instrument"));
			HotkeyRow();
			Changed(UI::Checkbox(TL("MenuNeedInstrument", "Need a lute or flute in your pack").c_str(), &v.requireInstrument));
			Changed(UI::Checkbox(TL("MenuThirdPerson", "Switch to third person while playing").c_str(), &v.thirdPerson));
			Changed(UI::Checkbox(TL("MenuMuteMusic", "Quiet the game's music while playing").c_str(), &v.muteMusic));
			UI::SeparatorText(T("MenuHeadSound", "Sound"));
			if (UI::SliderFloat(TL("MenuVolume", "Volume").c_str(), &v.volume, 0.0f, 2.0f, "%.2f")) {
				Audio::SetVolume(v.volume);
				g_dirty = true;
			}
			if (UI::SliderFloat(TL("MenuEcho", "Room echo").c_str(), &v.reverb, 0.0f, 0.6f, "%.2f")) {
				Audio::SetReverb(v.reverb);
				g_dirty = true;
			}
			UI::SeparatorText(T("MenuHeadSongs", "Songs"));
			float speed = v.speed * 100.0f;
			if (UI::SliderFloat(TL("MenuSpeed", "Song speed").c_str(), &speed, 50.0f, 200.0f, "%.0f%%")) {
				v.speed = std::round(speed / 5.0f) * 5.0f / 100.0f;
				Audio::SetSongSpeed(v.speed);
				g_dirty = true;
			}
			Hint(T("MenuSpeedHint", "How fast songs play: 100% is as written."));
			UI::SeparatorText(T("MenuHeadScreen", "Screen"));
			Changed(UI::Checkbox(TL("MenuKeyGuide", "Show the key guide").c_str(), &v.showKeyboard));
			Changed(UI::Checkbox(TL("MenuHideHud", "Hide the interface while playing").c_str(), &v.hideHud));
			Hint(T("MenuHideHudHint", "Nothing on screen while you play, for immersion. Backspace while playing toggles it."));
			Changed(UI::SliderFloat(TL("MenuUiSize", "Interface size").c_str(), &v.uiScale, 0.6f, 1.6f, "%.2f"));
			SaveRow();
		}

		void __stdcall Crowd()
		{
			auto& v = Config::Edit();
			Changed(UI::SliderFloat(TL("MenuEarshot", "Earshot").c_str(), &v.listenRange, 300.0f, 4000.0f, Units("%.0f", T("MenuUnits", "units")).c_str()));
			Hint(T("MenuEarshotHint", "How far away people can hear you (70 is about a metre)."));
			Changed(UI::SliderFloat(TL("MenuTipRate", "How often they tip").c_str(), &v.tipRate, 0.0f, 4.0f, "%.2f"));
			Changed(UI::SliderFloat(TL("MenuTipSize", "How much they tip").c_str(), &v.tipSize, 0.0f, 4.0f, "%.2f"));
			Changed(UI::SliderInt(TL("MenuMaxTip", "Most one patron gives").c_str(), &v.maxTipPerListener, 0, 500, Units("%d", T("MenuSeptims", "septims")).c_str()));
			Hint(T("MenuMaxTipHint", "Each person tips you once a day."));
			Changed(UI::SliderFloat(TL("MenuPatience", "Patience").c_str(), &v.patience, 0.2f, 3.0f, "%.2f"));
			Hint(T("MenuPatienceHint", "How quickly the room turns on a bad set (higher = quicker)."));
			Changed(UI::Checkbox(TL("MenuThrowOut", "Innkeepers throw out bad bards").c_str(), &v.throwOut));
			Hint(T("MenuThrowOutHint", "And won't let you play there again that day."));
			Changed(UI::SliderFloat(TL("MenuSpeechXp", "Speech experience").c_str(), &v.speechXp, 0.0f, 3.0f, Units("%.2f", T("MenuPerSeptim", "per septim")).c_str()));
			Changed(UI::Checkbox(TL("MenuReactions", "Listeners clap, dance and react").c_str(), &v.reactions));
			Changed(UI::SliderInt(TL("MenuVisitors", "Listeners who come over").c_str(), &v.maxVisitors, 0, 4));
			SaveRow();
		}
	}

	void Register()
	{
		if (!SKSEMenuFramework::IsInstalled()) {
			spdlog::info("SKSE Menu Framework not installed: settings are in SkyrimSongs.ini");
			return;
		}
		SKSEMenuFramework::SetSection(T("MenuSection", "Skyrim Songs"));
		SKSEMenuFramework::AddSectionItem(T("MenuPagePlaying", "Playing"), Playing);
		SKSEMenuFramework::AddSectionItem(T("MenuPageCrowd", "The crowd"), Crowd);
		spdlog::info("Settings page added to SKSE Menu Framework");
	}
}
