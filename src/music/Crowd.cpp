// SkyrimSongs - play the lute and flute in Skyrim.
// Copyright (C) 2026 evodube. GPL-3.0 (LICENSE.txt).
#include "Crowd.h"
#include "Text.h"

#include <algorithm>
#include <cmath>

namespace music
{
	namespace
	{
		using Line = Crowd::Line;

		const std::vector<Line> kHeckles = {
			{ "CrowdHeckle1", "By Ysmir, my ears!" },
			{ "CrowdHeckle2", "Did you learn that from a skeever?" },
			{ "CrowdHeckle3", "Play something we know!" },
			{ "CrowdHeckle4", "My horker sings better than that." },
			{ "CrowdHeckle5", "Stop! You're curdling the mead!" },
			{ "CrowdHeckle6", "Was that a song or a cry for help?" },
			{ "CrowdHeckle7", "Sweet Mara, make it stop." },
			{ "CrowdHeckle8", "I'd rather hear a dragon gargle." },
			{ "CrowdHeckle9", "Go back to the College and ask for your coin back!" },
			{ "CrowdHeckle10", "Boo! Off with you!" },
			{ "CrowdHeckle11", "Even a draugr would walk out on this." },
			{ "CrowdHeckle12", "Is that thing strung with sinew?" },
		};
		const std::vector<Line> kCheers = {
			{ "CrowdCheer1", "Now that's a tune!" },
			{ "CrowdCheer2", "Another! Play another!" },
			{ "CrowdCheer3", "By Talos, they can play!" },
			{ "CrowdCheer4", "Ha! Fingers of gold, this one." },
			{ "CrowdCheer5", "Better than the house bard, I'd say." },
			{ "CrowdCheer6", "That takes me back..." },
			{ "CrowdCheer7", "Shor's bones, that's lovely." },
			{ "CrowdCheer8", "Keep playing, friend!" },
		};
		const std::vector<Line> kTips = {
			{ "CrowdTipLine1", "Here, for your trouble." },
			{ "CrowdTipLine2", "Keep it coming." },
			{ "CrowdTipLine3", "Buy yourself some new strings." },
			{ "CrowdTipLine4", "For the song." },
			{ "CrowdTipLine5", "Lovely playing." },
			{ "CrowdTipLine6", "Have a drink on me." },
			{ nullptr, "" },
			{ nullptr, "" },
		};
		const std::vector<Line> kRelief = {
			{ "CrowdRelief1", "Is it over? Thank the Divines." },
			{ "CrowdRelief2", "Finally." },
			{ "CrowdRelief3", "Don't play that one again." },
		};
	}

	Crowd::Crowd(std::uint32_t a_seed) :
		m_rng(a_seed)
	{}

	double Crowd::Rand() { return std::uniform_real_distribution<double>(0.0, 1.0)(m_rng); }

	std::string Crowd::Pick(const std::vector<Line>& a_lines)
	{
		const Line& l = a_lines[std::uniform_int_distribution<std::size_t>(0, a_lines.size() - 1)(m_rng)];
		return l.key ? Tr(l.key, l.english) : std::string();
	}

	double Crowd::MoodOf(std::uint32_t a_id) const
	{
		for (const auto& p : m_people) {
			if (p.id == a_id) {
				return p.mood;
			}
		}
		return -1.0;
	}

	void Crowd::Lift(double a_by)
	{
		for (auto& p : m_people) {
			p.mood = std::clamp(p.mood + a_by, 0.0, 1.0);
		}
	}

	void Crowd::SetTaste(std::uint32_t a_id, double a_taste)
	{
		for (auto& p : m_people) {
			if (p.id == a_id) {
				p.taste = a_taste;
			}
		}
	}

	void Crowd::Sync(const std::vector<std::uint32_t>& a_present, const std::vector<std::uint32_t>* a_noTips)
	{
		std::erase_if(m_people, [&](const Person& p) { return std::find(a_present.begin(), a_present.end(), p.id) == a_present.end(); });
		for (auto id : a_present) {
			const bool known = std::any_of(m_people.begin(), m_people.end(), [&](const Person& p) { return p.id == id; });
			if (!known) {
				Person p{};
				p.id = id;
				// newcomers take their cue from the room
				p.mood = std::clamp((m_people.empty() ? 0.5 : Mood()) * 0.5 + 0.25 + (Rand() - 0.5) * 0.1 + m_set.welcome, 0.1, 0.9);
				p.taste = 0.7 + Rand() * 0.6;
				p.purse = 0.5 + Rand() * 1.0;
				p.sinceTip = 4.0 * Rand();
				p.sinceShout = 6.0 * Rand();
				if (a_noTips && std::find(a_noTips->begin(), a_noTips->end(), id) != a_noTips->end()) {
					p.given = m_set.maxPerListener;
				}
				m_people.push_back(p);
			}
		}
	}

	double Crowd::Mood() const
	{
		if (m_people.empty()) {
			return 0.5;
		}
		double s = 0.0;
		for (const auto& p : m_people) {
			s += p.mood;
		}
		return s / m_people.size();
	}

	const char* Crowd::MoodWord(double a_mood)
	{
		return a_mood < 0.18 ? Tr("MoodHostile", "Hostile") : a_mood < 0.35 ? Tr("MoodRestless", "Restless") : a_mood < 0.55 ? Tr("MoodListening", "Listening") :
		       a_mood < 0.78 ? Tr("MoodEnjoying", "Enjoying it") : Tr("MoodSpellbound", "Spellbound");
	}

	void Crowd::Step(double a_dt, double a_quality, double a_skill, bool a_playing, std::vector<CrowdEvent>& a_out)
	{
		m_sinceAnyShout += a_dt;
		for (auto& p : m_people) {
			p.sinceTip += a_dt;
			p.sinceShout += a_dt;
			if (a_playing) {
				p.heard += a_dt;
				const double d = a_quality - 0.5;
				const double k = 0.06 * m_set.patience * (d < 0.0 ? p.taste : 1.0 / p.taste);
				p.mood += d * k * a_dt;
			} else {
				p.mood += (0.5 - p.mood) * 0.01 * a_dt;
			}
			p.mood = std::clamp(p.mood, 0.0, 1.0);

			// a tip has to be earned: they've listened a good while (20 s at least) and are really enjoying it
			if (a_playing && p.mood > 0.7 && p.heard > 20.0 && p.given < m_set.maxPerListener && p.sinceTip > 6.0) {
				const double lam = 0.012 * m_set.tipRate * std::pow((p.mood - 0.7) / 0.3, 1.5) * p.purse * (1.0 + a_skill * 0.5);
				if (Rand() < 1.0 - std::exp(-lam * a_dt)) {
					int gold = static_cast<int>(std::lround((1.0 + Rand() * 4.0) * (0.6 + p.mood) * p.purse * m_set.tipSize * (1.0 + a_skill)));
					gold = std::clamp(gold, 1, m_set.maxPerListener - p.given);
					p.given = m_set.oneTipEach ? m_set.maxPerListener : p.given + gold;
					p.sinceTip = 0.0;
					m_earned += gold;
					a_out.push_back({ CrowdEvent::Kind::Tip, p.id, gold, Pick(kTips), {} });
					continue;
				}
			}
			if (a_playing && p.mood < 0.35 && p.sinceShout > 10.0 && m_sinceAnyShout > 3.0) {
				const double lam = 0.08 * ((0.35 - p.mood) / 0.35) * p.taste;
				if (Rand() < 1.0 - std::exp(-lam * a_dt)) {
					p.sinceShout = 0.0;
					m_sinceAnyShout = 0.0;
					a_out.push_back({ CrowdEvent::Kind::Heckle, p.id, 0, Pick(kHeckles), {} });
					continue;
				}
			}
			if (a_playing && p.mood > 0.8 && p.sinceShout > 20.0 && m_sinceAnyShout > 4.0) {
				if (Rand() < 1.0 - std::exp(-0.03 * a_dt)) {
					p.sinceShout = 0.0;
					m_sinceAnyShout = 0.0;
					a_out.push_back({ CrowdEvent::Kind::Cheer, p.id, 0, Pick(kCheers), {} });
				}
			}
		}
	}

	void Crowd::SongEnded(double a_score, double a_skill, std::vector<CrowdEvent>& a_out)
	{
		if (m_people.empty()) {
			return;
		}
		if (a_score >= 0.6) {
			int                        collection = 0;
			std::vector<std::uint32_t> payers;
			for (auto& p : m_people) {
				p.mood = std::clamp(p.mood + (a_score - 0.5) * 0.2, 0.0, 1.0);
				if (p.mood > 0.5 && p.given < m_set.maxPerListener) {
					int g = static_cast<int>(std::lround((a_score - 0.5) * 10.0 * p.purse * m_set.tipSize * (1.0 + a_skill)));
					g = std::clamp(g, 0, m_set.maxPerListener - p.given);
					if (g > 0) {
						p.given = m_set.oneTipEach ? m_set.maxPerListener : p.given + g;
						collection += g;
						payers.push_back(p.id);
					}
				}
			}
			m_earned += collection;
			CrowdEvent e{ CrowdEvent::Kind::Applause, m_people[static_cast<std::size_t>(Rand() * m_people.size()) % m_people.size()].id, collection,
				Pick(kCheers), {} };
			e.payers = std::move(payers);
			a_out.push_back(std::move(e));
		} else if (a_score < 0.35) {
			for (auto& p : m_people) {
				p.mood = std::clamp(p.mood - 0.05, 0.0, 1.0);
			}
			m_sinceAnyShout = 0.0;
			a_out.push_back({ CrowdEvent::Kind::Heckle, m_people[static_cast<std::size_t>(Rand() * m_people.size()) % m_people.size()].id, 0, Pick(kRelief), {} });
		}
	}
}
