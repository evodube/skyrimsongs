// SkyrimSongs - play the lute and flute in Skyrim.
// Copyright (C) 2026 evodube. GPL-3.0 (LICENSE.txt).
#pragma once

#include <cstdint>
#include <random>
#include <string>
#include <vector>

// The listeners: each has a mood that rises and falls with how well you play, a taste (some are harder to please)
// and a purse. Happy listeners toss coins, unhappy ones heckle, and a crowd that's had enough wants you gone.
// Plain C++: the plugin tells it who's in earshot and how well you're playing, and acts on what it says happened.
namespace music
{
	struct CrowdEvent
	{
		enum class Kind : std::uint8_t
		{
			Tip,     // gold
			Heckle,  // line
			Cheer,   // line
			Applause // a song finished well: gold is the collection
		} kind;
		std::uint32_t id = 0;  // the listener (an actor's form ID in game)
		int           gold = 0;
		std::string   line;
		std::vector<std::uint32_t> payers;  // Applause: who put into the collection
	};

	class Crowd
	{
	public:
		struct Settings
		{
			float tipRate = 1.0f;       // how often tips come
			float tipSize = 1.0f;       // how big they are
			int   maxPerListener = 40;  // gold one listener gives in one performance at most
			bool  oneTipEach = false;   // each listener tips once (the plugin: once a day)
			float patience = 1.0f;      // how quickly a bad set turns them
			float welcome = 0.0f;       // how newcomers arrive: + warmer (you're known and liked here), - cooler
		};

		explicit Crowd(std::uint32_t a_seed = 1);
		void SetSettings(const Settings& a_s) { m_set = a_s; }

		// Who's listening now. Newcomers arrive curious; anyone gone is forgotten. a_noTips: listeners who won't
		// tip (they already did today)
		void Sync(const std::vector<std::uint32_t>& a_present, const std::vector<std::uint32_t>* a_noTips = nullptr);
		double MoodOf(std::uint32_t a_id) const;  // -1 if not listening

		// a_quality: 0..1 how well you're playing; a_skill: 0..1 (Speech), a_playing: false while you're between songs
		void Step(double a_dt, double a_quality, double a_skill, bool a_playing, std::vector<CrowdEvent>& a_out);

		// A song played along with has ended: a good one gets applause and a collection
		void SongEnded(double a_score, double a_skill, std::vector<CrowdEvent>& a_out);

		double Mood() const;  // 0..1 averaged; 0.5 with nobody there
		void   Lift(double a_by);
		void   SetTaste(std::uint32_t a_id, double a_taste);  // > 1 harder to please (a teacher), < 1 easier (a student)  // everyone's mood up (or down) at once: a song the room knows and loves
		std::size_t Size() const { return m_people.size(); }
		int    Earned() const { return m_earned; }
		static const char* MoodWord(double a_mood);

	private:
		struct Person
		{
			std::uint32_t id;
			double        mood;
			double        taste;     // > 1 harder to please
			double        purse;     // > 1 more generous
			int           given = 0;
			double        sinceTip = 0.0;
			double        sinceShout = 0.0;
			double        heard = 0.0;  // seconds they've listened to you playing
		};

	public:
		struct Line
		{
			const char* key;  // nullptr: says nothing
			const char* english;
		};

	private:
		std::string Pick(const std::vector<Line>& a_lines);
		double             Rand();

		Settings            m_set;
		std::mt19937        m_rng;
		std::vector<Person> m_people;
		int                 m_earned = 0;
		double              m_sinceAnyShout = 0.0;
	};
}
