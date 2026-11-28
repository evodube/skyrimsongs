// SkyrimSongs - play the lute and flute in Skyrim.
// Copyright (C) 2026 evodube. GPL-3.0 (LICENSE.txt).
#include "Lang.h"

#include "Config.h"

namespace Lang
{
	namespace
	{
		constexpr std::string_view kPrefix = "SkyrimSongs_";

		std::unordered_map<std::string, std::string> g_text;  // key (no "$SkyrimSongs_") -> text, never changed after Load
		std::string                                  g_lang = "ENGLISH";
		std::string                                  g_font;
		std::u32string                               g_all;

		const char* Lookup(const char* a_key, const char* a_english)
		{
			if (!a_key || g_text.empty()) {
				return a_english;
			}
			const auto it = g_text.find(a_key);
			if (it != g_text.end()) {
				return it->second.c_str();
			}
			// a line the translation lacks (an older translation, or new text): English, and a word in the log once
			static std::mutex            m;
			static std::set<std::string> told;
			std::lock_guard              lk(m);
			if (told.insert(a_key).second) {
				spdlog::info("Language: {} has no $SkyrimSongs_{} yet (English used)", g_lang, a_key);
			}
			return a_english;
		}

		std::string Upper(std::string a_s)
		{
			std::transform(a_s.begin(), a_s.end(), a_s.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
			return a_s;
		}

		std::string Trim(std::string_view a_s)
		{
			while (!a_s.empty() && (a_s.front() == ' ' || a_s.front() == '\t')) {
				a_s.remove_prefix(1);
			}
			while (!a_s.empty() && (a_s.back() == ' ' || a_s.back() == '\t' || a_s.back() == '\r')) {
				a_s.remove_suffix(1);
			}
			return std::string(a_s);
		}

		// the file's bytes as UTF-8: UTF-16 LE/BE with its byte order mark (the game's own format), else UTF-8
		std::string AsUtf8(const std::string& a_raw)
		{
			const auto* b = reinterpret_cast<const unsigned char*>(a_raw.data());
			const bool  le = a_raw.size() >= 2 && b[0] == 0xFF && b[1] == 0xFE;
			const bool  be = a_raw.size() >= 2 && b[0] == 0xFE && b[1] == 0xFF;
			if (le || be) {
				std::wstring w((a_raw.size() - 2) / 2, L'\0');
				for (std::size_t i = 0; i < w.size(); ++i) {
					const unsigned char lo = b[2 + i * 2 + (le ? 0 : 1)], hi = b[2 + i * 2 + (le ? 1 : 0)];
					w[i] = static_cast<wchar_t>(lo | (hi << 8));
				}
				const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
				std::string out(static_cast<std::size_t>((std::max)(n, 0)), '\0');
				if (n > 0) {
					::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
				}
				return out;
			}
			if (a_raw.size() >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF) {
				return a_raw.substr(3);
			}
			return a_raw;
		}

		std::string Unescape(const std::string& a_s)
		{
			std::string out;
			out.reserve(a_s.size());
			for (std::size_t i = 0; i < a_s.size(); ++i) {
				if (a_s[i] == '\\' && i + 1 < a_s.size()) {
					const char c = a_s[i + 1];
					if (c == 'n' || c == 't' || c == '\\') {
						out += c == 'n' ? '\n' : c == 't' ? '\t' : '\\';
						++i;
						continue;
					}
				}
				out += a_s[i];
			}
			return out;
		}

		void AddChars(const std::string& a_utf8)
		{
			const int n = ::MultiByteToWideChar(CP_UTF8, 0, a_utf8.data(), static_cast<int>(a_utf8.size()), nullptr, 0);
			if (n <= 0) {
				return;
			}
			std::wstring w(static_cast<std::size_t>(n), L'\0');
			::MultiByteToWideChar(CP_UTF8, 0, a_utf8.data(), static_cast<int>(a_utf8.size()), w.data(), n);
			for (std::size_t i = 0; i < w.size(); ++i) {
				char32_t c = w[i];
				if (c >= 0xD800 && c < 0xDC00 && i + 1 < w.size()) {  // a pair: beyond the fonts' 16-bit range, skipped
					++i;
					continue;
				}
				if (c >= 0x80 && g_all.find(c) == std::u32string::npos) {
					g_all += c;
				}
			}
		}

		// one file into g_text; how many lines it gave
		int Read(const std::filesystem::path& a_path)
		{
			std::ifstream f(a_path, std::ios::binary);
			if (!f) {
				return -1;
			}
			const std::string raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
			const std::string text = AsUtf8(raw);
			int               lines = 0;
			std::size_t       at = 0;
			while (at <= text.size()) {
				const std::size_t end = (std::min)(text.find('\n', at), text.size());
				std::string_view  line(text.data() + at, end - at);
				at = end + 1;
				while (!line.empty() && (line.back() == '\r')) {
					line.remove_suffix(1);
				}
				if (line.empty() || line.front() != '$') {
					continue;  // anything else is a comment
				}
				line.remove_prefix(1);
				// the key ends at the tab (Skyrim's format); a space will do too
				std::size_t cut = line.find('\t');
				const bool  tab = cut != std::string_view::npos;
				if (!tab) {
					cut = line.find(' ');
				}
				if (cut == std::string_view::npos) {
					continue;
				}
				std::string key = Trim(line.substr(0, cut));
				if (key.starts_with(kPrefix)) {
					key.erase(0, kPrefix.size());
				}
				// the text exactly as written after the tab (a space it starts with is meant)
				std::string_view raw = line.substr(cut + 1);
				while (!tab && !raw.empty() && raw.front() == ' ') {
					raw.remove_prefix(1);
				}
				std::string value = Unescape(std::string(raw));
				if (key.empty()) {
					continue;
				}
				AddChars(value);
				g_text[key] = std::move(value);
				++lines;
			}
			return lines;
		}
	}

	const std::string&    Language() { return g_lang; }
	const std::string&    FontFile() { return g_font; }
	const std::u32string& AllText() { return g_all; }

	void Load()
	{
		g_text.clear();
		g_all.clear();
		std::string lang = Config::Get().language;
		if (lang.empty()) {
			if (auto* s = RE::GetINISetting("sLanguage:General"); s && s->GetType() == RE::Setting::Type::kString && s->GetString() && *s->GetString()) {
				lang = s->GetString();
			}
		}
		g_lang = lang.empty() ? "ENGLISH" : Upper(Trim(lang));

		const std::filesystem::path dir = "Data\\Interface\\Translations";
		const auto                  file = dir / ("SkyrimSongs_" + g_lang + ".txt");
		int                         n = Read(file);
		if (n < 0 && g_lang != "ENGLISH") {
			spdlog::info("Language: {} - no {}, so English", g_lang, file.filename().string());
			n = Read(dir / "SkyrimSongs_ENGLISH.txt");  // an edited English, if there is one
		} else if (n >= 0) {
			spdlog::info("Language: {} ({} lines from {})", g_lang, n, file.filename().string());
		}
		if (const auto it = g_text.find("Font"); it != g_text.end()) {
			g_font = it->second;
			g_text.erase(it);
		} else {
			g_font.clear();
		}
		music::g_translator = &Lookup;
	}
}
