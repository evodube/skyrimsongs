// SkyrimSongs - play the lute and flute in Skyrim.
// Copyright (C) 2026 evodube. GPL-3.0 (LICENSE.txt).
#include "Performance.h"

#include "Audio.h"
#include "InstrumentHud.h"
#include "Config.h"
#include "MidiIn.h"
#include "Overlay.h"

#include "../music/Crowd.h"
#include "../music/Judge.h"
#include "../music/Keymap.h"
#include "../music/Songs.h"

#ifndef IMGUI_DEFINE_MATH_OPERATORS
#	define IMGUI_DEFINE_MATH_OPERATORS
#endif
#include <imgui.h>

#undef GetObject  // Windows.h renames it to GetObjectA, and BSScript::Variable has a GetObject
#undef small      // and rpcndr.h makes "small" a type

namespace Performance
{
	namespace
	{
		using music::Instrument;

		// ------------------------------------------------------------------------------------------ game forms
		constexpr RE::FormID kGold = 0x0000000F;
		constexpr RE::FormID kInnkeeperFaction = 0x0005091B;  // JobInnkeeperFaction
		constexpr RE::FormID kLocTypeInn = 0x0001CB87;
		constexpr RE::FormID kActorTypeNPC = 0x00013794;
		// the vanilla instruments (and the quest ones, found by sharing their models)
		constexpr RE::FormID kLutes[] = { 0x000DABAB };
		constexpr RE::FormID kFlutes[] = { 0x000DABA7, 0x00105177, 0x00105109 };
		// the game's bard idles (Skyrim.esm): they put the instrument in the hands. Idles' editor IDs aren't kept
		// in memory, so these are looked up by form ID (the same ones Skyrim's Got Talent plays on the player).
		constexpr RE::FormID kIdleLute = 0x00096F8D;
		constexpr RE::FormID kIdleFlute = 0x00096F8C;
		constexpr RE::FormID kIdleStopLoose = 0x0010D9EE;
		// the audience's idles (Skyrim.esm), the ones the game's own bard audiences use (and Skyrim's Got Talent):
		// applause, dancing, drinking and cheering, and handing something over (a tip)
		constexpr RE::FormID kIdleApplaud[] = { 0x000D8730, 0x000D8731, 0x000D8732, 0x000D8733 };
		constexpr RE::FormID kIdleDance[] = { 0x000F7C8A, 0x000F7C8B };
		constexpr RE::FormID kIdleEnjoy[] = { 0x00103653, 0x00097273 };
		constexpr RE::FormID kIdleGive = 0x000B5E20;
		// and the unimpressed (animation events, as Skyrim's Got Talent sends them)
		constexpr const char* kBooEvents[] = { "idlesurrender", "idlescratchhead", "idlepointclose", "idledialoguedefensivehandgesture" };
		constexpr std::uint32_t kSaveVersion = 1;

		const char* kSongDir = "Data\\SKSE\\Plugins\\SkyrimSongs\\Songs";

		RE::TESBoundObject*            g_gold = nullptr;
		RE::TESFaction*                g_innkeepers = nullptr;
		std::set<RE::FormID>           g_instrumentIds[2];
		std::set<std::string>          g_instrumentModels[2];
		RE::TESIdleForm*               g_idles[2]{};
		RE::TESIdleForm*               g_stopIdle = nullptr;
		std::vector<RE::BGSSoundCategory*> g_musicCats;
		std::set<RE::FormID>           g_lookers;            // seated listeners turned to watch you (head and eyes; a chair won't let them turn)
		double                         g_nextBardHush = 0.0;  // when to ask the inn's bard to stop again
		RE::BGSMusicType*              g_silence = nullptr;  // a music type of nothing but silence, pushed while you play
		RE::BGSMusicType*              g_tavernSilence = nullptr;  // Skyrim's own "hush the inn" (MUSTavernSILENCE, what the bards' script uses)
		std::vector<RE::TESIdleForm*>  g_applaud, g_dance, g_enjoy;
		RE::TESIdleForm*               g_give = nullptr;
		bool                           g_silenced = false;

		// ------------------------------------------------------------------------------------------ state
		enum class Act
		{
			Free,   // play what you like
			Along,  // play a song from the songbook, judged note by note
			Auto    // the song plays itself (the cheat)
		};

		struct Feed
		{
			std::string text;
			ImU32       color;
			double      t;
		};

		std::recursive_mutex g_mtx;
		bool                 g_playing = false;
		std::atomic<bool>    g_wantStart{ false };
		Act                  g_act = Act::Free;
		Instrument           g_inst = Instrument::Lute;
		Instrument           g_lastInst = Instrument::Lute;
		int                  g_octave = 0;
		bool                 g_shiftL = false, g_shiftR = false;
		std::map<int, int>   g_keyPitch;  // a key that's down -> the pitch it started
		std::atomic<int>     g_instA{ 0 };  // g_inst, for the MIDI keyboard's thread
		std::atomic<bool>    g_midiLive{ false };
		bool                 g_owns[2]{};
		// notes from the MIDI keyboard: sounded at once on its thread, judged on the next frame
		struct MidiHit
		{
			int    pitch;
			double now;
			double songT;
		};
		std::mutex           g_midiMtx;
		std::vector<MidiHit> g_midiQ;
		double                          g_hudToggled = -10.0;  // when Backspace last hid or showed the interface
		std::unordered_map<int, double> g_lit;  // pitch -> when it last sounded

		music::FreeJudge  g_free;
		music::ChartJudge g_chart;
		music::Crowd      g_crowd;
		music::Song       g_song;  // the song in play (Along / Auto)
		int               g_songShift = 0;
		double            g_lastClick = -99.0;
		double            g_quality = 0.5;
		music::FreeJudge::Breakdown g_breakdown;

		std::deque<Feed>                          g_feed;
		std::unordered_map<std::uint32_t, std::string> g_names;
		std::uint32_t                             g_keeper = 0;
		bool                                      g_inInn = false;
		RE::TESObjectCELL*                        g_cell = nullptr;
		int                                       g_earned = 0;
		double                                    g_badFor = 0.0;
		bool                                      g_warned = false;
		bool                                      g_forcedThird = false;
		std::vector<std::pair<RE::BGSSoundCategory*, float>> g_muted;
		int                                       g_midiCount = 0;
		std::vector<std::string>                  g_midiNames;

		// the songbook
		struct Entry
		{
			music::Song           song;
			std::string           error;
			std::filesystem::path path;
			std::filesystem::file_time_type mtime{};
		};
		std::vector<Entry> g_files;
		std::string        g_search;              // typed while the songbook is open
		std::map<std::string, int> g_plays;       // song title -> times you've played it (saved)
		std::set<std::string>      g_favourites;  // saved
		// Signature songs: a song you've played well to a room in a town again and again becomes yours there, and the
		// room cheers when it starts (saved: town -> song -> times it went down well there)
		std::map<RE::FormID, std::map<std::string, int>> g_signature;
		constexpr int                                    kSignatureAt = 4;
		std::vector<RE::FormID>                          g_present;  // who was in earshot at the last look

		// saved
		std::map<RE::FormID, float> g_bans;  // cell -> the day you were thrown out
		std::map<RE::FormID, float> g_tipped;  // listener -> the day they last tipped you (once a day each)

		// Your name in each town: how the place thinks of your playing (-1 notorious .. 1 beloved). It grows with
		// rooms you've pleased, falls with rooms you've emptied (and being thrown out), and fades as the days pass.
		struct Fame
		{
			float value = 0.0f;
			float day = 0.0f;  // when it was last brought up to date
		};
		std::map<RE::FormID, Fame> g_fame;          // a town's location -> your name there
		RE::FormID                 g_town = 0;      // where you're playing
		std::string                g_townName;
		double                     g_moodSum = 0.0, g_heardFor = 0.0;  // the room's mood while you played to it (listener-seconds)
		bool                       g_thrown = false;
		// The Bards College's luthier refits your instruments: 0 common, 1 fine, 2 masterwork
		int g_refit[2]{};
		int g_tierNow[2]{};  // what Tier() said last (the HUD shows it every frame; the pack is only looked through now and then)
		// the instrument comes out with the first note (or a song), and goes away again after a while of quiet
		bool   g_animOn = false;
		double g_lastNoteAt = 0.0;
		int                         g_totalEarned = 0;
		int                         g_performances = 0;

		std::mt19937 g_rng{ std::random_device{}() };

		double Now()
		{
			static const auto t0 = std::chrono::steady_clock::now();
			return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		}

		RE::PlayerCharacter* Player() { return RE::PlayerCharacter::GetSingleton(); }

		void Hud(const std::string& a_text) { RE::SendHUDMessage::ShowHUDMessage(a_text.c_str()); }

		void Say(std::string a_text, ImU32 a_color)
		{
			g_feed.push_back({ std::move(a_text), a_color, Now() });
			while (g_feed.size() > 7) {
				g_feed.pop_front();
			}
		}

		constexpr ImU32 kInk = IM_COL32(242, 236, 222, 255);
		constexpr ImU32 kGoldCol = IM_COL32(230, 190, 105, 255);
		constexpr ImU32 kGreen = IM_COL32(150, 215, 130, 255);
		constexpr ImU32 kRed = IM_COL32(235, 110, 95, 255);
		constexpr ImU32 kDim = IM_COL32(190, 180, 160, 255);

		template <class Self, class... Args>
		void Call(Self* a_self, const char* a_fn, Args... a_args)
		{
			if (!a_self) {
				return;
			}
			auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
			if (!vm) {
				return;
			}
			RE::BSScript::Variable v;
			RE::BSScript::PackValue(&v, a_self);
			auto obj = v.GetObject();
			if (!obj) {
				spdlog::warn("Papyrus call {}: no script object for {:08X}", a_fn, a_self->GetFormID());
				return;
			}
			auto*                                                    args = RE::MakeFunctionArguments(std::move(a_args)...);
			RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> cb;
			vm->DispatchMethodCall1(obj, a_fn, args, cb);
		}

		const char* InstName(Instrument a_i) { return a_i == Instrument::Lute ? "lute" : "flute"; }
		int         Base() { return (g_inst == Instrument::Lute ? 36 : 60) + g_octave * 12; }  // the bottom row's C: lute C2-C5, flute C4-C7

		// ------------------------------------------------------------------------------------------ instruments
		std::string Lower(const char* a_s)
		{
			std::string s = a_s ? a_s : "";
			std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return s;
		}

		// "lute" in a name or file name, but not inside "flute" (or "absolute", "resolute"...)
		bool HasWord(const std::string& a_s, const char* a_word)
		{
			for (auto at = a_s.find(a_word); at != std::string::npos; at = a_s.find(a_word, at + 1)) {
				const bool startOk = at == 0 || !std::isalpha(static_cast<unsigned char>(a_s[at - 1]));
				if (startOk) {
					return true;
				}
			}
			return false;
		}

		// 0 lute, 1 flute, -1 neither. The game's own (by form ID and model), and anything from a mod - misc item,
		// weapon, armour you wear on your back - with "lute" or "flute" in its name or its model's file name.
		int InstrumentKind(RE::TESBoundObject& a_obj)
		{
			for (int k = 0; k < 2; ++k) {
				if (g_instrumentIds[k].contains(a_obj.GetFormID())) {
					return k;
				}
			}
			std::string model;
			if (const auto* m = a_obj.As<RE::TESModel>(); m && m->GetModel()) {
				model = Lower(m->GetModel());
				for (int k = 0; k < 2; ++k) {
					if (g_instrumentModels[k].contains(model)) {
						return k;
					}
				}
				if (const auto slash = model.find_last_of("\\/"); slash != std::string::npos) {
					model = model.substr(slash + 1);
				}
			}
			const std::string name = Lower(a_obj.GetName());
			if (HasWord(name, "flute") || HasWord(model, "flute")) {
				return 1;
			}
			if (HasWord(name, "lute") || HasWord(model, "lute")) {
				return 0;
			}
			return -1;
		}

		bool IsInstrument(RE::TESBoundObject& a_obj, int a_k) { return InstrumentKind(a_obj) == a_k; }

		// what's in the pack that might be meant as an instrument, for the log when nothing is recognised
		void LogInstrumentLikeItems()
		{
			auto* p = Player();
			if (!p) {
				return;
			}
			int shown = 0;
			for (const auto& [obj, n] : p->GetInventoryCounts()) {
				if (!obj || n <= 0) {
					continue;
				}
				const std::string name = Lower(obj->GetName());
				std::string       model;
				if (const auto* m = obj->As<RE::TESModel>(); m && m->GetModel()) {
					model = Lower(m->GetModel());
				}
				if (name.find("lute") != std::string::npos || name.find("instrument") != std::string::npos || model.find("lute") != std::string::npos ||
					model.find("instrument") != std::string::npos || name.find("drum") != std::string::npos) {
					spdlog::info("  in your pack: {:08X} '{}' type {} model '{}' x{}", obj->GetFormID(), obj->GetName(), static_cast<int>(obj->GetFormType()), model, n);
					++shown;
				}
			}
			if (!shown) {
				spdlog::info("  nothing in your pack is named or modelled like a lute or flute");
			}
		}

		bool Owns(Instrument a_i)
		{
			if (!Config::Get().requireInstrument) {
				return true;
			}
			auto* p = Player();
			if (!p) {
				return false;
			}
			const int k = a_i == Instrument::Lute ? 0 : 1;
			for (const auto& [obj, n] : p->GetInventoryCounts([k](RE::TESBoundObject& a_obj) { return IsInstrument(a_obj, k); })) {
				if (n > 0) {
					return true;
				}
			}
			return false;
		}

		float Today() { return RE::Calendar::GetSingleton() ? RE::Calendar::GetSingleton()->GetDaysPassed() : 0.0f; }

		// the town a place belongs to: the location just under its hold (an inn -> Whiterun -> Whiterun Hold)
		RE::BGSLocation* TownOf(RE::Actor* a_actor)
		{
			auto* loc = a_actor ? a_actor->GetCurrentLocation() : nullptr;
			if (!loc) {
				return nullptr;
			}
			std::vector<RE::BGSLocation*> chain;
			for (auto* l = loc; l && chain.size() < 12; l = l->parentLoc) {
				chain.push_back(l);
			}
			for (std::size_t i = 1; i < chain.size(); ++i) {
				if (chain[i]->HasKeywordString("LocTypeHold")) {
					return chain[i - 1];
				}
			}
			return chain.size() >= 2 ? chain[chain.size() - 2] : chain.back();
		}

		// your name in a town, faded for the days since (it halves in about three weeks)
		float FameIn(RE::FormID a_town)
		{
			if (!a_town) {
				return 0.0f;
			}
			auto& f = g_fame[a_town];
			const float today = Today();
			if (today > f.day) {
				f.value *= std::pow(0.968f, today - f.day);
				f.day = today;
			}
			return f.value;
		}

		const char* FameWord(float a_v)
		{
			return a_v < -0.6f ? "Notorious" : a_v < -0.25f ? "Disliked" : a_v < 0.1f ? "A stranger" : a_v < 0.35f ? "Heard of" : a_v < 0.65f ? "Admired" : "Beloved";
		}

		// how fine an instrument is: the College's refit, or a master's own instrument from the College's quests
		int Tier(Instrument a_i)
		{
			const int k = a_i == Instrument::Lute ? 0 : 1;
			int       t = g_refit[k];
			if (auto* p = Player()) {
				const char* own = k == 0 ? "finn's lute" : "pantea's flute";
				for (const auto& [obj, n] : p->GetInventoryCounts([k](RE::TESBoundObject& a_obj) { return IsInstrument(a_obj, k); })) {
					if (n > 0 && Lower(obj->GetName()).find(own) != std::string::npos) {
						t = 3;  // the lost instrument itself: a legend
					}
				}
			}
			return t;
		}
		const char* TierWord(int a_t) { return a_t >= 3 ? "legendary " : a_t == 2 ? "masterwork " : a_t == 1 ? "fine " : ""; }
		// "fine lute", "Finn's lute"
		std::string InstLabel(Instrument a_i, int a_t)
		{
			if (a_t >= 3) {
				return a_i == Instrument::Lute ? "Finn's lute" : "Pantea's flute";
			}
			return fmt::format("{}{}", TierWord(a_t), InstName(a_i));
		}

		// The instrument in your hands. The bard idle is what puts it there, and IdleStop_Loose (the put-away) is the
		// only thing that takes it out again; starting one before the other has finished leaves both instruments
		// held. So: what should be held (g_want) and what is (g_held) are kept apart, and AnimTick moves one towards
		// the other a step at a time: put away, wait for it to finish, then take out. Each step is checked a moment
		// later (the game can accept an idle and still not play it) and tried again if it didn't take.
		int    g_want = -1, g_held = -1;  // -1 nothing, 0 lute, 1 flute
		double g_busyUntil = 0.0;         // a put-away is still playing until then
		double g_nextTry = 0.0;
		int    g_tries = 0;
		double g_checkAt = -1.0;          // when to see whether the last idle took
		int    g_checking = -1;           // what it should show then: 0/1 an instrument, 2 put away
		int    g_checks = 0;
		double g_firstPersonAt = -1.0;    // back to first person, once the put-away has played

		bool NativeIdle(RE::TESIdleForm* a_idle)
		{
			auto* p = Player();
			if (!p || !a_idle) {
				return false;
			}
			auto* proc = p->GetActorRuntimeData().currentProcess;
			return proc && proc->PlayIdle(p, a_idle, nullptr);
		}

		RE::TESIdleForm* LastIdle()
		{
			auto* p = Player();
			auto* proc = p ? p->GetActorRuntimeData().currentProcess : nullptr;
			return proc && proc->middleHigh ? proc->middleHigh->lastIdlePlayed : nullptr;
		}

		const char* IdleName(RE::TESIdleForm* a_idle)
		{
			return !a_idle ? "none" : a_idle == g_idles[0] ? "lute" : a_idle == g_idles[1] ? "flute" : a_idle == g_stopIdle ? "put-away" : "other";
		}

		void IssueStop()
		{
			if (g_stopIdle && NativeIdle(g_stopIdle)) {
				spdlog::info("Putting the instrument away");
			} else if (g_stopIdle) {
				spdlog::info("Putting the instrument away (through Papyrus)");
				Call(Player(), "PlayIdle", g_stopIdle);
			} else if (auto* p = Player()) {
				p->NotifyAnimationGraph("IdleForceDefaultState");
			}
		}

		void AnimTick(double a_now)
		{
			// did the last step take?
			if (g_checkAt > 0.0 && a_now >= g_checkAt) {
				g_checkAt = -1.0;
				auto* last = LastIdle();
				if (g_checking == 2) {
					if (last == g_idles[0] || last == g_idles[1]) {
						spdlog::info("The put-away didn't take (last idle: {}); again", IdleName(last));
						if (++g_checks <= 3) {
							IssueStop();
							g_busyUntil = a_now + 1.6;
							g_checking = 2;
							g_checkAt = a_now + 0.6;
						}
					} else {
						g_checks = 0;
					}
				} else if (g_checking >= 0 && g_held == g_checking) {
					if (last == nullptr || last == g_stopIdle) {
						spdlog::info("The {} idle didn't take (last idle: {}); again", g_checking == 0 ? "lute" : "flute", IdleName(last));
						if (++g_checks <= 3) {
							g_held = -1;  // not out after all: take it out again
							g_nextTry = a_now;
						}
					} else {
						g_checks = 0;
					}
				}
			}
			if (g_held == g_want) {
				return;
			}
			if (g_held != -1) {
				// the wrong instrument (or one that should be away) is out: put it away first
				IssueStop();
				g_held = -1;
				g_busyUntil = a_now + 1.6;
				g_checking = 2;
				g_checkAt = a_now + 0.6;
				g_checks = 0;
				return;
			}
			if (g_want == -1 || a_now < g_busyUntil || a_now < g_nextTry) {
				return;
			}
			auto* idle = g_idles[g_want];
			if (!idle) {
				spdlog::warn("No {} idle to play", g_want == 0 ? "lute" : "flute");
				g_want = -1;
				return;
			}
			if (NativeIdle(idle)) {
				spdlog::info("Taking out the {} (idle {:08X}, try {})", g_want == 0 ? "lute" : "flute", idle->GetFormID(), g_tries + 1);
				g_held = g_want;
				g_tries = 0;
				g_checking = g_held;
				g_checkAt = a_now + 0.8;
				return;
			}
			if (++g_tries < 12) {
				g_nextTry = a_now + 0.25;
				return;
			}
			spdlog::warn("The game refused the {} idle twelve times; asking through Papyrus", g_want == 0 ? "lute" : "flute");
			Call(Player(), "PlayIdle", idle);
			g_held = g_want;
			g_tries = 0;
		}

		// the first note (or a song) brings the instrument out; a while of quiet puts it away (see Tick)
		void EnsureAnim()
		{
			g_lastNoteAt = Now();
			g_animOn = true;
			g_want = g_inst == Instrument::Lute ? 0 : 1;
		}

		void PutAway()
		{
			g_animOn = false;
			g_want = -1;
		}

		// ------------------------------------------------------------------------------------------ gold
		int GoldOf(RE::Actor* a_actor)
		{
			// (not Actor::GetGoldAmount: it goes through the default-object table, which crashed on 1.5.97 in Yacht)
			if (!a_actor) {
				return 0;
			}
			for (const auto& [obj, n] : a_actor->GetInventoryCounts([](RE::TESBoundObject& a_obj) { return a_obj.GetFormID() == kGold; })) {
				return n;
			}
			return 0;
		}

		void GiveGold(int a_n)
		{
			auto* p = Player();
			if (!p || !g_gold || a_n <= 0) {
				return;
			}
			p->AddObjectToContainer(g_gold, nullptr, a_n, nullptr);
			g_earned += a_n;
			g_totalEarned += a_n;
			if (Config::Get().speechXp > 0.0f) {
				p->AddSkillExperience(RE::ActorValue::kSpeech, a_n * Config::Get().speechXp);
			}
			if (auto* s = Audio::Synth()) {
				s->Coin(std::clamp(0.4f + a_n * 0.05f, 0.4f, 1.0f));
			}
		}

		double Speech01()
		{
			auto* p = Player();
			if (!p) {
				return 0.0;
			}
			return std::clamp(p->AsActorValueOwner()->GetActorValue(RE::ActorValue::kSpeech) / 100.0, 0.0, 1.0);
		}

		// ------------------------------------------------------------------------------------------ the game's music
		void MuteMusic(bool a_mute)
		{
			// the music: a silent music type on top of the game's (the way Skyrim's Got Talent does it)
			if (a_mute && Config::Get().muteMusic && (g_silence || g_tavernSilence) && !g_silenced) {
				Call(g_silence, "Add");
				Call(g_tavernSilence, "Add");
				g_silenced = true;
			} else if (!a_mute && g_silenced) {
				Call(g_silence, "Remove");
				Call(g_tavernSilence, "Remove");
				g_silenced = false;
			}
			if (a_mute) {
				if (!Config::Get().muteMusic || !g_muted.empty()) {
					return;
				}
				for (auto* c : g_musicCats) {
					g_muted.push_back({ c, c->GetCategoryVolume() });
					c->SetCategoryVolume(0.0f);
				}
			} else {
				for (auto& [c, v] : g_muted) {
					c->SetCategoryVolume(v);
				}
				g_muted.clear();
			}
		}

		// A bard's song goes on as music after their performance (the scene) is stopped: while you play, any bard
		// music that's playing or queued is taken off. Whatever else plays is logged once, to know what to hush.
		void HushMusic()
		{
			auto* mm = RE::BSMusicManager::GetSingleton();
			if (!mm || !Config::Get().muteMusic) {
				return;
			}
			static std::set<RE::FormID> s_logged;
			auto check = [&](RE::BSIMusicType* a_t) {
				auto* m = a_t ? skyrim_cast<RE::BGSMusicType*>(a_t) : nullptr;
				if (!m || m == g_silence || m == g_tavernSilence) {
					return;
				}
				const std::string id = Lower(m->GetFormEditorID());
				const bool        bard = id.find("bard") != std::string::npos;
				if (s_logged.insert(m->GetFormID()).second) {
					spdlog::info("Music while you play: {} {:08X} (priority {}){}", m->GetFormEditorID(), m->GetFormID(), m->priority, bard ? ": a bard's, hushed" : "");
				}
				if (bard) {
					Call(m, "Remove");
				}
			};
			check(mm->current);
			for (auto* t : mm->musicQueue) {
				check(t);
			}
		}

		// ------------------------------------------------------------------------------------------ notes
		void SetInst(Instrument a_i)
		{
			g_inst = a_i;
			g_instA = static_cast<int>(a_i);
		}

		// a note you played: lit on the keyboard and judged
		void Register(int a_pitch, double a_now, double a_songT)
		{
			g_lit[a_pitch] = a_now;
			EnsureAnim();
			switch (g_act) {
			case Act::Free:
				g_free.Note(a_now, a_pitch);
				break;
			case Act::Along:
				g_chart.Note(a_songT - Config::Get().latencyMs / 1000.0, a_pitch);
				break;
			case Act::Auto:
				break;
			}
		}

		void Sound(int a_pitch, float a_vel)
		{
			if (auto* s = Audio::Synth()) {
				s->NoteOn(g_inst, a_pitch, a_vel, true);
			}
			Register(a_pitch, Now(), g_act == Act::Along ? Audio::SongTime() : 0.0);
		}

		void Unsound(int a_pitch)
		{
			if (auto* s = Audio::Synth()) {
				s->NoteOff(g_inst, a_pitch, true);
			}
		}

		// On the MIDI driver's thread: the note sounds straight away (the synth takes its own lock); the rest waits
		// for the frame. Never takes the performance's lock, so closing the device can't deadlock with it.
		void OnMidi(int a_status, int a_d1, int a_d2)
		{
			if (!g_midiLive.load()) {
				return;
			}
			auto* s = Audio::Synth();
			if (!s) {
				return;
			}
			const auto inst = static_cast<Instrument>(g_instA.load());
			const int  kind = a_status & 0xF0;
			if (kind == 0x90 && a_d2 > 0) {
				s->NoteOn(inst, a_d1, std::clamp(a_d2 / 127.0f, 0.1f, 1.0f), true);
				const MidiHit hit{ a_d1, Now(), Audio::SongActive() ? Audio::SongTime() : 0.0 };
				std::lock_guard lk(g_midiMtx);
				if (g_midiQ.size() < 512) {
					g_midiQ.push_back(hit);
				}
			} else if (kind == 0x80 || (kind == 0x90 && a_d2 == 0)) {
				s->NoteOff(inst, a_d1, true);
			} else if (kind == 0xB0 && a_d1 == 64) {
				s->Sustain(a_d2 >= 64);
			} else if (kind == 0xB0 && (a_d1 == 120 || a_d1 == 123)) {
				s->AllOff();
			}
		}

		void ReleaseAllKeys()
		{
			for (const auto& [scan, pitch] : g_keyPitch) {
				Unsound(pitch);
			}
			g_keyPitch.clear();
			if (auto* s = Audio::Synth()) {
				s->Sustain(false);
				s->AllOff();
			}
		}

		void RefreshOwned()
		{
			g_owns[0] = Owns(Instrument::Lute);
			g_owns[1] = Owns(Instrument::Flute);
		}

		// ------------------------------------------------------------------------------------------ the songbook
		void LoadSongs()
		{
			std::error_code ec;
			std::filesystem::create_directories(kSongDir, ec);
			std::vector<Entry> found;
			for (const auto& de : std::filesystem::directory_iterator(kSongDir, ec)) {
				auto ext = de.path().extension().string();
				std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
				if (ext != ".mid" && ext != ".midi" && ext != ".rmi") {
					continue;
				}
				const auto mtime = de.last_write_time(ec);
				// already read and unchanged
				auto old = std::find_if(g_files.begin(), g_files.end(), [&](const Entry& a) { return a.path == de.path(); });
				if (old != g_files.end() && old->mtime == mtime) {
					found.push_back(std::move(*old));
					continue;
				}
				Entry e;
				e.path = de.path();
				e.mtime = mtime;
				const auto title = e.path.stem().u8string();
				std::ifstream f(e.path, std::ios::binary);
				std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
				if (bytes.size() > 8 * 1024 * 1024) {
					e.error = "too big";
					e.song.title = std::string(title.begin(), title.end());
				} else if (!music::SongFromMidi(bytes, std::string(title.begin(), title.end()), e.song, &e.error)) {
					e.song.title = std::string(title.begin(), title.end());
					spdlog::warn("Songbook: {}: {}", e.path.filename().string(), e.error);
				} else {
					spdlog::info("Songbook: {} ({} notes, melody {}, {:.0f} s)", e.path.filename().string(), e.song.all.size(), e.song.melody.size(),
						e.song.length);
				}
				found.push_back(std::move(e));
			}
			std::sort(found.begin(), found.end(), [](const Entry& a, const Entry& b) { return _stricmp(a.song.title.c_str(), b.song.title.c_str()) < 0; });
			g_files = std::move(found);
		}

		// what a key types, on this keyboard's layout (for the songbook's search)
		char CharOf(int a_scan, bool a_shift)
		{
			const UINT vk = ::MapVirtualKeyA(static_cast<UINT>(a_scan), MAPVK_VSC_TO_VK);
			if (!vk) {
				return 0;
			}
			BYTE ks[256]{};
			if (a_shift) {
				ks[VK_SHIFT] = 0x80;
			}
			WCHAR buf[4]{};
			const int n = ::ToUnicode(vk, static_cast<UINT>(a_scan), ks, buf, 4, 0x4);  // 0x4: leave the keyboard's state alone
			return n == 1 && buf[0] >= 32 && buf[0] < 127 ? static_cast<char>(buf[0]) : 0;
		}

		bool Matches(const std::string& a_title)
		{
			if (g_search.empty()) {
				return true;
			}
			return Lower(a_title.c_str()).find(Lower(g_search.c_str())) != std::string::npos;
		}

		void BackToFree()
		{
			Audio::StopSong();
			g_act = Act::Free;
			g_song = {};
			g_free.Reset();
		}

		void StartAlong(const music::Song& a_song)
		{
			BackToFree();
			g_song = a_song;
			g_songShift = music::FitOctaves(g_song.melody, Base(), 36);
			std::vector<music::Note> chart = g_song.melody;
			for (auto& n : chart) {
				n.pitch += g_songShift;
			}
			g_chart.Load(std::move(chart));
			g_act = Act::Along;
			g_lastClick = -99.0;
			EnsureAnim();
			Audio::PlaySong({}, g_inst, Config::Get().speed, 3.0, false);
			Say(fmt::format("Play along: {}", g_song.title), kGoldCol);
			spdlog::info("Play along: {} ({} notes, shifted {} semitones, speed {:.2f})", g_song.title, g_chart.Chart().size(), g_songShift, Config::Get().speed);
		}

		void CheerSignature();

		void StartAuto(const music::Song& a_song)
		{
			BackToFree();
			g_song = a_song;
			// the song as written, every note from C2 to A7 (like a sequencer's piano roll); anything beyond is
			// moved by octaves into it
			g_songShift = 0;
			const std::size_t before = g_song.all.size();
			auto              notes = music::FoldIntoRange(g_song.all);
			spdlog::info("  {} notes, {} doubled ones dropped after folding into the {}'s range", before, before - notes.size(), InstName(g_inst));
			g_song.all = notes;
			g_act = Act::Auto;
			EnsureAnim();
			++g_plays[g_song.title];
			CheerSignature();
			Audio::PlaySong(notes, g_inst, Config::Get().speed, 1.0, true);
			Say(fmt::format("You play {}.", g_song.title), kDim);
			spdlog::info("Let it play: {} ({} notes)", g_song.title, notes.size());
		}

		// ------------------------------------------------------------------------------------------ the audience moves
		// Listeners react with the game's own audience idles: a tipper hands the coin over, a delighted room claps,
		// dances and raises a mug, an unhappy one scratches its head and points. Happy listeners also come over to
		// stand in front of you while you play (KeepOffsetFromActor, which overrides their AI until you stop).
		constexpr int kMaxVisitors = 4;
		// A visitor walks (PathToReference) to an invisible marker on an arc in front of where you stood when you
		// took the instrument out, then stays put (SetDontMove), turned to face you and looking at you. (Not
		// KeepOffsetFromActor: that keeps them at an offset in your own frame, so every turn of the camera swung
		// them round you in circles.)
		constexpr RE::FormID kXMarker = 0x0000003B;
		struct Visitor
		{
			RE::FormID          id = 0;
			int                 slot = 0;
			double              leaveAt = 0.0;  // they have places to be: each stays a while, then wanders off mid-song
			double              giveUpAt = 0.0;  // couldn't get there
			RE::ObjectRefHandle marker;
			RE::NiPoint3        spot;
			bool                arrived = false;
		};
		RE::NiPoint3 g_anchor;
		float        g_anchorHeading = 0.0f;
		std::unordered_map<RE::FormID, double> g_awayUntil;  // wandered off: not coming back for a while
		std::vector<Visitor>                     g_visitors;
		std::unordered_map<RE::FormID, double>   g_nextMove;  // listener -> when they may move again
		std::vector<RE::FormID>                  g_staleVisitors;  // saved mid-performance: sent home after the load

		RE::Actor* ActorOf(RE::FormID a_id) { return RE::TESForm::LookupByID<RE::Actor>(a_id); }
		bool       IsVisitor(RE::FormID a_id)
		{
			return std::any_of(g_visitors.begin(), g_visitors.end(), [&](const Visitor& v) { return v.id == a_id; });
		}

		std::string NameOfActor(RE::Actor* a_actor)
		{
			const char* n = a_actor ? a_actor->GetDisplayFullName() : nullptr;
			return n && *n ? n : "Someone";
		}

		bool NpcIdle(RE::Actor* a_actor, RE::TESIdleForm* a_idle)
		{
			if (!a_actor || !a_idle || a_actor->IsDead() || !a_actor->Is3DLoaded()) {
				return false;
			}
			auto* proc = a_actor->GetActorRuntimeData().currentProcess;
			return proc && proc->PlayIdle(a_actor, a_idle, nullptr);
		}

		bool Seated(RE::Actor* a_actor)
		{
			auto* st = a_actor ? a_actor->AsActorState() : nullptr;
			return st && st->GetSitSleepState() != RE::SIT_SLEEP_STATE::kNormal;
		}

		// turn to you first: an idle plays the way the actor faces, and many were facing away
		void FacePlayer(RE::Actor* a_actor)
		{
			auto* player = Player();
			if (!a_actor || !player) {
				return;
			}
			if (Seated(a_actor)) {
				// a chair holds them where they sit: they turn their head and watch you instead
				if (g_lookers.insert(a_actor->GetFormID()).second) {
					Call(a_actor, "SetLookAt", static_cast<RE::TESObjectREFR*>(player), false);
				}
				return;
			}
			const auto at = a_actor->GetPosition(), me = player->GetPosition();
			a_actor->SetHeading(std::atan2(me.x - at.x, me.y - at.y));  // Skyrim's heading: 0 = north (+y), clockwise
		}

		template <class T>
		const T& PickOf(const std::vector<T>& a_v)
		{
			return a_v[std::uniform_int_distribution<std::size_t>(0, a_v.size() - 1)(g_rng)];
		}

		// The Bards College, from the game's own quests: a member after Tending the Flames, and the instruments you
		// brought back (Finn's Lute, Pantea's Flute, Rjorn's Drum)
		struct College
		{
			bool member = false;
			bool lute = false, flute = false, drum = false;
			int  Returned() const { return (lute ? 1 : 0) + (flute ? 1 : 0) + (drum ? 1 : 0); }
		};

		College CollegeStanding()
		{
			auto done = [](const char* a_id, std::uint16_t a_stage) {
				auto* q = RE::TESForm::LookupByEditorID<RE::TESQuest>(a_id);
				return q && (q->IsCompleted() || (a_stage && q->GetCurrentStageID() >= a_stage));
			};
			College c;
			c.member = done("MS05", 300);
			c.lute = done("BardsCollegeLute", 0);
			c.flute = done("BardsCollegeFlute", 0);
			c.drum = done("BardsCollegeDrum", 0);
			return c;
		}
		College g_college;  // looked up when you take your instrument out
		bool    g_atCollege = false;   // playing in the College's own hall: students gather, teachers judge
		int     g_collegeShows = 0;    // performances the College took note of (saved): word from the College goes before you
		bool    g_recognised = false;  // someone has remarked on the lost instrument you're playing (once a performance)
		bool IsStudent(const std::string& a_name) { return a_name == "Aia Arria" || a_name == "Illdi" || a_name == "Ataf"; }

		// The College's own people, by name: they say what a teacher would
		struct Teacher
		{
			const char*              name;
			std::vector<std::string> praise, scold;
		};
		const Teacher* TeacherOf(const std::string& a_name)
		{
			static const std::vector<Teacher> kTeachers = {
				{ "Viarmo", { "Splendid! The College could use more like you.", "That's how it's done. Remember that feeling.", "Olaf himself would have stopped to listen." },
					{ "No, no, no. Again, from the top.", "You embarrass the College with that.", "Have you been practising at all?" } },
				{ "Inge Six Fingers", { "Hmph. Not bad. There's feeling in it.", "Your fingers know the way. Finally.", "Finn would have smiled at that." },
					{ "Your fingering's sloppy. Start past four and it shows.", "That's not a lute, that's a hen house.", "Mind your courses, they're buzzing." } },
				{ "Pantea Ateia", { "Lovely breath. You listened to me.", "Now that is a flute singing.", "Yes! Let it float." },
					{ "Breathe from the belly, not the chest!", "You're blowing, not playing.", "Mind your embouchure, it's all air." } },
				{ "Giraud Gemane", { "A fine performance. Worth writing about.", "Now that has a story in it.", "Bravo, bravo." },
					{ "Even history wouldn't record that.", "Perhaps stick to the written word.", "Hm. Interesting choices." } },
				{ "Jorn", { "Ha! You'll take my spot at the Festival.", "Now that's a song!", "Play that again tonight!" },
					{ "Even I've heard better at the Winking Skeever.", "Easy there, you'll wake the dead.", "Rough night?" } },
				{ "Aia Arria", { "That was beautiful.", "I'd love to learn that one.", "You make it look easy." }, { "Oh. Oh dear.", "Maybe a little softer?", "Was that on purpose?" } },
				{ "Illdi", { "Oh, that's lovely!", "I hope my piece sounds half as good.", "You have to teach me that!" }, { "Um... was that the classic?", "Inge would have words.", "Oof." } },
				{ "Ataf", { "Good. Very good.", "Your rhythm is strong.", "That one moves the feet." }, { "Lost the beat there.", "Slower. Feel it first.", "Again, with a steadier hand." } },
			};
			for (const auto& t : kTeachers) {
				if (a_name == t.name) {
					return &t;
				}
			}
			return nullptr;
		}

		std::string NameOf(std::uint32_t a_id)
		{
			auto it = g_names.find(a_id);
			return it == g_names.end() ? "Someone" : it->second;
		}

		void React(RE::FormID a_id, int a_kind)  // 0 tip, 1 cheer/applause, 2 boo, 3 enjoy (dance / drink)
		{
			auto* a = ActorOf(a_id);
			if (!a || !Config::Get().reactions) {
				return;
			}
			FacePlayer(a);
			switch (a_kind) {
			case 0:
				NpcIdle(a, g_give);
				break;
			case 1:
				if (!g_applaud.empty()) {
					NpcIdle(a, PickOf(g_applaud));
				}
				break;
			case 2:
				a->NotifyAnimationGraph(kBooEvents[std::uniform_int_distribution<int>(0, 3)(g_rng)]);
				break;
			default:
				{
					const bool dance = !g_dance.empty() && std::uniform_int_distribution<int>(0, 5)(g_rng) == 0;  // a dance now and then
					const auto& set = dance ? g_dance : (g_enjoy.empty() ? g_applaud : g_enjoy);
					if (!set.empty()) {
						NpcIdle(a, PickOf(set));
					}
				}
				break;
			}
			g_nextMove[a_id] = Now() + 15.0 + std::uniform_real_distribution<double>(0.0, 20.0)(g_rng);
		}

		void ReleaseActor(RE::Actor* a_actor)
		{
			if (!a_actor) {
				return;
			}
			Call(a_actor, "SetDontMove", false);
			Call(a_actor, "ClearLookAt");
			Call(a_actor, "EvaluatePackage");  // back to their own business (ends the walk, if they were still on it)
		}

		void SendAway(const Visitor& a_v)
		{
			ReleaseActor(ActorOf(a_v.id));
			if (auto m = a_v.marker.get()) {
				Call(m.get(), "Delete");
			}
		}

		void SendAllAway()
		{
			for (const auto& v : g_visitors) {
				SendAway(v);
			}
			g_visitors.clear();
			g_nextMove.clear();
			g_awayUntil.clear();
		}

		// every half second: who comes over, who drifts off, and the room's own little movements
		void MoveAudience(const std::vector<std::uint32_t>& a_present, RE::FormID a_keeper)
		{
			const auto& cfg = Config::Get();
			auto*       player = Player();
			if (!player) {
				return;
			}
			const double now = Now();
			// visitors who've reached their spot stop there, turn to you and watch
			for (auto& v : g_visitors) {
				if (v.arrived) {
					continue;
				}
				auto* a = ActorOf(v.id);
				if (!a) {
					continue;
				}
				const auto at = a->GetPosition();
				if (std::hypot(at.x - v.spot.x, at.y - v.spot.y) < 70.0f) {
					v.arrived = true;
					Call(a, "SetDontMove", true);
					FacePlayer(a);
					Call(a, "SetLookAt", static_cast<RE::TESObjectREFR*>(player), false);
					g_nextMove[v.id] = now + 3.0;
				}
			}
			// visitors who've cooled off go back to what they were doing, and any of them may just wander off
			std::erase_if(g_visitors, [&](const Visitor& v) {
				const double mood = g_crowd.MoodOf(v.id);
				if (!v.arrived && now >= v.giveUpAt) {
					SendAway(v);  // couldn't find a way: never mind
					g_awayUntil[v.id] = now + 120.0;
					return true;
				}
				if (mood < 0.45 || now >= v.leaveAt) {
					SendAway(v);
					if (mood >= 0.45) {
						g_awayUntil[v.id] = now + std::uniform_real_distribution<double>(90.0, 240.0)(g_rng);
						if (auto* a = ActorOf(v.id)) {
							Say(fmt::format("{} wanders off.", NameOfActor(a)), kDim);
						}
					}
					return true;
				}
				return false;
			});
			// happy listeners come over (not the innkeeper: someone has to mind the bar)
			const int most = (std::min)(cfg.maxVisitors, kMaxVisitors);
			for (auto id : a_present) {
				if (static_cast<int>(g_visitors.size()) >= most) {
					break;
				}
				if (auto away = g_awayUntil.find(id); away != g_awayUntil.end() && now < away->second) {
					continue;
				}
				const double keen = g_atCollege && IsStudent(NameOf(id)) ? 0.5 : 0.65;  // students crowd round
				if (id == a_keeper || g_crowd.MoodOf(id) < keen ||
					std::any_of(g_visitors.begin(), g_visitors.end(), [&](const Visitor& v) { return v.id == id; })) {
					continue;
				}
				auto* a = ActorOf(id);
				if (!a || a->IsPlayerTeammate()) {
					continue;  // followers are with you already
				}
				int slot = 0;
				while (std::any_of(g_visitors.begin(), g_visitors.end(), [&](const Visitor& v) { return v.slot == slot; })) {
					++slot;
				}
				// a little arc in front of where you stood (in the world, not your frame), each with a marker to walk to
				static constexpr float kAngles[kMaxVisitors] = { -20.0f, 25.0f, -55.0f, 60.0f };
				const float            ang = g_anchorHeading + kAngles[slot] * 3.14159265f / 180.0f, r = 210.0f;
				const RE::NiPoint3     spot{ g_anchor.x + r * std::sin(ang), g_anchor.y + r * std::cos(ang), g_anchor.z };
				auto*                  markerBase = RE::TESForm::LookupByID<RE::TESBoundObject>(kXMarker);
				auto                   marker = markerBase ? player->PlaceObjectAtMe(markerBase, false) : RE::NiPointer<RE::TESObjectREFR>{};
				if (!marker) {
					continue;
				}
				marker->SetPosition(spot);
				Call(a, "PathToReference", marker.get(), 0.5f);  // a walk
				Visitor v;
				v.id = id;
				v.slot = slot;
				// how long they stay: anything from half a minute to a few minutes
				v.leaveAt = now + std::uniform_real_distribution<double>(30.0, 180.0)(g_rng);
				v.giveUpAt = now + 25.0;
				v.marker = marker->GetHandle();
				v.spot = spot;
				g_visitors.push_back(v);
				g_nextMove[id] = now + 1e9;  // no moves until they've got there
				spdlog::info("{} comes over to listen", NameOfActor(a));
			}
			// the room's movements
			if (!cfg.reactions) {
				return;
			}
			for (auto id : a_present) {
				auto it = g_nextMove.find(id);
				if (it != g_nextMove.end() && now < it->second) {
					continue;
				}
				const double mood = g_crowd.MoodOf(id);
				if (mood > 0.75 && std::uniform_int_distribution<int>(0, 2)(g_rng) == 0) {
					React(id, std::uniform_int_distribution<int>(0, 1)(g_rng) == 0 ? 1 : 3);
				} else if (mood >= 0.0 && mood < 0.3 && std::uniform_int_distribution<int>(0, 3)(g_rng) == 0) {
					React(id, 2);
				} else {
					g_nextMove[id] = now + 4.0 + std::uniform_real_distribution<double>(0.0, 6.0)(g_rng);
				}
			}
		}

		// ------------------------------------------------------------------------------------------ the crowd
		// a beggar: the Beggar class (Edda, Snilf, Brenuin...), or named as one
		bool IsBeggar(RE::Actor* a_actor)
		{
			auto* base = a_actor ? a_actor->GetActorBase() : nullptr;
			auto  has = [](const char* a_s) {
				if (!a_s) {
					return false;
				}
				std::string l = a_s;
				std::transform(l.begin(), l.end(), l.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
				return l.find("beggar") != std::string::npos;
			};
			return (base && base->npcClass && has(base->npcClass->GetFullName())) || has(a_actor->GetDisplayFullName());
		}

		// the inn's bard stops while you play: Skyrim's own "stop playing" (BardSongsScript.StopAllSongs), asked again
		// now and then, as a bard will start up again by themselves after a while
		void HushBards()
		{
			const double now = Now();
			if (now < g_nextBardHush) {
				return;
			}
			g_nextBardHush = now + 8.0;
			static RE::TESQuest* quest = [] {
				auto* q = RE::TESForm::LookupByEditorID<RE::TESQuest>("BardSongs");
				if (!q) {
					q = RE::TESForm::LookupByID<RE::TESQuest>(0x74A55);
				}
				spdlog::info("Bards: {}", q ? fmt::format("BardSongs quest {:08X}", q->GetFormID()) : std::string("BardSongs quest not found"));
				return q;
			}();
			if (quest && quest->IsRunning()) {
				Call(quest, "StopAllSongs");
			}
		}

		void RemarkOnInstrument(const std::vector<std::uint32_t>& a_present);

		void ScanListeners()
		{
			auto* pl = RE::ProcessLists::GetSingleton();
			auto* player = Player();
			if (!pl || !player) {
				return;
			}
			auto*      cell = player->GetParentCell();
			const bool interior = cell && cell->IsInteriorCell();
			const auto here = player->GetPosition();
			const float range = Config::Get().listenRange;
			std::vector<std::uint32_t> ids, beggars;
			g_keeper = 0;
			for (auto& h : pl->highActorHandles) {
				auto ap = h.get();
				auto* a = ap.get();
				if (!a || a->IsPlayerRef() || a->IsDead() || a->IsChild() || !a->Is3DLoaded() || a->IsDisabled()) {
					continue;
				}
				if (a->IsInCombat() || a->IsHostileToActor(player)) {
					continue;
				}
				auto* race = a->GetRace();
				if (!race || !race->HasKeywordID(kActorTypeNPC)) {
					continue;  // people, not dogs and horses
				}
				if (interior && a->GetParentCell() != cell) {
					continue;
				}
				if (a->GetPosition().GetDistance(here) > range) {
					continue;
				}
				if (auto* st = a->AsActorState(); st && st->GetSitSleepState() == RE::SIT_SLEEP_STATE::kIsSleeping) {
					continue;
				}
				const auto id = a->GetFormID();
				ids.push_back(id);
				const char* name = a->GetDisplayFullName();
				g_names[id] = name && *name ? name : "Someone";
				if (g_innkeepers && a->IsInFaction(g_innkeepers)) {
					g_keeper = id;
				}
				// beggars listen, but have nothing to give
				if (IsBeggar(a)) {
					beggars.push_back(id);
				}
			}
			// who already tipped you today
			const float               today = RE::Calendar::GetSingleton() ? std::floor(RE::Calendar::GetSingleton()->GetDaysPassed()) : 0.0f;
			std::vector<std::uint32_t> noTips = beggars;
			for (auto id : ids) {
				if (auto it = g_tipped.find(id); it != g_tipped.end() && std::floor(it->second) >= today) {
					noTips.push_back(id);
				}
			}
			g_crowd.Sync(ids, &noTips);
			g_present = ids;
			for (auto id : ids) {
				const std::string n = NameOf(id);
				if (IsStudent(n)) {
					g_crowd.SetTaste(id, 0.85);  // students are glad of any music
				} else if (TeacherOf(n)) {
					g_crowd.SetTaste(id, 1.45);  // their teachers are not
				}
			}
			RemarkOnInstrument(ids);
			MoveAudience(ids, g_keeper);
			for (auto id : ids) {
				if (auto* a = ActorOf(id); a && Seated(a)) {
					FacePlayer(a);
				}
			}
			for (auto it = g_lookers.begin(); it != g_lookers.end();) {
				auto* a = ActorOf(*it);
				if (!a || std::find(ids.begin(), ids.end(), *it) == ids.end() || !Seated(a)) {
					if (a && !Seated(a)) {
						// stood up: let them be (a visitor's own look-at is set when they arrive)
						if (!IsVisitor(*it)) {
							Call(a, "ClearLookAt");
						}
					} else if (a) {
						Call(a, "ClearLookAt");
					}
					it = g_lookers.erase(it);
				} else {
					++it;
				}
			}
			HushBards();
			bool innLoc = false;
			if (auto* loc = player->GetCurrentLocation()) {
				innLoc = loc->HasKeywordID(kLocTypeInn);
			}
			g_inInn = interior && (g_keeper != 0 || innLoc);
		}


		void MarkTipped(RE::FormID a_id)
		{
			g_tipped[a_id] = RE::Calendar::GetSingleton() ? RE::Calendar::GetSingleton()->GetDaysPassed() : 0.0f;
		}

		void HandleCrowd(const std::vector<music::CrowdEvent>& a_eventsIn)
		{
			// the College's teachers speak as teachers
			std::vector<music::CrowdEvent> a_events = a_eventsIn;
			for (auto& e : a_events) {
				if (const auto* t = TeacherOf(NameOf(e.id))) {
					if (e.kind == music::CrowdEvent::Kind::Heckle) {
						e.line = PickOf(t->scold);
					} else if (e.kind == music::CrowdEvent::Kind::Cheer || e.kind == music::CrowdEvent::Kind::Tip) {
						e.line = PickOf(t->praise);
					}
				}
			}
			for (const auto& e : a_events) {
				switch (e.kind) {
				case music::CrowdEvent::Kind::Tip:
					GiveGold(e.gold);
					MarkTipped(e.id);
					React(e.id, 0);
					Say(e.line.empty() ? fmt::format("{} tosses you {} septim{}.", NameOf(e.id), e.gold, e.gold == 1 ? "" : "s") :
					                     fmt::format("{} tosses you {} septim{}. \"{}\"", NameOf(e.id), e.gold, e.gold == 1 ? "" : "s", e.line),
						kGoldCol);
					break;
				case music::CrowdEvent::Kind::Heckle:
					React(e.id, 2);
					Say(fmt::format("{}: \"{}\"", NameOf(e.id), e.line), kRed);
					break;
				case music::CrowdEvent::Kind::Cheer:
					React(e.id, 1);
					Say(fmt::format("{}: \"{}\"", NameOf(e.id), e.line), kGreen);
					break;
				case music::CrowdEvent::Kind::Applause:
					GiveGold(e.gold);
					for (auto id : e.payers) {
						MarkTipped(id);
					}
					for (auto& [id, name] : g_names) {
						if (g_crowd.MoodOf(id) >= 0.5) {
							React(id, 1);
						}
					}
					Say(e.gold > 0 ? fmt::format("Applause! The room puts together {} septims. {}: \"{}\"", e.gold, NameOf(e.id), e.line) :
					                 fmt::format("Applause! {}: \"{}\"", NameOf(e.id), e.line),
						kGreen);
					break;
				}
			}
		}

		// ------------------------------------------------------------------------------------------ start / stop
		// The hotkey is a key's place on the keyboard (a scan code). The default, 0x2B, is \ on a US keyboard; on a UK
		// or other ISO keyboard \ is the key beside left Shift (0x56) and 0x2B is #. So while the hotkey is left at
		// its default, the key that types \ on this keyboard's layout works too.
		int g_layoutHotkey = -1;

		void FindLayoutHotkey()
		{
			g_layoutHotkey = -1;
			const HKL   layout = ::GetKeyboardLayout(0);
			const SHORT vks = ::VkKeyScanExW(L'\\', layout);
			if (vks == -1 || (vks >> 8) != 0) {
				spdlog::info("Keyboard layout {:08X}: \\ needs a modifier (or isn't there); the hotkey is the key at 0x{:X}",
					static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(layout)), Config::Get().hotkey);
				return;
			}
			const UINT sc = ::MapVirtualKeyExW(static_cast<UINT>(vks & 0xFF), MAPVK_VK_TO_VSC, layout);
			if (sc && static_cast<int>(sc) != 0x2B) {
				g_layoutHotkey = static_cast<int>(sc);
			}
			spdlog::info("Keyboard layout {:08X}: \\ is the key at 0x{:X}{}", static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(layout)), sc,
				g_layoutHotkey > 0 ? " (taken as the hotkey too, as it's left at its default)" : "");
		}

		bool IsHotkey(int a_scan)
		{
			const int hk = Config::Get().hotkey;
			return a_scan == hk || (hk == 0x2B && g_layoutHotkey > 0 && a_scan == g_layoutHotkey);
		}

		bool BlockedByUi()
		{
			auto* ui = RE::UI::GetSingleton();
			if (!ui) {
				return true;
			}
			if (ui->GameIsPaused() || ui->IsApplicationMenuOpen() || ui->IsItemMenuOpen() || ui->IsModalMenuOpen() ||
				ui->IsMenuOpen(RE::Console::MENU_NAME) || ui->IsMenuOpen(RE::DialogueMenu::MENU_NAME) || ui->IsMenuOpen(RE::MapMenu::MENU_NAME)) {
				return true;
			}
			if (auto* cm = RE::ControlMap::GetSingleton(); cm && (cm->GetRuntimeData().textEntryCount > 0 || !cm->IsMovementControlsEnabled())) {
				return true;
			}
			return false;
		}

		// why you can't play right now ("" if you can)
		std::string CantPlay()
		{
			auto* p = Player();
			if (!p || !p->Is3DLoaded() || p->IsDead()) {
				return "-";
			}
			if (p->IsInCombat()) {
				return "Not in the middle of a fight.";
			}
			auto* st = p->AsActorState();
			if (st && st->IsWeaponDrawn()) {
				return "Put your weapon away first.";
			}
			if (p->IsOnMount()) {
				return "Get down from your horse first.";
			}
			if (st && st->IsSwimming()) {
				return "Not while swimming.";
			}
			if (st && st->GetSitSleepState() != RE::SIT_SLEEP_STATE::kNormal) {
				return "Stand up to play.";
			}
			if (auto* cell = p->GetParentCell(); cell && cell->IsInteriorCell()) {
				if (auto it = g_bans.find(cell->GetFormID()); it != g_bans.end()) {
					const float today = RE::Calendar::GetSingleton() ? RE::Calendar::GetSingleton()->GetDaysPassed() : 0.0f;
					if (today - it->second < 1.0f) {
						return "Not in here. You were thrown out earlier today.";
					}
				}
			}
			return "";
		}

		void Stop(const char* a_why);

		constexpr int kRefitFine = 600, kRefitMaster = 2000;

		// playing a lost instrument: someone notices, and its owner certainly does
		void RemarkOnInstrument(const std::vector<std::uint32_t>& a_present)
		{
			const int k = g_inst == Instrument::Lute ? 0 : 1;
			if (g_recognised || g_tierNow[k] < 3 || a_present.empty() || !g_animOn) {
				return;
			}
			const char* owner = k == 0 ? "Inge Six Fingers" : "Pantea Ateia";
			const bool  returned = k == 0 ? g_college.lute : g_college.flute;
			for (auto id : a_present) {
				if (NameOf(id) == owner) {
					g_recognised = true;
					const std::string line = k == 0 ? (returned ? "Is that... Finn's Lute? I thought you'd given it back to me!" :
					                                              "That's Finn's Lute! I'd know those courses anywhere. You'll bring that back to me, you hear?") :
					                                  (returned ? "My flute? How are you playing my flute?" :
					                                              "That's my flute! Where did you find it? I want it back!");
					React(id, 1);
					Say(fmt::format("{}: \"{}\"", NameOf(id), line), kGoldCol);
					Hud(fmt::format("{}: \"{}\"", NameOf(id), line));
					return;
				}
			}
			if (std::uniform_int_distribution<int>(0, 39)(g_rng) == 0) {  // now and then, someone else
				g_recognised = true;
				const auto id = a_present[std::uniform_int_distribution<std::size_t>(0, a_present.size() - 1)(g_rng)];
				Say(fmt::format("{}: \"{}\"", NameOf(id), k == 0 ? "An eight-course lute? That's an old one. A fine old one." : "What a sweet flute that is."), kGreen);
			}
		}

		bool AtTheCollege()
		{
			auto* p = Player();
			auto* cell = p ? p->GetParentCell() : nullptr;
			const char* cn = cell ? cell->GetFullName() : nullptr;
			auto*       loc = p ? p->GetCurrentLocation() : nullptr;
			const char* ln = loc ? loc->GetFullName() : nullptr;
			return Lower(cn).find("bards college") != std::string::npos || Lower(ln).find("bards college") != std::string::npos;
		}

		// a town's own song of yours: the one played well there most, at least kSignatureAt times ("" if none yet)
		std::string SignatureIn(RE::FormID a_town)
		{
			auto it = g_signature.find(a_town);
			if (!a_town || it == g_signature.end()) {
				return {};
			}
			const std::string* best = nullptr;
			int                most = kSignatureAt - 1;
			for (const auto& [title, n] : it->second) {
				if (n > most) {
					most = n;
					best = &title;
				}
			}
			return best ? *best : std::string{};
		}

		// a song the room knows as yours: cheers, and a warmer room
		void CheerSignature()
		{
			if (!g_song.title.empty() && SignatureIn(g_town) == g_song.title && !g_present.empty()) {
				// the room knows this one
				g_crowd.Lift(0.12);
				static const std::vector<std::string> kKnown = { "Ah, this one!", "That's the song!", "Here it comes!", "I know this one!",
					"My favourite!", "Hah! Play it loud!", "This is the one I told you about." };
				std::vector<RE::FormID> who = g_present;
				std::shuffle(who.begin(), who.end(), g_rng);
				std::vector<music::CrowdEvent> ev;
				for (std::size_t i = 0; i < who.size() && i < 3; ++i) {
					music::CrowdEvent e{ music::CrowdEvent::Kind::Cheer };
					e.id = who[i];
					e.line = PickOf(kKnown);
					ev.push_back(e);
				}
				HandleCrowd(ev);
				Say(fmt::format("{} knows this one as your song.", g_townName), kGoldCol);
			}
		}

		// ---- Bard ranks: earned as a member of the College, from its notices, the towns that admire you, the songs
		// that are yours somewhere and the nights you've played
		struct Rank
		{
			int         level;     // 0 not of the College, 1 Apprentice .. 4 Master Bard
			int         score;
			int         next;      // the score the next rank needs (0 at the top)
			const char* name;
		};
		int g_rankSeen = 0;  // the highest rank you've been told about (saved)

		Rank RankNow()
		{
			int admired = 0, signatures = 0;
			for (const auto& [town, f] : g_fame) {
				admired += FameIn(town) >= 0.35f ? 1 : 0;
			}
			for (const auto& [town, songs] : g_signature) {
				signatures += SignatureIn(town).empty() ? 0 : 1;
			}
			const int score = g_collegeShows * 2 + admired * 3 + signatures * 2 + (std::min)(g_performances / 10, 5);
			if (!g_college.member) {
				return { 0, score, 0, "Busker" };
			}
			if (score >= 30 && g_collegeShows >= 5) {
				return { 4, score, 0, "Master Bard" };
			}
			if (score >= 18) {
				return { 3, score, 30, "Bard" };
			}
			if (score >= 8) {
				return { 2, score, 18, "Journeyman" };
			}
			return { 1, score, 8, "Apprentice" };
		}

		// the room's temper: your settings, your name in this town, and how fine your instrument is
		void ApplyCrowdSettings()
		{
			const auto&            c = Config::Get();
			const float            f = FameIn(g_town);
			const int              tier = Tier(g_inst);
			music::Crowd::Settings cs;
			cs.tipRate = c.tipRate * (1.0f + 0.4f * f);
			const float college = (g_college.member ? 0.10f : 0.0f) + 0.03f * g_college.Returned();
			cs.tipSize = c.tipSize * (1.0f + 0.3f * f) * (1.0f + 0.15f * tier) * (1.0f + college);
			cs.maxPerListener = c.maxTipPerListener;
			cs.patience = c.patience * (1.0f - 0.3f * f) * (1.0f - 0.1f * tier);
			cs.welcome = 0.15f * f + 0.03f * tier + (g_college.member ? 0.04f : 0.0f);
			cs.tipSize *= 1.0f + 0.01f * g_collegeShows;  // word from the College goes before you
			{
				const int rank = RankNow().level;
				cs.tipSize *= 1.0f + 0.05f * (std::max)(0, rank - 1);  // a ranked bard is paid like one
				cs.maxPerListener = static_cast<int>(cs.maxPerListener * (1.0f + 0.15f * (std::max)(0, rank - 1)));
			}
			if (g_atCollege) {
				cs.tipSize *= 0.6f;    // students have little to give
				cs.patience *= 1.25f;  // and the hall knows good playing from bad
			}
			cs.oneTipEach = true;  // and once a day: see g_tipped
			g_crowd.SetSettings(cs);
			g_tierNow[0] = Tier(Instrument::Lute);
			g_tierNow[1] = Tier(Instrument::Flute);
			if (auto* syn = Audio::Synth()) {
				syn->SetQuality(Instrument::Lute, g_tierNow[0]);
				syn->SetQuality(Instrument::Flute, g_tierNow[1]);
			}
		}

		// what the College makes of a performance in its own hall
		void EndCollegeShow()
		{
			if (!g_atCollege || g_heardFor < 60.0) {
				return;
			}
			const double avg = g_moodSum / g_heardFor;
			std::string  judge = "Viarmo";
			for (auto id : g_present) {
				if (TeacherOf(NameOf(id)) && !IsStudent(NameOf(id))) {
					judge = NameOf(id);
					break;
				}
			}
			if (avg >= 0.7) {
				if (g_collegeShows < 10) {
					++g_collegeShows;
				}
				Hud(fmt::format("{}: \"The College takes note of you. Word of this will travel.\"", judge));
				Say(fmt::format("The College takes note ({} performance{} so far): rooms everywhere tip a little more.", g_collegeShows, g_collegeShows == 1 ? "" : "s"),
					kGoldCol);
			} else if (avg < 0.35) {
				Hud(fmt::format("{}: \"Practise more before you play in these halls again.\"", judge));
			}
			spdlog::info("College show: room {:.2f}, noted {}", avg, g_collegeShows);
		}

		// a letter from Viarmo when you rise
		void CheckRank()
		{
			const Rank r = RankNow();
			if (r.level > g_rankSeen) {
				g_rankSeen = r.level;
				if (r.level >= 2) {
					Hud(fmt::format("A letter from Viarmo: \"The College names you {}. Play like it.\"", r.name));
					Say(fmt::format("The Bards College names you {}.", r.name), kGoldCol);
					spdlog::info("Rank: {} (score {})", r.name, r.score);
				}
			}
		}

		// what the town makes of this performance
		void EndFame()
		{
			if (!g_town || (g_heardFor < 20.0 && !g_thrown)) {
				return;
			}
			const float  before = FameIn(g_town);
			const double avg = g_heardFor > 0.0 ? g_moodSum / g_heardFor : 0.5;
			const double weight = std::clamp(g_heardFor / 600.0, 0.1, 1.0);  // ten listener-minutes counts in full
			const double delta = (avg - 0.5) * 0.5 * weight - (g_thrown ? 0.25 : 0.0);
			auto&        fame = g_fame[g_town];
			fame.value = static_cast<float>(std::clamp(fame.value + delta, -1.0, 1.0));
			spdlog::info("Fame in {}: {:.2f} -> {:.2f} (room {:.2f} over {:.0f} listener-s{})", g_townName, before, fame.value, avg, g_heardFor,
				g_thrown ? ", thrown out" : "");
			if (std::string_view(FameWord(before)) != FameWord(fame.value)) {
				Hud(fmt::format("Word gets around {}: you're {} here now.", g_townName, Lower(FameWord(fame.value))));
			}
		}

		void Begin()
		{
			if (BlockedByUi()) {
				auto*      cm = RE::ControlMap::GetSingleton();
				const bool controls = cm && (cm->GetRuntimeData().textEntryCount > 0 || !cm->IsMovementControlsEnabled());
				spdlog::info("Hotkey: not now ({})", controls ? "the game has your movement controls switched off, or a text box is open" :
				                                                 "a menu is open or the game is paused");
				if (controls) {
					Hud("You can't take out an instrument right now.");
				}
				return;
			}
			const auto why = CantPlay();
			if (!why.empty()) {
				spdlog::info("Hotkey: not now ({})", why == "-" ? "the player isn't ready" : why);
				if (why != "-") {
					Hud(why);
				}
				return;
			}
			auto* p0 = Player();
			// your instrument: the last one you played, if you still have it
			RefreshOwned();
			const bool lute = g_owns[0], flute = g_owns[1];
			if (!lute && !flute) {
				Hud("You need a lute or a flute to play.");
				spdlog::info("No lute or flute recognised in your pack:");
				LogInstrumentLikeItems();
				return;
			}
			SetInst((g_lastInst == Instrument::Lute ? lute : !flute) ? Instrument::Lute : Instrument::Flute);
			if (!Audio::Start()) {
				Hud("SkyrimSongs: no sound device found.");
				return;
			}
			Audio::SetVolume(Config::Get().volume);
			Audio::SetReverb(Config::Get().reverb);
			if (auto* cam = RE::PlayerCamera::GetSingleton(); cam && Config::Get().thirdPerson && cam->IsInFirstPerson()) {
				cam->ForceThirdPerson();
				g_forcedThird = true;
			}
			// both hands are on the instrument: a torch or shield in the left hand goes away (as in Skyrim's Got Talent)
			if (auto* left = p0->GetEquippedObject(true); left && (left->Is(RE::FormType::Light) || left->Is(RE::FormType::Armor))) {
				if (auto* bound = left->As<RE::TESBoundObject>()) {
					RE::ActorEquipManager::GetSingleton()->UnequipObject(p0, bound);
				}
			}
			g_animOn = false;  // the instrument comes out with the first note
			g_want = -1;
			if (g_firstPersonAt > 0.0) {
				g_firstPersonAt = -1.0;  // starting again before the camera went back: it goes back after this time instead
				g_forcedThird = true;
			}
			g_lastNoteAt = Now();
			MuteMusic(true);
			g_midiCount = MidiIn::Open(&OnMidi);
			g_midiNames = MidiIn::OpenNames();
			g_playing = true;
			g_act = Act::Free;
			g_octave = 0;
			g_shiftL = g_shiftR = false;
			g_keyPitch.clear();
			g_lit.clear();
			{
				std::lock_guard mk(g_midiMtx);
				g_midiQ.clear();
			}
			g_free.Reset();
			g_crowd = music::Crowd(static_cast<std::uint32_t>(g_rng()));
			{
				auto* town = TownOf(p0);
				g_town = town ? town->GetFormID() : 0;
				const char* name = town ? town->GetFullName() : nullptr;
				g_townName = name && *name ? name : "these parts";
			}
			g_moodSum = g_heardFor = 0.0;
			g_thrown = false;
			g_college = CollegeStanding();
			g_atCollege = AtTheCollege();
			g_recognised = false;
			CheckRank();
			ApplyCrowdSettings();
			if (g_atCollege) {
				Say("You play in the College's own hall. The students will listen; the teachers will judge.", kDim);
			}
			g_names.clear();
			g_feed.clear();
			g_earned = 0;
			g_badFor = 0.0;
			g_warned = false;
			g_cell = Player()->GetParentCell();
			g_anchor = Player()->GetPosition();
			g_anchorHeading = Player()->GetAngleZ();
			g_quality = 0.5;
			++g_performances;
			ScanListeners();
			Say(fmt::format("You take out your {}.{}", InstName(g_inst),
					g_midiCount ? fmt::format(" (MIDI: {})", g_midiNames.empty() ? "connected" : g_midiNames.front()) : ""),
				kInk);
			Overlay::SetMode(Overlay::Mode::Playing);
			spdlog::info("Playing the {}{}: {} listening{}, MIDI devices {}, in {} ({:08X}, fame {:.2f})", TierWord(Tier(g_inst)), InstName(g_inst), g_crowd.Size(),
				g_inInn ? " (an inn)" : "", g_midiCount, g_townName, g_town, FameIn(g_town));
		}

		void Stop(const char* a_why)
		{
			if (!g_playing) {
				return;
			}
			g_playing = false;
			g_midiLive = false;
			EndCollegeShow();
			EndFame();
			CheckRank();
			SendAllAway();
			for (auto id : g_lookers) {
				Call(ActorOf(id), "ClearLookAt");
			}
			g_lookers.clear();
			g_nextBardHush = 0.0;
			ReleaseAllKeys();
			Audio::StopSong();
			MidiIn::Close();
			g_act = Act::Free;
			PutAway();
			const bool wasOut = g_held != -1 || false;
			AnimTick(Now());  // the put-away starts now
			if (g_forcedThird) {
				// back to first person only once the put-away has played: switching at once cut it short and left
				// the instrument in your hands
				g_firstPersonAt = Now() + (wasOut ? 2.0 : 0.0);
				g_forcedThird = false;
			}
			MuteMusic(false);
			g_lastInst = g_inst;
			Overlay::SetMode(Overlay::Mode::Closed);
			if (g_earned > 0) {
				Hud(fmt::format("You earned {} septims playing the {}.", g_earned, InstName(g_inst)));
			}
			spdlog::info("Stopped playing ({}), earned {}", a_why, g_earned);
		}

		// The innkeeper has had enough: out the nearest door that leads outside, and not back today.
		void ThrowOut()
		{
			auto* p = Player();
			auto* cell = p ? p->GetParentCell() : nullptr;
			const std::string keeper = g_keeper ? NameOf(g_keeper) : "The innkeeper";
			Hud(fmt::format("{}: \"That's enough! Out, and take that racket with you!\"", keeper));
			if (cell) {
				g_bans[cell->GetFormID()] = RE::Calendar::GetSingleton() ? RE::Calendar::GetSingleton()->GetDaysPassed() : 0.0f;
			}
			RE::TESObjectREFR* dest = nullptr;
			RE::NiPoint3       destPos;
			float              best = std::numeric_limits<float>::max();
			if (cell) {
				const auto here = p->GetPosition();
				cell->ForEachReference([&](RE::TESObjectREFR* a_ref) {
					if (!a_ref || a_ref->IsDisabled()) {
						return RE::BSContainer::ForEachResult::kContinue;
					}
					auto* base = a_ref->GetBaseObject();
					if (!base || !base->Is(RE::FormType::Door)) {
						return RE::BSContainer::ForEachResult::kContinue;
					}
					auto* tp = a_ref->extraList.GetByType<RE::ExtraTeleport>();
					if (!tp || !tp->teleportData) {
						return RE::BSContainer::ForEachResult::kContinue;
					}
					auto linked = tp->teleportData->linkedDoor.get();
					if (!linked) {
						return RE::BSContainer::ForEachResult::kContinue;
					}
					auto*       lc = linked->GetParentCell();
					const float d = a_ref->GetPosition().GetDistance(here) + ((lc && lc->IsExteriorCell()) ? 0.0f : 1.0e6f);
					if (d < best) {
						best = d;
						dest = linked.get();
						destPos = tp->teleportData->position;
					}
					return RE::BSContainer::ForEachResult::kContinue;
				});
			}
			g_thrown = true;
			Stop("thrown out");
			if (dest) {
				const auto at = dest->GetPosition();
				Call(p, "MoveTo", dest, destPos.x - at.x, destPos.y - at.y, destPos.z - at.z, false);
				spdlog::info("Thrown out through {:08X}", dest->GetFormID());
			} else {
				spdlog::info("Thrown out: no door out found, just stopped");
			}
		}

		// ------------------------------------------------------------------------------------------ the frame
		void Tick()
		{
			static double s_prev = Now();
			const double  now = Now();
			const double  dt = std::clamp(now - s_prev, 0.0, 0.1);
			s_prev = now;

			if (g_wantStart.exchange(false) && !g_playing) {
				Begin();
				return;
			}
			if (!g_playing) {
				// after you stop: the put-away finishes (and is checked), then the camera goes back
				AnimTick(now);
				if (g_firstPersonAt > 0.0 && now >= g_firstPersonAt) {
					g_firstPersonAt = -1.0;
					if (auto* cam = RE::PlayerCamera::GetSingleton()) {
						cam->ForceFirstPerson();
					}
				}
				return;
			}
			auto* p = Player();
			auto* ui = RE::UI::GetSingleton();
			if (!p || p->IsDead()) {
				Stop("dead");
				return;
			}
			if (p->IsInCombat()) {
				Stop("combat");
				Hud(fmt::format("You put your {} away to fight.", InstName(g_inst)));
				return;
			}
			if (auto* st = p->AsActorState(); st && st->IsWeaponDrawn()) {
				Stop("weapon drawn");
				return;
			}
			if (p->GetParentCell() != g_cell) {
				Stop("left the area");
				return;
			}
			if (ui && (ui->IsMenuOpen(RE::DialogueMenu::MENU_NAME) || ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME))) {
				Stop("a menu opened");
				return;
			}
			if (g_act == Act::Auto && !Audio::SongPaused()) {
				g_lastNoteAt = now;
			} else if (g_animOn && now - g_lastNoteAt > 8.0) {
				PutAway();  // a while of quiet: the instrument goes down (the next note brings it up again)
			}
			AnimTick(now);
			g_midiLive = Overlay::GetMode() == Overlay::Mode::Playing && g_act != Act::Auto;
			{
				std::vector<MidiHit> hits;
				{
					std::lock_guard mk(g_midiMtx);
					hits.swap(g_midiQ);
				}
				for (const auto& h : hits) {
					Register(h.pitch, h.now, h.songT);
				}
			}
			static double s_scan = 0.0;
			if (now - s_scan > 0.5) {
				s_scan = now;
				ScanListeners();
				HushMusic();
			}

			const bool book = Overlay::GetMode() == Overlay::Mode::Book;
			bool       performing = !book;
			switch (g_act) {
			case Act::Free:
				g_breakdown = g_free.Judge(now);
				g_quality = g_breakdown.quality;
				// a long silence isn't a bad performance, it's no performance: the room drifts back to neutral
				performing = performing && !g_free.Silent(now);
				break;
			case Act::Along:
				{
					const double t = Audio::SongTime();
					// the count-in: three soft ticks
					for (double beat = -3.0; beat < 0.0; beat += 1.0) {
						if (g_lastClick < beat && t >= beat) {
							g_lastClick = beat;
							Audio::Click();
						}
					}
					performing = performing && t >= 0.0;
					g_chart.Advance(t - Config::Get().latencyMs / 1000.0);
					g_quality = g_chart.Quality();
					if (g_chart.Done(t)) {
						const double score = g_chart.Score();
						Say(fmt::format("{}: {:.0f}%  ({} perfect, {} good, {} missed)", g_song.title, score * 100.0, g_chart.Perfects(), g_chart.Goods(),
								g_chart.Misses()),
							score >= 0.6 ? kGreen : kRed);
						std::vector<music::CrowdEvent> ev;
						g_crowd.SongEnded(score, Speech01(), ev);
						HandleCrowd(ev);
						if (Config::Get().speechXp > 0.0f) {
							p->AddSkillExperience(RE::ActorValue::kSpeech, static_cast<float>(score * 20.0 * Config::Get().speechXp));
						}
						spdlog::info("Song over: {} {:.2f}", g_song.title, score);
						BackToFree();
					}
				}
				break;
			case Act::Auto:
				{
					g_quality = 0.92;
					performing = performing && !Audio::SongPaused();  // a pause is a breath between, not a bad bit
					if (Audio::SongTime() > g_song.length + 1.5) {
						// played well to a room here: a step toward it becoming your song in this town
						if (g_town && g_crowd.Size() > 0 && g_crowd.Mood() >= 0.5 && !g_song.title.empty()) {
							const std::string before = SignatureIn(g_town);
							++g_signature[g_town][g_song.title];
							if (SignatureIn(g_town) == g_song.title && before != g_song.title) {
								Hud(fmt::format("{} knows \"{}\" as your song now.", g_townName, g_song.title));
								Say(fmt::format("{} knows \"{}\" as your song now.", g_townName, g_song.title), kGoldCol);
							}
						}
						std::vector<music::CrowdEvent> ev;
						g_crowd.SongEnded(0.9, Speech01(), ev);
						HandleCrowd(ev);
						BackToFree();
					}
				}
				break;
			}
			std::vector<music::CrowdEvent> ev;
			g_crowd.Step(dt, g_quality, Speech01(), performing, ev);
			HandleCrowd(ev);
			if (performing && g_crowd.Size() > 0) {
				const double w = dt * (std::min)(5.0, static_cast<double>(g_crowd.Size()));
				g_moodSum += g_crowd.Mood() * w;
				g_heardFor += w;
			}

			// an inn that's turned on you
			if (g_inInn && Config::Get().throwOut && g_crowd.Size() > 0 && performing) {
				const double mood = g_crowd.Mood();
				if (mood < 0.18) {
					g_badFor += dt;
				} else if (mood > 0.25) {
					g_badFor = 0.0;
					g_warned = false;
				}
				if (g_badFor > 4.0 && !g_warned) {
					g_warned = true;
					const std::string keeper = g_keeper ? NameOf(g_keeper) : "The innkeeper";
					Say(fmt::format("{}: \"Play something decent or get out!\"", keeper), kRed);
					Hud(fmt::format("{}: \"Play something decent or get out!\"", keeper));
				}
				if (g_badFor > (g_college.member ? 16.0 : 11.0)) {  // a College bard gets a little longer
					ThrowOut();
				}
			}
		}

		void SafeTick()
		{
			__try {
				Tick();
			} __except (Overlay::SehFilter(GetExceptionInformation(), "Performance tick")) {
			}
		}

		// ------------------------------------------------------------------------------------------ the look
		float g_ui = 1.0f;
		struct Fonts
		{
			ImFont* body = nullptr;
			ImFont* sm = nullptr;
			ImFont* key = nullptr;
			ImFont* title = nullptr;
			ImFont* book = nullptr;
			ImFont* bookIt = nullptr;
			ImFont* big = nullptr;
		} F;

		void Text(ImDrawList* a_dl, ImFont* a_font, ImVec2 a_pos, ImU32 a_col, const std::string& a_text, float a_alignX = 0.0f)
		{
			const auto size = a_font->CalcTextSizeA(a_font->FontSize, FLT_MAX, 0.0f, a_text.c_str());
			a_pos.x -= size.x * a_alignX;
			a_dl->AddText(a_font, a_font->FontSize, a_pos + ImVec2(1, 1), IM_COL32(0, 0, 0, 160), a_text.c_str());
			a_dl->AddText(a_font, a_font->FontSize, a_pos, a_col, a_text.c_str());
		}

		ImU32 Fade(ImU32 a_col, float a_alpha)
		{
			const auto a = static_cast<ImU32>(((a_col >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(a_alpha, 0.0f, 1.0f));
			return (a_col & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
		}

		void DrawHud()
		{
			float sw = 0.0f, sh = 0.0f;
			Overlay::ScreenSize(sw, sh);
			const float s = g_ui;
			auto*       dl = ImGui::GetBackgroundDrawList();
			const double now = Now();
			const double songT = g_act == Act::Free ? 0.0 : Audio::SongTime();
			const auto&  cfg = Config::Get();

			const float W = (std::min)(980.0f * s, sw * 0.92f);
			const bool  along = g_act == Act::Along;
			const float headH = 34.0f * s, laneH = 30.0f * s, hwH = along ? laneH * 3.0f + 14.0f * s : 26.0f * s;
			const float kbH = !cfg.showKeyboard ? 0.0f : (g_inst == music::Instrument::Flute ? 170.0f : 190.0f) * s;  // the instrument
			const float hintH = 24.0f * s;
			const float H = headH + hwH + kbH + hintH + 18.0f * s;
			const ImVec2 p0((sw - W) * 0.5f, sh - H - 26.0f * s), p1(p0.x + W, p0.y + H);
			dl->AddRectFilled(p0, p1, IM_COL32(16, 12, 9, 200), 8.0f * s);
			dl->AddRect(p0, p1, IM_COL32(200, 160, 90, 110), 8.0f * s, 0, 1.5f * s);

			// the header: instrument and what you're doing, the room, your earnings
			float y = p0.y + 8.0f * s;
			std::string what = g_act == Act::Free ? "Free play" : g_act == Act::Along ? "Playing along: " + g_song.title :
			                   (Audio::SongPaused() ? "Paused: " : "Playing: ") + g_song.title;
			{
				std::string inst = InstLabel(g_inst, g_tierNow[g_inst == Instrument::Lute ? 0 : 1]);
				inst[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(inst[0])));
				const std::string head = fmt::format("{}  ·  {}", inst, what);
				Text(dl, F.book, { p0.x + 16.0f * s, y }, kGoldCol, head);
				if (g_town) {
					const float       x = p0.x + 16.0f * s + F.book->CalcTextSizeA(F.book->FontSize, FLT_MAX, 0.0f, head.c_str()).x + 14.0f * s;
					const std::string fame = fmt::format("{}: {}", g_townName, FameWord(FameIn(g_town)));
					if (x + F.sm->CalcTextSizeA(F.sm->FontSize, FLT_MAX, 0.0f, fame.c_str()).x < p0.x + W * 0.58f) {  // clear of the mood bar
						Text(dl, F.sm, { x, y + 3.0f * s }, kDim, fame);
					}
				}
			}
			{
				const double mood = g_crowd.Mood();
				const float  bw = 170.0f * s, bh = 8.0f * s;
				const ImVec2 b0(p0.x + W * 0.60f, y + 11.0f * s);
				std::string  label = g_crowd.Size() == 0 ? "Nobody's listening" :
				                                           fmt::format("{} · {} listening", music::Crowd::MoodWord(mood), g_crowd.Size());
				Text(dl, F.sm, { b0.x + bw * 0.5f, y - 8.0f * s }, kDim, label, 0.5f);
				dl->AddRectFilled(b0, b0 + ImVec2(bw, bh), IM_COL32(60, 50, 40, 220), 3.0f * s);
				if (g_crowd.Size() > 0) {
					const ImU32 c = mood < 0.3 ? kRed : mood < 0.55 ? IM_COL32(220, 200, 140, 255) : kGreen;
					dl->AddRectFilled(b0, b0 + ImVec2(bw * static_cast<float>(mood), bh), c, 3.0f * s);
				}
			}
			Text(dl, F.body, { p1.x - 16.0f * s, y + 2.0f * s }, kGoldCol, fmt::format("{} septims", g_earned), 1.0f);
			y += headH;

			// the sheet music: the notes slide to the line; the lanes are the keyboard's rows (top row highest)
			if (along) {
				const float  x0 = p0.x + 16.0f * s, x1 = p1.x - 16.0f * s, hitX = x0 + 90.0f * s;
				const double ahead = 3.2 / (std::max)(0.5f, cfg.speed);
				const float  pps = static_cast<float>((x1 - hitX) / ahead);
				dl->AddRectFilled({ x0, y }, { x1, y + laneH * 3.0f + 4.0f * s }, IM_COL32(0, 0, 0, 90), 4.0f * s);
				for (int r = 1; r < 3; ++r) {
					dl->AddLine({ x0, y + laneH * r }, { x1, y + laneH * r }, IM_COL32(255, 255, 255, 18));
				}
				dl->PushClipRect({ x0, y }, { x1, y + laneH * 3.0f + 4.0f * s }, true);
				const auto& chart = g_chart.Chart();
				const int   base = Base();
				for (std::size_t i = 0; i < chart.size(); ++i) {
					const auto& n = chart[i];
					const float xs = hitX + static_cast<float>((n.start - songT) * pps);
					if (xs > x1 + 10.0f) {
						break;
					}
					const float xe = (std::max)(xs + 26.0f * s, hitX + static_cast<float>((n.start + n.dur - songT) * pps));
					if (xe < x0) {
						continue;
					}
					music::keymap::Label l;
					const bool           ok = music::keymap::LabelFor(n.pitch, base, l);
					const int            lane = ok ? 2 - l.row : 1;
					const float          ly = y + lane * laneH + 3.0f * s;
					const auto           st = g_chart.StateOf(i);
					ImU32 fill = IM_COL32(120, 95, 60, 230), text = kInk;
					if (st == music::ChartJudge::State::Perfect) {
						fill = IM_COL32(230, 190, 105, 120);
					} else if (st == music::ChartJudge::State::Good) {
						fill = IM_COL32(120, 190, 110, 120);
					} else if (st == music::ChartJudge::State::Missed) {
						fill = IM_COL32(170, 70, 60, 110);
						text = kDim;
					}
					dl->AddRectFilled({ xs, ly }, { xe, ly + laneH - 6.0f * s }, fill, 5.0f * s);
					const std::string lab = ok ? fmt::format("{}{}", l.letter, l.shift ? "+" : "") : music::NoteName(n.pitch);
					Text(dl, F.key, { xs + 6.0f * s, ly + 1.0f * s }, text, lab);
				}
				dl->PopClipRect();
				dl->AddLine({ hitX, y - 2.0f * s }, { hitX, y + laneH * 3.0f + 6.0f * s }, kGoldCol, 2.5f * s);
				if (songT < 0.0) {
					Text(dl, F.big, { (x0 + x1) * 0.5f, y + laneH * 0.5f }, kGoldCol, fmt::format("{}", static_cast<int>(std::ceil(-songT))), 0.5f);
				}
				Text(dl, F.sm, { x1 - 6.0f * s, y + 2.0f * s }, kDim,
					fmt::format("{:.0f}%   streak {}", g_chart.Score() * 100.0, g_chart.Streak()), 1.0f);
				y += hwH;
			} else if (g_act == Act::Free) {
				// what the room hears in your playing
				const auto& b = g_breakdown;
				const bool  silent = g_free.Silent(now);
				const std::pair<const char*, double> parts[] = { { "Rhythm", b.rhythm }, { "Melody", b.melody }, { "Key", b.key },
					{ "Phrasing", b.stability }, { "Harmony", b.harmony }, { "Ideas", b.ideas } };
				const float colW = (W - 32.0f * s) / 6.0f;
				for (int i = 0; i < 6; ++i) {
					const float  cx = p0.x + 16.0f * s + colW * i;
					const double v = silent ? 0.0 : std::clamp(parts[i].second, 0.0, 1.0);
					Text(dl, F.sm, { cx, y + 2.0f * s }, kDim, parts[i].first);
					const ImVec2 b0(cx + 62.0f * s, y + 10.0f * s), b1(cx + colW - 14.0f * s, y + 15.0f * s);
					dl->AddRectFilled(b0, b1, IM_COL32(60, 50, 40, 220), 2.0f * s);
					const ImU32 c = v < 0.35 ? kRed : v < 0.6 ? IM_COL32(220, 200, 140, 255) : kGreen;
					dl->AddRectFilled(b0, { b0.x + (b1.x - b0.x) * static_cast<float>(v), b1.y }, c, 2.0f * s);
				}
				if (b.spam < 1.0 && !silent) {
					Text(dl, F.sm, { p1.x - 16.0f * s, y - 16.0f * s }, kRed, "sounds like noise", 1.0f);
				}
				y += hwH;
			} else if (g_act == Act::Auto) {
				const float  x0 = p0.x + 16.0f * s, x1 = p1.x - 16.0f * s;
				const float  f = static_cast<float>(std::clamp(songT / (std::max)(1.0, g_song.length), 0.0, 1.0));
				dl->AddRectFilled({ x0, y + 6.0f * s }, { x1, y + 12.0f * s }, IM_COL32(60, 50, 40, 220), 3.0f * s);
				dl->AddRectFilled({ x0, y + 6.0f * s }, { x0 + (x1 - x0) * f, y + 12.0f * s }, kGoldCol, 3.0f * s);
				y += hwH;
			}

			// the instrument itself, lit as its notes sound: a lute's neck, or a flute's tone holes
			if (cfg.showKeyboard) {
				// what's sounding now, and for how long: your notes, and in "let it play" the song's
				std::vector<InstrumentHud::Sounding> sounding;
				for (const auto& [pitch, t] : g_lit) {
					if (now - t < 0.6) {
						sounding.push_back({ pitch, static_cast<float>(now - t) });
					}
				}
				if (g_act == Act::Auto) {
					for (const auto& n : g_song.all) {
						if (n.start > songT) {
							break;
						}
						if (songT - n.start < 0.6) {
							sounding.push_back({ n.pitch, static_cast<float>(songT - n.start) });
						}
					}
				}
				const InstrumentHud::Fonts fonts{ F.key, F.sm, F.bookIt };
				const ImVec2               ip(p0.x + 16.0f * s, y), isz(W - 32.0f * s, kbH);
				if (g_inst == music::Instrument::Flute) {
					InstrumentHud::DrawFlute(dl, ip, isz, s, Base(), sounding, now, fonts);
				} else {
					InstrumentHud::DrawLute(dl, ip, isz, s, Base(), sounding, now, fonts);
				}
				y += kbH;
			}

			// the keys' help
			const std::string octave = g_octave == 0 ? "" : fmt::format("   (octave {:+d})", g_octave);
			Text(dl, F.sm, { p0.x + W * 0.5f, y + 2.0f * s }, kDim,
				g_act == Act::Auto ?
					fmt::format("P {}  ·  Tab songbook  ·  Backspace hide  ·  Esc stop the song", Audio::SongPaused() ? "carry on" : "pause") :
					fmt::format("Z-M  A-J  Q-I play  ·  Shift = black keys (S+Z = C#)  ·  Up/Down octave  ·  Tab songbook  ·  Backspace hide  ·  Esc {}{}",
						g_act == Act::Free ? "put away" : "stop the song", octave),
				0.5f);

			// what the room says
			float fy = p0.y - 10.0f * s;
			for (auto it = g_feed.rbegin(); it != g_feed.rend(); ++it) {
				const double age = now - it->t;
				if (age > 9.0) {
					continue;
				}
				const float a = static_cast<float>(age < 7.0 ? 1.0 : 1.0 - (age - 7.0) / 2.0);
				fy -= F.body->FontSize + 4.0f * s;
				Text(dl, F.body, { p0.x + 8.0f * s, fy }, Fade(it->color, a), it->text);
			}
		}

		void SongRow(const Entry& a_e, int a_id)
		{
			const float s = g_ui;
			ImGui::PushID(a_id);
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			{
				// a favourite: a star to click
				const bool   fav = g_favourites.contains(a_e.song.title);
				const float  r = 8.0f * s;
				const ImVec2 at = ImGui::GetCursorScreenPos() + ImVec2(r + 2.0f * s, r + 4.0f * s);
				if (ImGui::InvisibleButton("##fav", ImVec2(2.0f * r + 4.0f * s, 2.0f * r + 8.0f * s))) {
					if (fav) {
						g_favourites.erase(a_e.song.title);
					} else {
						g_favourites.insert(a_e.song.title);
					}
				}
				const bool hov = ImGui::IsItemHovered();
				ImVec2     pts[10];
				for (int i = 0; i < 10; ++i) {
					const float a = -1.5707963f + i * 0.6283185f, rr = i % 2 ? r * 0.45f : r;
					pts[i] = at + ImVec2(std::cos(a) * rr, std::sin(a) * rr);
				}
				auto* dl = ImGui::GetWindowDrawList();
				if (fav) {
					dl->AddConcavePolyFilled(pts, 10, kGoldCol);
				}
				dl->AddPolyline(pts, 10, fav ? kGoldCol : (hov ? IM_COL32(230, 200, 140, 220) : IM_COL32(160, 140, 110, 150)), ImDrawFlags_Closed, 1.3f * s);
				if (hov) {
					ImGui::SetTooltip(fav ? "A favourite (click to unmark)" : "Mark as a favourite: favourites come first");
				}
			}
			ImGui::TableNextColumn();
			ImGui::PushFont(F.book);
			ImGui::TextUnformatted(a_e.song.title.c_str());
			ImGui::PopFont();
			if (g_town && SignatureIn(g_town) == a_e.song.title) {
				ImGui::SameLine();
				ImGui::TextColored(ImVec4(0.9f, 0.75f, 0.45f, 1.0f), " - your song in %s", g_townName.c_str());
			} else if (g_town) {
				if (auto t = g_signature.find(g_town); t != g_signature.end()) {
					if (auto it = t->second.find(a_e.song.title); it != t->second.end() && it->second > 0 && ImGui::IsItemHovered()) {
						ImGui::SetTooltip("Gone down well here %d time%s (%d makes it your song in %s)", it->second, it->second == 1 ? "" : "s", kSignatureAt,
							g_townName.c_str());
					}
				}
			}
			ImGui::TableNextColumn();
			if (!a_e.error.empty()) {
				ImGui::TextDisabled("%s", a_e.error.c_str());
				ImGui::TableNextColumn();
				ImGui::TableNextColumn();
				ImGui::TableNextColumn();
				ImGui::PopID();
				return;
			}
			const int secs = static_cast<int>(a_e.song.length);
			ImGui::TextDisabled("%d:%02d", secs / 60, secs % 60);
			ImGui::TableNextColumn();
			if (a_e.song.difficulty > 0) {
				std::string stars;
				for (int i = 0; i < 5; ++i) {
					stars += i < a_e.song.difficulty ? "*" : ".";
				}
				ImGui::TextDisabled("%s", stars.c_str());
			} else {
				ImGui::TextDisabled("%zu", a_e.song.all.size());
			}
			ImGui::TableNextColumn();
			if (auto it = g_plays.find(a_e.song.title); it != g_plays.end() && it->second > 0) {
				ImGui::TextDisabled("%d", it->second);
			} else {
				ImGui::TextDisabled("-");
			}
			ImGui::TableNextColumn();
			if (ImGui::Button("Play", ImVec2(90.0f * s, 0))) {
				StartAuto(a_e.song);
				Overlay::SetMode(Overlay::Mode::Playing);
			}
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("The song plays itself on your instrument, every note of it, perfectly.\nThe crowd won't know.");
			}
			ImGui::PopID();
		}

		void DrawBook()
		{
			float sw = 0.0f, sh = 0.0f;
			Overlay::ScreenSize(sw, sh);
			const float s = g_ui;
			ImGui::SetNextWindowSize(ImVec2((std::min)(860.0f * s, sw * 0.9f), (std::min)(640.0f * s, sh * 0.85f)), ImGuiCond_Always);
			ImGui::SetNextWindowPos(ImVec2(sw * 0.5f, sh * 0.45f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
			ImGui::Begin("##songbook", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
			ImGui::PushFont(F.title);
			ImGui::TextColored(ImVec4(0.9f, 0.75f, 0.45f, 1.0f), "Songbook");
			ImGui::PopFont();
			ImGui::Separator();

			// the instrument
			ImGui::TextDisabled("Instrument");
			for (auto inst : { Instrument::Lute, Instrument::Flute }) {
				ImGui::SameLine();
				const bool owned = g_owns[inst == Instrument::Lute ? 0 : 1], on = g_inst == inst;
				ImGui::BeginDisabled(!owned);
				if (on) {
					ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.42f, 0.2f, 1.0f));
				}
				if (ImGui::Button(music::InstrumentName(inst), ImVec2(100.0f * s, 0)) && !on) {
					ReleaseAllKeys();
					BackToFree();
					SetInst(inst);
					g_lastInst = inst;
					// put the old instrument away properly (the put-away animation is what takes it out of the hands;
					// starting the new idle before it finished left both instruments held), then take out the new one
					if (g_animOn) {
						EnsureAnim();  // the old one goes away first, then the new one comes out (AnimTick)
					}
					ApplyCrowdSettings();
					Say(fmt::format("You put away the {} and take out the {}.", InstName(inst == Instrument::Lute ? Instrument::Flute : Instrument::Lute),
							InstName(inst)),
						kDim);
				}
				if (on) {
					ImGui::PopStyleColor();
				}
				ImGui::EndDisabled();
				if (!owned && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
					ImGui::SetTooltip("You don't have one");
				}
			}
			ImGui::SameLine(0.0f, 30.0f * s);
			if (ImGui::Button("Free play", ImVec2(110.0f * s, 0))) {
				BackToFree();
				Overlay::SetMode(Overlay::Mode::Playing);
			}
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("Play whatever you like. The room judges your rhythm, key and melody.");
			}
			// how fine it is, and your name in this town
			{
				const int t = g_tierNow[g_inst == Instrument::Lute ? 0 : 1];
				if (t >= 3) {
					ImGui::TextColored(ImVec4(0.9f, 0.75f, 0.45f, 1.0f), "You're playing %s", InstLabel(g_inst, t).c_str());
					if (ImGui::IsItemHovered()) {
						ImGui::SetTooltip("%s", g_inst == Instrument::Lute ? "Finn's own eight-course lute, the first ever made. Rooms hear the difference." :
						                                                     "Pantea's own flute. It sings like no other.");
					}
				} else {
					ImGui::TextDisabled("Your %s: %s", InstName(g_inst), t >= 2 ? "masterwork" : t == 1 ? "fine" : "common");
				}
				if (g_town) {
					ImGui::SameLine(0.0f, 30.0f * s);
					const float f = FameIn(g_town);
					ImGui::TextDisabled("%s: %s", g_townName.c_str(), FameWord(f));
					if (ImGui::IsItemHovered()) {
						ImGui::SetTooltip("Your name here. Please a room and word gets around: newcomers come warmer, tip more and forgive more.\n"
										  "Empty one (or get thrown out) and it goes the other way. It fades as the days pass.");
					}
				}
				ImGui::SameLine(0.0f, 30.0f * s);
				if (g_college.member) {
					const Rank r = RankNow();
					ImGui::TextColored(ImVec4(0.9f, 0.75f, 0.45f, 1.0f), "%s of the College", r.name);
					if (ImGui::IsItemHovered()) {
						const std::string next = r.next ? fmt::format("\n\nTowards the next rank: {} / {}{}", r.score, r.next,
															  r.level == 3 && g_collegeShows < 5 ? " (and five College notices)" : "") :
						                                  std::string("\n\nThe highest rank there is.");
						ImGui::SetTooltip("A member of the Bards College: rooms take you more seriously (warmer welcome, better tips), innkeepers give you longer\n"
										  "before they lose patience, and the College's luthier gives you the members' price. Each rank above Apprentice pays better.\n"
										  "Ranks come from the College's notices, towns that admire you, songs that are yours somewhere, and nights played.%s%s",
							g_college.Returned() > 0 ? fmt::format("\nYou brought back {} of the College's lost instruments: word of it follows you.", g_college.Returned()).c_str() : "",
							next.c_str());
					}
				} else {
					ImGui::TextDisabled("Not of the College");
					if (ImGui::IsItemHovered()) {
						ImGui::SetTooltip("Join the Bards College in Solitude (Tending the Flames) and rooms take you more seriously.");
					}
				}
				// the Bards College's luthier refits instruments
				if (AtTheCollege()) {
					const int k = g_inst == Instrument::Lute ? 0 : 1;
					if (g_refit[k] < 2 && t < 2) {
						const int  next = g_refit[k] + 1;
						const bool gift = next == 1 && (k == 0 ? g_college.lute : g_college.flute);  // thanks for bringing hers back
						const int  price = gift ? 0 : static_cast<int>((next == 1 ? kRefitFine : kRefitMaster) * (g_college.member ? 0.75 : 1.0));
						const int  have = GoldOf(Player());
						ImGui::BeginDisabled(have < price);
						const std::string label = gift ? fmt::format("{} will refit your {} (fine) for free, for bringing hers back", k == 0 ? "Inge" : "Pantea", InstName(g_inst)) :
						                                 fmt::format("Have the College refit your {} ({}): {} septims{}", InstName(g_inst), next == 1 ? "fine" : "masterwork", price,
															 g_college.member ? " (members' price)" : "");
						if (ImGui::Button(label.c_str())) {
							if (auto* p = Player(); p && g_gold) {
								if (price > 0) {
									p->RemoveItem(g_gold, price, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
								}
								g_refit[k] = next;
								ApplyCrowdSettings();
								Say(fmt::format("The College's luthier refits your {}. It's {} now.", InstName(g_inst), next == 1 ? "a fine one" : "a masterwork"), kGoldCol);
								spdlog::info("Refit the {} to tier {} for {} septims", InstName(g_inst), next, price);
							}
						}
						ImGui::EndDisabled();
						if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
							ImGui::SetTooltip("%s", have < price ? "You can't afford it." :
							                         "A finer instrument rings fuller and sweeter, and rooms tip more for it and forgive more.");
						}
					}
				}
			}
			ImGui::Spacing();

			// the songs
			const float footer = 150.0f * s;
			// search (just type) and sort
			{
				auto&  cfg = Config::Edit();
				const ImVec2 b0 = ImGui::GetCursorScreenPos();
				const float  bw = 320.0f * s, bh = ImGui::GetFrameHeight();
				auto*        dl = ImGui::GetWindowDrawList();
				dl->AddRectFilled(b0, b0 + ImVec2(bw, bh), IM_COL32(0, 0, 0, 90), 4.0f * s);
				dl->AddRect(b0, b0 + ImVec2(bw, bh), g_search.empty() ? IM_COL32(200, 160, 90, 80) : kGoldCol, 4.0f * s, 0, 1.2f * s);
				const bool blink = std::fmod(Now(), 1.0) < 0.55;
				const std::string shown = g_search.empty() ? std::string("Type to search...") : g_search + (blink ? "|" : "");
				dl->AddText(F.body, F.body->FontSize, b0 + ImVec2(8.0f * s, (bh - F.body->FontSize) * 0.5f), g_search.empty() ? IM_COL32(160, 145, 120, 160) : kInk,
					shown.c_str());
				ImGui::Dummy(ImVec2(bw, bh));
				if (!g_search.empty()) {
					ImGui::SameLine();
					if (ImGui::SmallButton("Clear")) {
						g_search.clear();
					}
				}
				ImGui::SameLine(0.0f, 30.0f * s);
				static const char* kSorts[] = { "Name", "Length", "Notes", "Most played", "Newest" };
				ImGui::SetNextItemWidth(170.0f * s);
				int sort = std::clamp(cfg.songSort, 0, 4);
				if (ImGui::Combo("Sort by", &sort, kSorts, 5)) {
					cfg.songSort = sort;
					Config::Save();
				}
			}
			std::vector<const Entry*> list;
			for (const auto& e : g_files) {
				if (Matches(e.song.title)) {
					list.push_back(&e);
				}
			}
			{
				const int sort = Config::Get().songSort;
				auto      plays = [](const Entry* a_e) { auto it = g_plays.find(a_e->song.title); return it == g_plays.end() ? 0 : it->second; };
				std::stable_sort(list.begin(), list.end(), [&](const Entry* a, const Entry* b) {
					const bool fa = g_favourites.contains(a->song.title), fb = g_favourites.contains(b->song.title);
					if (fa != fb) {
						return fa;  // favourites first
					}
					switch (sort) {
					case 1:
						return a->song.length < b->song.length;
					case 2:
						return a->song.all.size() < b->song.all.size();
					case 3:
						return plays(a) > plays(b);
					case 4:
						return a->mtime > b->mtime;
					default:
						return _stricmp(a->song.title.c_str(), b->song.title.c_str()) < 0;
					}
				});
			}
			ImGui::BeginChild("##songs", ImVec2(0, -footer), true);
			if (ImGui::BeginTable("##list", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
				ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 26.0f * s);
				ImGui::TableSetupColumn("Song", ImGuiTableColumnFlags_WidthStretch, 3.0f);
				ImGui::TableSetupColumn("Length", ImGuiTableColumnFlags_WidthStretch, 0.7f);
				ImGui::TableSetupColumn("Notes", ImGuiTableColumnFlags_WidthStretch, 0.7f);
				ImGui::TableSetupColumn("Played", ImGuiTableColumnFlags_WidthStretch, 0.6f);
				ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 100.0f * s);
				int id = 0;
				for (const auto* e : list) {
					SongRow(*e, id++);
				}
				ImGui::EndTable();
			}
			if (!g_files.empty() && list.empty()) {
				ImGui::Spacing();
				ImGui::TextDisabled("No song matches \"%s\".", g_search.c_str());
			}
			if (g_files.empty()) {
				ImGui::Spacing();
				ImGui::PushFont(F.bookIt);
				ImGui::TextWrapped("Your own songs: put MIDI files (.mid) in Data\\SKSE\\Plugins\\SkyrimSongs\\Songs and open the songbook again.");
				ImGui::PopFont();
			}
			ImGui::EndChild();

			// settings and leaving
			auto& cfg = Config::Edit();
			ImGui::SetNextItemWidth(220.0f * s);
			float speed = cfg.speed * 100.0f;
			if (ImGui::SliderFloat("Song speed", &speed, 50.0f, 200.0f, "%.0f%%")) {
				cfg.speed = std::round(speed / 5.0f) * 5.0f / 100.0f;  // in steps of 5%
				Audio::SetSongSpeed(cfg.speed);
			}
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("How fast songs play (100%% = as written). Changes a song that's playing, too.");
			}
			ImGui::SameLine(0.0f, 30.0f * s);
			ImGui::SetNextItemWidth(220.0f * s);
			if (ImGui::SliderFloat("Volume", &cfg.volume, 0.0f, 2.0f, "%.2f")) {
				Audio::SetVolume(cfg.volume);
			}
			if (ImGui::Checkbox("Hide the interface while playing (Backspace)", &cfg.hideHud)) {
				Config::Save();
			}

			if (g_midiCount > 0) {
				std::string names;
				for (const auto& n : g_midiNames) {
					names += (names.empty() ? "" : ", ") + n;
				}
				ImGui::TextDisabled("MIDI keyboard: %s", names.c_str());
			} else {
				ImGui::TextDisabled("No MIDI keyboard found (plug it in before you take out your instrument).");
			}
			ImGui::Spacing();
			if (ImGui::Button("Back to playing", ImVec2(160.0f * s, 0))) {
				Overlay::SetMode(Overlay::Mode::Playing);
			}
			if (g_act == Act::Auto) {
				ImGui::SameLine();
				const bool paused = Audio::SongPaused();
				if (ImGui::Button(paused ? "Carry on (P)" : "Pause (P)", ImVec2(140.0f * s, 0))) {
					Audio::PauseSong(!paused);
					if (paused) {
						EnsureAnim();
					}
				}
			}
			ImGui::SameLine();
			if (ImGui::Button(fmt::format("Put the {} away", InstName(g_inst)).c_str(), ImVec2(180.0f * s, 0))) {
				Stop("put away from the songbook");
			}
			ImGui::SameLine();
			ImGui::TextDisabled("  Esc: back to playing");
			ImGui::End();
		}
	}

	// ============================================================================================== public
	void OnDataLoaded()
	{
		FindLayoutHotkey();
		auto* dh = RE::TESDataHandler::GetSingleton();
		g_gold = RE::TESForm::LookupByID<RE::TESBoundObject>(kGold);
		g_innkeepers = RE::TESForm::LookupByID<RE::TESFaction>(kInnkeeperFaction);
		for (int k = 0; k < 2; ++k) {
			const auto& ids = k == 0 ? std::vector<RE::FormID>(std::begin(kLutes), std::end(kLutes)) : std::vector<RE::FormID>(std::begin(kFlutes), std::end(kFlutes));
			for (auto id : ids) {
				auto* f = RE::TESForm::LookupByID(id);
				if (!f) {
					continue;
				}
				g_instrumentIds[k].insert(id);
				if (auto* m = f->As<RE::TESModel>(); m && m->GetModel() && *m->GetModel()) {
					std::string p = m->GetModel();
					std::transform(p.begin(), p.end(), p.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
					g_instrumentModels[k].insert(p);
				}
			}
		}
		const auto& cfg = Config::Get();
		g_idles[0] = RE::TESForm::LookupByID<RE::TESIdleForm>(cfg.luteIdle ? cfg.luteIdle : kIdleLute);
		g_idles[1] = RE::TESForm::LookupByID<RE::TESIdleForm>(cfg.fluteIdle ? cfg.fluteIdle : kIdleFlute);
		g_stopIdle = RE::TESForm::LookupByID<RE::TESIdleForm>(cfg.stopIdle ? cfg.stopIdle : kIdleStopLoose);
		auto idles = [](auto& a_ids, std::vector<RE::TESIdleForm*>& a_out) {
			for (auto id : a_ids) {
				if (auto* f = RE::TESForm::LookupByID<RE::TESIdleForm>(id)) {
					a_out.push_back(f);
				}
			}
		};
		idles(kIdleApplaud, g_applaud);
		idles(kIdleDance, g_dance);
		idles(kIdleEnjoy, g_enjoy);
		g_give = RE::TESForm::LookupByID<RE::TESIdleForm>(kIdleGive);
		spdlog::info("Audience idles: applause {}, dance {}, enjoy {}, give {}", g_applaud.size(), g_dance.size(), g_enjoy.size(), g_give != nullptr);
		// a music type that's only silence: the highest priority one
		for (auto* m : dh->GetFormArray<RE::BGSMusicType>()) {
			if (!m || m->tracks.empty()) {
				continue;
			}
			bool silent = true;
			for (auto* t : m->tracks) {
				if (!t || t->GetType() != RE::BSIMusicTrack::TrackType::kSilentTrack) {
					silent = false;
					break;
				}
			}
			if (silent && (!g_silence || m->priority < g_silence->priority)) {
				g_silence = m;
			}
		}
		for (auto* c : dh->GetFormArray<RE::BGSSoundCategory>()) {
			const char* name = c ? c->GetFullName() : nullptr;
			if (name && _stricmp(name, cfg.musicCategory.c_str()) == 0) {
				g_musicCats.push_back(c);
			}
		}
		g_tavernSilence = RE::TESForm::LookupByEditorID<RE::BGSMusicType>("MUSTavernSILENCE");
		spdlog::info("Inn silence: {}", g_tavernSilence ? fmt::format("MUSTavernSILENCE {:08X} (priority {})", g_tavernSilence->GetFormID(), g_tavernSilence->priority) :
		                                                  std::string("MUSTavernSILENCE not found"));
		spdlog::info("Forms: gold {}, innkeepers {}, lute models {}, flute models {}, idles lute {} flute {} stop {}, music categories {}, silence {:08X}",
			g_gold != nullptr, g_innkeepers != nullptr, g_instrumentModels[0].size(), g_instrumentModels[1].size(), g_idles[0] != nullptr,
			g_idles[1] != nullptr, g_stopIdle != nullptr, g_musicCats.size(), g_silence ? g_silence->GetFormID() : 0u);
		std::lock_guard lk(g_mtx);
		LoadSongs();
	}

	void OnGameLoaded()
	{
		std::lock_guard lk(g_mtx);
		Stop("game loaded");
		MuteMusic(false);
		for (auto id : g_staleVisitors) {
			ReleaseActor(ActorOf(id));
		}
		g_staleVisitors.clear();
	}

	void FrameTick()
	{
		std::lock_guard lk(g_mtx);
		SafeTick();
	}

	void OnHotkey(int a_scan)
	{
		if (IsHotkey(a_scan)) {
			spdlog::info("Hotkey pressed (0x{:X})", a_scan);
			g_wantStart = true;
		}
	}

	void OnKey(int a_scan, bool a_down)
	{
		std::lock_guard lk(g_mtx);
		if (!g_playing) {
			return;
		}
		if (a_scan == music::keymap::kLeftShift) {
			g_shiftL = a_down;
			return;
		}
		if (a_scan == music::keymap::kRightShift) {
			g_shiftR = a_down;
			return;
		}
		const auto mode = Overlay::GetMode();
		if (mode == Overlay::Mode::Book) {
			if (!a_down) {
				return;
			}
			if (a_scan == 0x01 || IsHotkey(a_scan)) {  // Esc or the hotkey closes the songbook
				Overlay::SetMode(Overlay::Mode::Playing);
			} else if (a_scan == 0x0E) {  // Backspace: the search
				if (!g_search.empty()) {
					g_search.pop_back();
				}
			} else if (const char c = CharOf(a_scan, g_shiftL || g_shiftR); c && g_search.size() < 40) {
				g_search += c;
			}
			return;
		}
		if (a_down && a_scan == 0x19 && g_act == Act::Auto) {  // P: pause the song, or carry on
			const bool pause = !Audio::SongPaused();
			Audio::PauseSong(pause);
			if (!pause) {
				EnsureAnim();
			}
			Say(pause ? "You pause." : "You carry on.", kDim);
			return;
		}
		// a note key (while a song plays itself, the note keys rest)
		if (a_down) {
			const int pitch = g_act == Act::Auto ? -1 : music::keymap::Pitch(a_scan, Base(), g_shiftL || g_shiftR);
			if (pitch >= 0) {
				if (auto it = g_keyPitch.find(a_scan); it != g_keyPitch.end()) {
					Unsound(it->second);
				}
				g_keyPitch[a_scan] = pitch;
				const float vel = std::clamp(0.78f + std::uniform_real_distribution<float>(-0.08f, 0.08f)(g_rng), 0.1f, 1.0f);
				Sound(pitch, vel);
				return;
			}
		} else {
			if (auto it = g_keyPitch.find(a_scan); it != g_keyPitch.end()) {
				Unsound(it->second);
				g_keyPitch.erase(it);
			}
			return;
		}
		// the rest (key down). While you play, every other key is swallowed: the hotkey or Esc gets you out.
		if (IsHotkey(a_scan)) {
			Stop("hotkey");
			return;
		}
		switch (a_scan) {
		case 0x01:  // Esc: stop the song, or put the instrument away
			if (g_act != Act::Free) {
				Say("You stop the song.", kDim);
				BackToFree();
			} else {
				Stop("Esc");
			}
			break;
		case 0x0E:  // Backspace: hide or show everything on screen
			{
				auto& cfg = Config::Edit();
				cfg.hideHud = !cfg.hideHud;
				g_hudToggled = Now();
				Config::Save();
			}
			break;
		case 0x0F:  // Tab: the songbook
			ReleaseAllKeys();
			LoadSongs();
			RefreshOwned();
			Overlay::SetMode(Overlay::Mode::Book);
			break;
		case 0xC8:  // Up
			if (g_octave < 2) {
				ReleaseAllKeys();
				++g_octave;
			}
			break;
		case 0xD0:  // Down
			if (g_octave > -2) {
				ReleaseAllKeys();
				--g_octave;
			}
			break;
		default:
			break;
		}
	}

	void SetupUi(float a_screenH)
	{
		g_ui = (std::max)(0.6f, a_screenH / 1080.0f) * Config::Get().uiScale;
		auto& io = ImGui::GetIO();
		char  windir[MAX_PATH]{};
		::GetWindowsDirectoryA(windir, MAX_PATH);
		const std::string ours = "Data\\SKSE\\Plugins\\SkyrimSongs\\Fonts\\";
		const std::string sys = std::string(windir) + "\\Fonts\\";
		std::error_code   ec;
		auto load = [&](const char* a_file, const char* a_fallback, float a_size) -> ImFont* {
			std::string p = ours + a_file;
			if (!std::filesystem::exists(p, ec)) {
				p = sys + a_fallback;
				if (!std::filesystem::exists(p, ec)) {
					return nullptr;
				}
			}
			ImFontConfig fc;
			fc.OversampleH = 2;
			fc.OversampleV = 1;
			return io.Fonts->AddFontFromFileTTF(p.c_str(), a_size * g_ui, &fc);
		};
		F.body = load("BarlowCondensed-Medium.ttf", "arialn.ttf", 21.0f);
		F.sm = load("BarlowCondensed-Medium.ttf", "arialn.ttf", 16.0f);
		F.key = load("BarlowCondensed-SemiBold.ttf", "arialnb.ttf", 22.0f);
		F.big = load("BarlowCondensed-SemiBold.ttf", "arialnb.ttf", 64.0f);
		F.title = load("IMFellEnglishSC-Regular.ttf", "georgia.ttf", 34.0f);
		F.book = load("IMFellEnglish-Regular.ttf", "georgia.ttf", 21.0f);
		F.bookIt = load("IMFellEnglish-Italic.ttf", "georgiai.ttf", 19.0f);
		ImFont* any = F.body ? F.body : io.Fonts->AddFontDefault();
		for (ImFont** f : { &F.body, &F.sm, &F.key, &F.big, &F.title, &F.book, &F.bookIt }) {
			if (!*f) {
				*f = any;
			}
		}
		io.FontDefault = F.body;

		auto& st = ImGui::GetStyle();
		st = ImGuiStyle();
		st.ScaleAllSizes(g_ui);
		st.WindowRounding = 8.0f * g_ui;
		st.FrameRounding = 4.0f * g_ui;
		st.WindowBorderSize = 1.5f;
		st.WindowPadding = ImVec2(20.0f * g_ui, 16.0f * g_ui);
		st.ItemSpacing = ImVec2(8.0f * g_ui, 7.0f * g_ui);
		auto* c = st.Colors;
		c[ImGuiCol_Text] = ImVec4(0.95f, 0.93f, 0.87f, 1.0f);
		c[ImGuiCol_TextDisabled] = ImVec4(0.72f, 0.68f, 0.6f, 1.0f);
		c[ImGuiCol_WindowBg] = ImVec4(0.07f, 0.055f, 0.04f, 0.94f);
		c[ImGuiCol_ChildBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.18f);
		c[ImGuiCol_PopupBg] = ImVec4(0.09f, 0.07f, 0.05f, 0.97f);
		c[ImGuiCol_Border] = ImVec4(0.78f, 0.62f, 0.35f, 0.45f);
		c[ImGuiCol_FrameBg] = ImVec4(0.2f, 0.16f, 0.11f, 1.0f);
		c[ImGuiCol_FrameBgHovered] = ImVec4(0.28f, 0.22f, 0.15f, 1.0f);
		c[ImGuiCol_FrameBgActive] = ImVec4(0.34f, 0.27f, 0.18f, 1.0f);
		c[ImGuiCol_Button] = ImVec4(0.26f, 0.2f, 0.13f, 1.0f);
		c[ImGuiCol_ButtonHovered] = ImVec4(0.45f, 0.35f, 0.18f, 1.0f);
		c[ImGuiCol_ButtonActive] = ImVec4(0.6f, 0.46f, 0.22f, 1.0f);
		c[ImGuiCol_SliderGrab] = ImVec4(0.85f, 0.68f, 0.38f, 1.0f);
		c[ImGuiCol_SliderGrabActive] = ImVec4(0.95f, 0.78f, 0.45f, 1.0f);
		c[ImGuiCol_Separator] = ImVec4(0.78f, 0.62f, 0.35f, 0.4f);
		c[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
		c[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 0.9f, 0.7f, 0.04f);
		c[ImGuiCol_ScrollbarBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.2f);
		c[ImGuiCol_ScrollbarGrab] = ImVec4(0.45f, 0.35f, 0.2f, 1.0f);
		spdlog::info("Interface: scale {:.2f}, fonts {}", g_ui, std::filesystem::exists(ours + "BarlowCondensed-Medium.ttf", ec) ? "ours" : "Windows'");
	}

	void DrawUi()
	{
		std::lock_guard lk(g_mtx);
		if (!g_playing) {
			return;
		}
		if (!Config::Get().hideHud) {
			DrawHud();
		} else if (const double age = Now() - g_hudToggled; age < 3.0 && Overlay::GetMode() == Overlay::Mode::Playing) {
			// a moment's word that it's hidden, then nothing
			float sw = 0.0f, sh = 0.0f;
			Overlay::ScreenSize(sw, sh);
			const float a = static_cast<float>(age < 2.0 ? 1.0 : 3.0 - age);
			Text(ImGui::GetBackgroundDrawList(), F.sm, { sw * 0.5f, sh - 40.0f * g_ui }, Fade(kDim, a), "Interface hidden  ·  Backspace to show it", 0.5f);
		}
		if (Overlay::GetMode() == Overlay::Mode::Book) {
			DrawBook();
		}
	}

	// ============================================================================================== co-save
	void Save(SKSE::SerializationInterface* a_intfc)
	{
		std::lock_guard lk(g_mtx);
		if (!a_intfc->OpenRecord('BANS', kSaveVersion)) {
			return;
		}
		const auto n = static_cast<std::uint32_t>(g_bans.size());
		a_intfc->WriteRecordData(n);
		for (const auto& [cell, day] : g_bans) {
			a_intfc->WriteRecordData(cell);
			a_intfc->WriteRecordData(day);
		}
		if (a_intfc->OpenRecord('VIST', kSaveVersion)) {
			// a save made mid-performance: the listeners standing with you go home after it's loaded
			const auto v = static_cast<std::uint32_t>(g_visitors.size());
			a_intfc->WriteRecordData(v);
			for (const auto& vis : g_visitors) {
				a_intfc->WriteRecordData(vis.id);
			}
		}
		if (a_intfc->OpenRecord('TIPS', kSaveVersion)) {
			const auto t = static_cast<std::uint32_t>(g_tipped.size());
			a_intfc->WriteRecordData(t);
			for (const auto& [id, day] : g_tipped) {
				a_intfc->WriteRecordData(id);
				a_intfc->WriteRecordData(day);
			}
		}
		if (a_intfc->OpenRecord('STAT', kSaveVersion)) {
			a_intfc->WriteRecordData(g_totalEarned);
			a_intfc->WriteRecordData(g_performances);
		}
		if (a_intfc->OpenRecord('FAME', kSaveVersion)) {
			const auto n2 = static_cast<std::uint32_t>(g_fame.size());
			a_intfc->WriteRecordData(n2);
			for (const auto& [town, f] : g_fame) {
				a_intfc->WriteRecordData(town);
				a_intfc->WriteRecordData(f.value);
				a_intfc->WriteRecordData(f.day);
			}
		}
		auto writeStr = [&](const std::string& a_s) {
			const auto len = static_cast<std::uint32_t>(a_s.size());
			a_intfc->WriteRecordData(len);
			a_intfc->WriteRecordData(a_s.data(), len);
		};
		if (a_intfc->OpenRecord('PLAY', kSaveVersion)) {
			const auto n3 = static_cast<std::uint32_t>(g_plays.size());
			a_intfc->WriteRecordData(n3);
			for (const auto& [title, count] : g_plays) {
				writeStr(title);
				a_intfc->WriteRecordData(count);
			}
		}
		if (a_intfc->OpenRecord('FAVS', kSaveVersion)) {
			const auto n4 = static_cast<std::uint32_t>(g_favourites.size());
			a_intfc->WriteRecordData(n4);
			for (const auto& title : g_favourites) {
				writeStr(title);
			}
		}
		if (a_intfc->OpenRecord('SIGN', kSaveVersion)) {
			const auto towns = static_cast<std::uint32_t>(g_signature.size());
			a_intfc->WriteRecordData(towns);
			for (const auto& [town, songs] : g_signature) {
				a_intfc->WriteRecordData(town);
				const auto n5 = static_cast<std::uint32_t>(songs.size());
				a_intfc->WriteRecordData(n5);
				for (const auto& [title, count] : songs) {
					writeStr(title);
					a_intfc->WriteRecordData(count);
				}
			}
		}
		if (a_intfc->OpenRecord('RFIT', kSaveVersion)) {
			a_intfc->WriteRecordData(g_refit[0]);
			a_intfc->WriteRecordData(g_refit[1]);
		}
		if (a_intfc->OpenRecord('COLL', kSaveVersion)) {
			a_intfc->WriteRecordData(g_collegeShows);
		}
		if (a_intfc->OpenRecord('RANK', kSaveVersion)) {
			a_intfc->WriteRecordData(g_rankSeen);
		}
	}

	void Load(SKSE::SerializationInterface* a_intfc)
	{
		std::lock_guard lk(g_mtx);
		g_bans.clear();
		g_tipped.clear();
		g_fame.clear();
		g_refit[0] = g_refit[1] = 0;
		g_plays.clear();
		g_favourites.clear();
		g_signature.clear();
		g_collegeShows = 0;
		g_rankSeen = 0;
		std::uint32_t type = 0, version = 0, length = 0;
		while (a_intfc->GetNextRecordInfo(type, version, length)) {
			if (version != kSaveVersion) {
				continue;
			}
			if (type == 'BANS') {
				std::uint32_t n = 0;
				a_intfc->ReadRecordData(n);
				for (std::uint32_t i = 0; i < n && i < 10000; ++i) {
					RE::FormID cell = 0, resolved = 0;
					float      day = 0.0f;
					a_intfc->ReadRecordData(cell);
					a_intfc->ReadRecordData(day);
					if (a_intfc->ResolveFormID(cell, resolved)) {
						g_bans[resolved] = day;
					}
				}
			} else if (type == 'VIST') {
				std::uint32_t n = 0;
				a_intfc->ReadRecordData(n);
				for (std::uint32_t i = 0; i < n && i < 16; ++i) {
					RE::FormID id = 0, resolved = 0;
					a_intfc->ReadRecordData(id);
					if (a_intfc->ResolveFormID(id, resolved)) {
						g_staleVisitors.push_back(resolved);
					}
				}
			} else if (type == 'TIPS') {
				std::uint32_t n = 0;
				a_intfc->ReadRecordData(n);
				for (std::uint32_t i = 0; i < n && i < 100000; ++i) {
					RE::FormID id = 0, resolved = 0;
					float      day = 0.0f;
					a_intfc->ReadRecordData(id);
					a_intfc->ReadRecordData(day);
					if (a_intfc->ResolveFormID(id, resolved)) {
						g_tipped[resolved] = day;
					}
				}
			} else if (type == 'STAT') {
				a_intfc->ReadRecordData(g_totalEarned);
				a_intfc->ReadRecordData(g_performances);
			} else if (type == 'FAME') {
				std::uint32_t n = 0;
				a_intfc->ReadRecordData(n);
				for (std::uint32_t i = 0; i < n && i < 10000; ++i) {
					RE::FormID town = 0, resolved = 0;
					Fame       f;
					a_intfc->ReadRecordData(town);
					a_intfc->ReadRecordData(f.value);
					a_intfc->ReadRecordData(f.day);
					if (a_intfc->ResolveFormID(town, resolved)) {
						g_fame[resolved] = f;
					}
				}
			} else if (type == 'SIGN') {
				auto readStr = [&](std::string& a_s) {
					std::uint32_t len = 0;
					a_intfc->ReadRecordData(len);
					if (len > 4096) {
						return false;
					}
					a_s.resize(len);
					return len == 0 || a_intfc->ReadRecordData(a_s.data(), len) == len;
				};
				std::uint32_t towns = 0;
				a_intfc->ReadRecordData(towns);
				bool ok = true;
				for (std::uint32_t t = 0; ok && t < towns && t < 10000; ++t) {
					RE::FormID    town = 0, resolved = 0;
					std::uint32_t n = 0;
					a_intfc->ReadRecordData(town);
					a_intfc->ReadRecordData(n);
					const bool known = a_intfc->ResolveFormID(town, resolved);
					for (std::uint32_t i = 0; i < n && i < 100000; ++i) {
						std::string title;
						int         count = 0;
						if (!readStr(title)) {
							ok = false;
							break;
						}
						a_intfc->ReadRecordData(count);
						if (known) {
							g_signature[resolved][title] = count;
						}
					}
				}
			} else if (type == 'PLAY' || type == 'FAVS') {
				auto readStr = [&](std::string& a_s) {
					std::uint32_t len = 0;
					a_intfc->ReadRecordData(len);
					if (len > 4096) {
						return false;
					}
					a_s.resize(len);
					return len == 0 || a_intfc->ReadRecordData(a_s.data(), len) == len;
				};
				std::uint32_t n = 0;
				a_intfc->ReadRecordData(n);
				for (std::uint32_t i = 0; i < n && i < 100000; ++i) {
					std::string title;
					if (!readStr(title)) {
						break;
					}
					if (type == 'PLAY') {
						int count = 0;
						a_intfc->ReadRecordData(count);
						g_plays[title] = count;
					} else {
						g_favourites.insert(title);
					}
				}
			} else if (type == 'RANK') {
				a_intfc->ReadRecordData(g_rankSeen);
			} else if (type == 'COLL') {
				a_intfc->ReadRecordData(g_collegeShows);
				g_collegeShows = std::clamp(g_collegeShows, 0, 10);
			} else if (type == 'RFIT') {
				a_intfc->ReadRecordData(g_refit[0]);
				a_intfc->ReadRecordData(g_refit[1]);
				g_refit[0] = std::clamp(g_refit[0], 0, 2);
				g_refit[1] = std::clamp(g_refit[1], 0, 2);
			}
		}
		spdlog::info("Co-save: {} inn ban(s), {} septims earned in {} performance(s), known in {} town(s), refits {}/{}", g_bans.size(), g_totalEarned,
			g_performances, g_fame.size(), g_refit[0], g_refit[1]);
	}

	void Revert(SKSE::SerializationInterface*)
	{
		std::lock_guard lk(g_mtx);
		g_bans.clear();
		g_tipped.clear();
		g_totalEarned = 0;
		g_performances = 0;
		g_fame.clear();
		g_refit[0] = g_refit[1] = 0;
		g_plays.clear();
		g_favourites.clear();
		g_signature.clear();
		g_collegeShows = 0;
		g_rankSeen = 0;
	}
}
