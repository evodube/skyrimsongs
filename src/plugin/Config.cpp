// SkyrimSongs - play the lute and flute in Skyrim.
// Copyright (C) 2026 evodube. GPL-3.0 (LICENSE.txt).
#include "Config.h"

namespace Config
{
	namespace
	{
		Values      g_values;
		const char* kPath = "Data\\SKSE\\Plugins\\SkyrimSongs.ini";

		std::string Str(const char* a_section, const char* a_key, const char* a_def)
		{
			char buf[256]{};
			::GetPrivateProfileStringA(a_section, a_key, a_def, buf, sizeof(buf), kPath);
			std::string s = buf;
			if (const auto c = s.find_first_of(";#"); c != std::string::npos) {
				s.resize(c);
			}
			while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
				s.pop_back();
			}
			return s.empty() ? std::string(a_def) : s;
		}
		float Float(const char* a_section, const char* a_key, float a_def)
		{
			const auto s = Str(a_section, a_key, "");
			if (s.empty()) {
				return a_def;
			}
			try {
				return std::stof(s);
			} catch (...) {
				return a_def;
			}
		}
		int Int(const char* a_section, const char* a_key, int a_def)
		{
			const auto s = Str(a_section, a_key, "");
			if (s.empty()) {
				return a_def;
			}
			try {
				return static_cast<int>(std::stol(s, nullptr, 0));  // 0x2B or 43
			} catch (...) {
				return a_def;
			}
		}
		bool Bool(const char* a_section, const char* a_key, bool a_def)
		{
			const auto s = Str(a_section, a_key, "");
			if (s.empty()) {
				return a_def;
			}
			return s == "1" || _stricmp(s.c_str(), "true") == 0 || _stricmp(s.c_str(), "on") == 0;
		}
	}

	const Values& Get() { return g_values; }
	Values&       Edit() { return g_values; }

	void Save()
	{
		const auto& v = g_values;
		auto put = [](const char* a_section, const char* a_key, const std::string& a_value) {
			::WritePrivateProfileStringA(a_section, a_key, a_value.c_str(), kPath);
		};
		auto f = [](float a_v) { return fmt::format("{:.2f}", a_v); };
		auto b = [](bool a_v) { return std::string(a_v ? "1" : "0"); };
		put("Play", "Hotkey", fmt::format("0x{:02X}", v.hotkey));
		put("Play", "RequireInstrument", b(v.requireInstrument));
		put("Play", "ThirdPerson", b(v.thirdPerson));
		put("Play", "MuteGameMusic", b(v.muteMusic));
		put("Play", "Volume", f(v.volume));
		put("Play", "Reverb", f(v.reverb));
		put("Play", "LatencyMs", std::to_string(v.latencyMs));
		put("Play", "Speed", f(v.speed));
		put("Play", "UiScale", f(v.uiScale));
		put("Play", "ShowKeyboard", b(v.showKeyboard));
		put("Play", "HideHud", b(v.hideHud));
		put("Display", "UpscalerFix", b(v.upscalerFix));
		put("Play", "SongSort", std::to_string(v.songSort));
		put("Crowd", "ListenRange", f(v.listenRange));
		put("Crowd", "TipRate", f(v.tipRate));
		put("Crowd", "TipSize", f(v.tipSize));
		put("Crowd", "MaxTipPerListener", std::to_string(v.maxTipPerListener));
		put("Crowd", "Patience", f(v.patience));
		put("Crowd", "ThrowOut", b(v.throwOut));
		put("Crowd", "SpeechXP", f(v.speechXp));
		put("Crowd", "Reactions", b(v.reactions));
		put("Crowd", "MaxVisitors", std::to_string(v.maxVisitors));
		spdlog::info("Settings saved");
	}

	void Load()
	{
		Values v;
		v.hotkey = Int("Play", "Hotkey", v.hotkey);
		v.requireInstrument = Bool("Play", "RequireInstrument", v.requireInstrument);
		v.thirdPerson = Bool("Play", "ThirdPerson", v.thirdPerson);
		v.muteMusic = Bool("Play", "MuteGameMusic", v.muteMusic);
		v.musicCategory = Str("Play", "MusicCategory", v.musicCategory.c_str());
		v.volume = std::clamp(Float("Play", "Volume", v.volume), 0.0f, 2.0f);
		v.reverb = std::clamp(Float("Play", "Reverb", v.reverb), 0.0f, 1.0f);
		v.latencyMs = std::clamp(Int("Play", "LatencyMs", v.latencyMs), -300, 300);
		v.speed = std::clamp(Float("Play", "Speed", v.speed), 0.5f, 2.0f);
		v.uiScale = std::clamp(Float("Play", "UiScale", v.uiScale), 0.5f, 2.0f);
		v.showKeyboard = Bool("Play", "ShowKeyboard", v.showKeyboard);
		v.hideHud = Bool("Play", "HideHud", v.hideHud);
		v.upscalerFix = Bool("Display", "UpscalerFix", v.upscalerFix);
		v.songSort = std::clamp(Int("Play", "SongSort", v.songSort), 0, 4);
		v.listenRange = std::clamp(Float("Crowd", "ListenRange", v.listenRange), 200.0f, 6000.0f);
		v.tipRate = std::clamp(Float("Crowd", "TipRate", v.tipRate), 0.0f, 10.0f);
		v.tipSize = std::clamp(Float("Crowd", "TipSize", v.tipSize), 0.0f, 10.0f);
		v.maxTipPerListener = std::clamp(Int("Crowd", "MaxTipPerListener", v.maxTipPerListener), 0, 10000);
		v.patience = std::clamp(Float("Crowd", "Patience", v.patience), 0.1f, 5.0f);
		v.throwOut = Bool("Crowd", "ThrowOut", v.throwOut);
		v.speechXp = std::clamp(Float("Crowd", "SpeechXP", v.speechXp), 0.0f, 10.0f);
		v.reactions = Bool("Crowd", "Reactions", v.reactions);
		v.maxVisitors = std::clamp(Int("Crowd", "MaxVisitors", v.maxVisitors), 0, 4);
		v.luteIdle = static_cast<RE::FormID>(Int("Animation", "LuteIdle", 0));
		v.fluteIdle = static_cast<RE::FormID>(Int("Animation", "FluteIdle", 0));
		v.stopIdle = static_cast<RE::FormID>(Int("Animation", "StopIdle", 0));
		v.logInput = Bool("Debug", "LogInput", v.logInput);
		g_values = v;
		spdlog::info("Settings: hotkey 0x{:02X}, instrument needed {}, third person {}, volume {:.2f}, range {:.0f}, throw-out {}", v.hotkey,
			v.requireInstrument, v.thirdPerson, v.volume, v.listenRange, v.throwOut);
	}
}
