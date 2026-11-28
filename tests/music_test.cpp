// SkyrimSongs tests: the MIDI reader, the judges, the crowd, the built-in songs, the keyboard and the synth.
#include "../src/music/Crowd.h"
#include "../src/music/Judge.h"
#include "../src/music/Keymap.h"
#include "../src/music/Midi.h"
#include "../src/music/Songs.h"
#include "../src/music/Synth.h"

#include <cmath>
#include <map>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

static int g_fail = 0;
#define CHECK(c)                                                          \
	do {                                                                  \
		if (!(c)) {                                                       \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);      \
			++g_fail;                                                     \
		}                                                                 \
	} while (0)

namespace
{
	using namespace music;

	void Vlq(std::vector<std::uint8_t>& o, std::uint32_t v)
	{
		std::uint8_t buf[4];
		int          n = 0;
		buf[n++] = v & 0x7F;
		while (v >>= 7) {
			buf[n++] = static_cast<std::uint8_t>((v & 0x7F) | 0x80);
		}
		while (n) {
			o.push_back(buf[--n]);
		}
	}
	void U32(std::vector<std::uint8_t>& o, std::uint32_t v)
	{
		for (int i = 3; i >= 0; --i) {
			o.push_back(static_cast<std::uint8_t>(v >> (i * 8)));
		}
	}
	void Chunk(std::vector<std::uint8_t>& o, const char* tag, const std::vector<std::uint8_t>& body)
	{
		o.insert(o.end(), tag, tag + 4);
		U32(o, static_cast<std::uint32_t>(body.size()));
		o.insert(o.end(), body.begin(), body.end());
	}

	// format 1, 480 tpq: a conductor track (120 bpm, then 60 bpm from beat 4) and a melody using running status and
	// note-on velocity 0 for note-off, plus a drum note that must be ignored.
	std::vector<std::uint8_t> MakeMidi()
	{
		std::vector<std::uint8_t> f, hdr = { 0, 1, 0, 2, 0x01, 0xE0 };
		Chunk(f, "MThd", hdr);
		std::vector<std::uint8_t> t0;
		Vlq(t0, 0);
		t0.insert(t0.end(), { 0xFF, 0x51, 3, 0x07, 0xA1, 0x20 });  // 500000 us
		Vlq(t0, 480 * 4);
		t0.insert(t0.end(), { 0xFF, 0x51, 3, 0x0F, 0x42, 0x40 });  // 1000000 us
		Vlq(t0, 0);
		t0.insert(t0.end(), { 0xFF, 0x2F, 0 });
		Chunk(f, "MTrk", t0);
		std::vector<std::uint8_t> t1;
		Vlq(t1, 0);
		t1.insert(t1.end(), { 0xFF, 0x03, 4, 'L', 'e', 'a', 'd' });
		const int pitches[6] = { 60, 62, 64, 65, 67, 69 };
		bool      first = true;
		for (int p : pitches) {
			Vlq(t1, 0);
			if (first) {
				t1.push_back(0x90);
				first = false;
			}
			t1.push_back(static_cast<std::uint8_t>(p));
			t1.push_back(100);
			Vlq(t1, 480);
			t1.push_back(static_cast<std::uint8_t>(p));  // running status, velocity 0 = off
			t1.push_back(0);
		}
		Vlq(t1, 0);
		t1.insert(t1.end(), { 0x99, 36, 100 });
		Vlq(t1, 100);
		t1.insert(t1.end(), { 0x89, 36, 0 });
		Vlq(t1, 0);
		t1.insert(t1.end(), { 0xFF, 0x2F, 0 });
		Chunk(f, "MTrk", t1);
		return f;
	}

	void TestMidi()
	{
		MidiFile    m;
		std::string err;
		CHECK(ParseMidi(MakeMidi(), m, &err));
		CHECK(m.notes.size() == 6);  // drum left out
		CHECK(m.trackNames.size() == 2 && m.trackNames[1] == "Lead");
		// beats 0-3 at 0.5 s, beats 4-5 at 1 s
		const double expect[6] = { 0.0, 0.5, 1.0, 1.5, 2.0, 3.0 };
		for (int i = 0; i < 6 && i < static_cast<int>(m.notes.size()); ++i) {
			CHECK(std::abs(m.notes[i].start - expect[i]) < 1e-6);
		}
		if (m.notes.size() == 6) {
			CHECK(std::abs(m.notes[4].dur - 1.0) < 1e-6);
			CHECK(m.notes[0].pitch == 60 && m.notes[5].pitch == 69);
		}
		CHECK(std::abs(m.length - 4.0) < 1e-6);
		auto mel = ExtractMelody(m);
		CHECK(mel.size() == 6 || mel.empty());  // 6 notes is under the 8 a part needs to be preferred, but it's all there is
		MidiFile bad;
		CHECK(!ParseMidi({ 1, 2, 3, 4 }, bad));
		std::vector<std::uint8_t> junk = MakeMidi();
		junk.resize(40);  // cut short: must not crash
		ParseMidi(junk, bad);
		Song s;
		CHECK(SongFromMidi(MakeMidi(), "Test", s));
		CHECK(!s.melody.empty());
	}

	void TestMelodyFromChords()
	{
		MidiFile f;
		// a chordal part (low) and a melody (high), 16 beats
		for (int i = 0; i < 16; ++i) {
			for (int p : { 48, 52, 55 }) {
				f.notes.push_back({ i * 0.5, 0.5, p, 80, 0, 1 });
			}
			f.notes.push_back({ i * 0.5, 0.4, 72 + (i % 5), 100, 1, 2 });
		}
		int track = -1;
		auto mel = ExtractMelody(f, &track);
		CHECK(track == 2);
		CHECK(mel.size() == 16);
	}

	void TestSongs()
	{
		auto songs = BuiltInSongs();
		CHECK(songs.size() == 5);
		for (const auto& s : songs) {
			CHECK(!s.melody.empty());
			CHECK(s.length > 15.0);
			for (std::size_t i = 1; i < s.melody.size(); ++i) {
				CHECK(s.melody[i].start > s.melody[i - 1].start);
			}
			// every tune fits the lute's and the flute's three rows
			for (int base : { 48, 60 }) {
				const int shift = FitOctaves(s.melody, base, 36);
				int       out = 0;
				for (const auto& n : s.melody) {
					keymap::Label l;
					out += keymap::LabelFor(n.pitch + shift, base, l) ? 0 : 1;
				}
				CHECK(out == 0);
			}
		}
		// folding a wide arrangement onto the 37 keys
		{
			std::vector<Note> wide = { { 0.0, 1.0, 24, 90, 0, 0 }, { 0.0, 1.0, 36, 90, 0, 0 }, { 0.5, 1.0, 100, 90, 0, 0 }, { 1.0, 1.0, 60, 90, 0, 0 } };
			const auto        f = FoldIntoRange(wide, 36, 72);
			CHECK(f.size() == 3);  // the 24 folds onto the 36 already there
			for (const auto& x : f) {
				CHECK(x.pitch >= 36 && x.pitch <= 72);
			}
		}
		// a busy arrangement cut down: the melody plus a sparse bass under it
		{
			Song song;
			for (int b = 0; b < 16; ++b) {
				const double t = b * 0.25;
				song.melody.push_back({ t, 0.25, 72 + (b % 5), 100, 0, 1 });
				song.all.push_back({ t, 0.25, 72 + (b % 5), 100, 0, 1 });
				// a chord on every beat: bass and inner voices
				for (int p : { 40 + (b / 4) * 2, 55, 59, 62 }) {
					song.all.push_back({ t, 0.25, p, 90, 1, 2 });
				}
			}
			const auto simple = Simplify(song);
			int        under = 0;
			for (const auto& x : simple) {
				under += x.pitch < 64 ? 1 : 0;
				CHECK(x.pitch == 40 + static_cast<int>(x.start / 1.0) * 2 || x.pitch >= 72);  // only the melody and the bass
			}
			CHECK(simple.size() < song.all.size() / 2);
			CHECK(under >= 4 && under <= 8);  // the bass where it moves, no faster than every 0.25-0.45 s
			for (std::size_t i = 1; i < simple.size(); ++i) {
				CHECK(simple[i].start >= simple[i - 1].start);
			}
		}
		std::vector<Note> n;
		CHECK(!ParseNotation("C4:1 X9:1", 120, n));
		CHECK(ParseNotation("C4:1 r:1 D#4:.5", 120, n) && n.size() == 2 && n[1].pitch == 63 && std::abs(n[1].start - 1.0) < 1e-9);
		CHECK(NoteName(61) == "C#4" && NoteName(48) == "C3");
	}

	void TestKeymap()
	{
		CHECK(keymap::Pitch(0x2C, 48, false) == 48);   // Z = C3
		CHECK(keymap::Pitch(0x1E, 48, false) == 60);   // A = C4
		CHECK(keymap::Pitch(0x16, 48, false) == 83);   // U = B5
		CHECK(keymap::Pitch(0x1F, 48, true) == 63);    // Shift+S = D#4
		CHECK(keymap::Pitch(0x18, 48, false) == -1);   // O (the hotkey): not a note
		CHECK(keymap::Pitch(0x17, 48, false) == 84);   // I = C6, the top C
		// all 37 notes have a key
		int notes = 0;
		for (int p = 48; p <= 84; ++p) {
			keymap::Label l;
			CHECK(keymap::LabelFor(p, 48, l));
			const int scan = l.col == 7 ? keymap::kTopC : keymap::kRows[l.row][l.col];
			CHECK(keymap::Pitch(scan, 48, l.shift) == p);
			++notes;
		}
		CHECK(notes == keymap::kNotes);
		keymap::Label l;
		CHECK(!keymap::LabelFor(47, 48, l));
		CHECK(!keymap::LabelFor(85, 48, l));
	}

	void TestPadmap()
	{
		CHECK(padmap::Pitch(padmap::Index(0x0002), 48, false, false, false) == 60);  // D-pad down = middle C
		CHECK(padmap::Pitch(padmap::Index(0x1000), 48, true, false, false) == 55);   // LT+A = G3
		CHECK(padmap::Pitch(padmap::Index(0x2000), 48, false, false, false) == 72);  // B = the C above
		CHECK(padmap::Pitch(padmap::Index(0x2000), 48, false, true, false) == 84);   // RT+B = the top C
		CHECK(padmap::Pitch(padmap::Index(0x2000), 48, false, true, true) == -1);    // nothing above it
		CHECK(padmap::Pitch(padmap::Index(0x0004), 48, false, false, true) == 63);   // LB+left = D#4
		CHECK(padmap::Index(0x0100) == -1 && padmap::Index(0x0009) == -1);
		// every one of the 37 notes the keyboard has, the pad has, and the guide names the right buttons
		for (int p = 48; p <= 84; ++p) {
			keymap::Label l;
			CHECK(keymap::LabelFor(p, 48, l));
			const std::string trig = padmap::Trigger(l.row, l.col);
			const int         idx = l.col == 7 ? 7 : l.col;
			CHECK(padmap::Pitch(idx, 48, trig == "LT", trig == "RT", l.shift) == p);
			CHECK(padmap::Text(l.row, l.col, l.shift).find(padmap::Button(l.col)) != std::string::npos);
		}
		CHECK(padmap::Text(2, 7, false) == "RT+B" && padmap::Text(0, 4, true) == "LB+LT+A" && padmap::Text(1, 5, false) == "X");
	}

	// averaged over the performance, the way the crowd hears it (judged every half second from 4 s on)
	double Heard(const std::vector<std::pair<double, int>>& a_notes)
	{
		double sum = 0.0;
		int    n = 0;
		for (double t = 4.0; t <= a_notes.back().first + 0.5; t += 0.5) {
			FreeJudge j;
			for (auto [nt, p] : a_notes) {
				if (nt <= t) {
					j.Note(nt, p);
				}
			}
			sum += j.Judge(t).quality;
			++n;
		}
		return n ? sum / n : 0.0;
	}

	void TestFreeJudge()
	{
		// real tunes, played freely, go down well
		for (const auto& s : BuiltInSongs()) {
			std::vector<std::pair<double, int>> n;
			for (const auto& x : s.melody) {
				n.push_back({ x.start, x.pitch });
			}
			const double q = Heard(n);
			std::printf("free play: %-18s %.2f\n", s.title.c_str(), q);
			CHECK(q > 0.7);
		}
		std::mt19937 rng(7);
		auto         pick = [&](int a, int b) { return std::uniform_int_distribution<int>(a, b)(rng); };
		// random keys, even perfectly in time, don't
		std::vector<std::pair<double, int>> randomSteady, randomAny, chromatic, spam, hammer, clusters, triads;
		double                              t = 0.0;
		for (int i = 0; i < 40; ++i) {
			randomSteady.push_back({ i * 0.4, 48 + pick(0, 2) * 12 + keymap::kWhite[pick(0, 6)] });
			t += std::uniform_real_distribution<double>(0.1, 0.5)(rng);
			randomAny.push_back({ t, 48 + pick(0, 2) * 12 + keymap::kWhite[pick(0, 6)] });
			chromatic.push_back({ i * 0.4, 48 + pick(0, 35) });
		}
		for (int i = 0; i < 80; ++i) {
			spam.push_back({ i * 0.07, 60 + pick(0, 11) });
			hammer.push_back({ i * 0.12, 60 });
		}
		for (int i = 0; i < 16; ++i) {
			const int r = 60 + pick(0, 11);
			clusters.push_back({ i * 0.6, r });
			clusters.push_back({ i * 0.6 + 0.01, r + 1 });
			clusters.push_back({ i * 0.6 + 0.02, r + 6 });
			static constexpr int kChords[4][3] = { { 60, 64, 67 }, { 65, 69, 72 }, { 67, 71, 74 }, { 60, 64, 67 } };
			for (int k = 0; k < 3; ++k) {
				triads.push_back({ i * 0.8 + k * 0.01, kChords[i % 4][k] });
			}
		}
		const double qSteady = Heard(randomSteady), qAny = Heard(randomAny), qChrom = Heard(chromatic), qSpam = Heard(spam), qHammer = Heard(hammer),
					 qClusters = Heard(clusters), qTriads = Heard(triads);
		std::printf("free play: random in time %.2f, random %.2f, chromatic %.2f, spam %.2f, hammering %.2f, clashing chords %.2f, I-IV-V-I %.2f\n",
			qSteady, qAny, qChrom, qSpam, qHammer, qClusters, qTriads);
		CHECK(qSteady < 0.55);
		CHECK(qAny < 0.35);
		CHECK(qChrom < 0.55);
		CHECK(qSpam < 0.2);
		CHECK(qHammer < 0.4);
		CHECK(qClusters < 0.35);
		CHECK(qTriads > 0.6);
		// silence isn't judged: the room just drifts
		FreeJudge j;
		j.Note(1.0, 60);
		CHECK(!j.Silent(4.0));
		CHECK(j.Silent(7.0));
		FreeJudge empty;
		CHECK(empty.Judge(5.0).quality < 0.2);
	}

	void TestChartJudge()
	{
		std::vector<Note> chart;
		for (int i = 0; i < 20; ++i) {
			chart.push_back({ 1.0 + i * 0.5, 0.4, 60 + (i % 7), 100, 0, 0 });
		}
		ChartJudge perfect;
		perfect.Load(chart);
		for (const auto& n : chart) {
			perfect.Advance(n.start);
			perfect.Note(n.start + 0.02, n.pitch + 12);  // an octave up counts
		}
		perfect.Advance(20.0);
		CHECK(perfect.Perfects() == 20 && perfect.Misses() == 0);
		CHECK(perfect.Score() > 0.99);
		CHECK(perfect.Quality() > 0.95);
		CHECK(perfect.Done(20.0));

		ChartJudge sloppy;
		sloppy.Load(chart);
		for (std::size_t i = 0; i < chart.size(); ++i) {
			sloppy.Advance(chart[i].start);
			if (i % 2 == 0) {
				sloppy.Note(chart[i].start + 0.12, chart[i].pitch);  // late
			} else {
				sloppy.Note(chart[i].start, chart[i].pitch + 1);  // wrong note
			}
		}
		sloppy.Advance(20.0);
		CHECK(sloppy.Goods() == 10 && sloppy.Misses() == 10 && sloppy.Extras() == 10);
		CHECK(sloppy.Score() < 0.45);
		CHECK(sloppy.Quality() < perfect.Quality());

		ChartJudge idle;
		idle.Load(chart);
		idle.Advance(20.0);
		CHECK(idle.Misses() == 20 && idle.Score() == 0.0 && idle.Quality() < 0.05);
	}

	void TestCrowd()
	{
		auto run = [](double q, int& tips, int& heckles, double& mood, int& earned) {
			Crowd c(42);
			c.Sync({ 1, 2, 3, 4, 5, 6 });
			std::vector<CrowdEvent> ev;
			for (int i = 0; i < 180 * 10; ++i) {
				c.Step(0.1, q, 0.3, true, ev);
			}
			tips = heckles = 0;
			for (const auto& e : ev) {
				tips += e.kind == CrowdEvent::Kind::Tip;
				heckles += e.kind == CrowdEvent::Kind::Heckle;
			}
			mood = c.Mood();
			earned = c.Earned();
		};
		int    tips = 0, heckles = 0, earned = 0;
		double mood = 0.0;
		run(0.9, tips, heckles, mood, earned);
		std::printf("crowd, 3 min played well: %d tips (%d gold), %d heckles, mood %.2f\n", tips, earned, heckles, mood);
		CHECK(tips >= 6 && heckles == 0 && mood > 0.8);
		CHECK(earned <= 6 * 40);
		run(0.1, tips, heckles, mood, earned);
		std::printf("crowd, 3 min played badly: %d tips, %d heckles, mood %.2f\n", tips, heckles, mood);
		CHECK(tips == 0 && heckles >= 3 && mood < 0.15);

		// one tip each (the plugin: once a day), and those who tipped earlier today give nothing
		{
			Crowd one(5);
			Crowd::Settings st;
			st.oneTipEach = true;
			one.SetSettings(st);
			const std::vector<std::uint32_t> tippedToday = { 3 };
			one.Sync({ 1, 2, 3, 4 }, &tippedToday);
			std::vector<CrowdEvent> ev;
			for (int i = 0; i < 6000; ++i) {
				one.Step(0.1, 0.95, 0.5, true, ev);
			}
			std::map<std::uint32_t, int> tipsBy;
			for (const auto& e : ev) {
				if (e.kind == CrowdEvent::Kind::Tip) {
					++tipsBy[e.id];
				}
			}
			CHECK(!tipsBy.contains(3));
			for (const auto& [id, n] : tipsBy) {
				CHECK(n == 1);
			}
			CHECK(tipsBy.size() == 3);
			ev.clear();
			one.SongEnded(0.95, 0.5, ev);
			CHECK(ev.size() == 1 && ev[0].payers.empty() && ev[0].gold == 0);  // everyone has given already
		}
		Crowd c(3);
		c.Sync({ 10, 11 });
		CHECK(c.Size() == 2);
		c.Sync({ 11, 12, 13 });
		CHECK(c.Size() == 3);
		std::vector<CrowdEvent> ev;
		c.SongEnded(0.95, 0.5, ev);
		CHECK(ev.size() == 1 && ev[0].kind == CrowdEvent::Kind::Applause);
		Crowd nobody;
		CHECK(nobody.Mood() == 0.5);
	}

	void TestSynth()
	{
		Synth s(48000.0f);
		std::vector<float> buf(48000);
		s.NoteOn(Instrument::Lute, 57, 0.9f);
		s.Render(buf.data(), 4800);
		float peak = 0.0f;
		bool  finite = true;
		for (int i = 0; i < 4800; ++i) {
			peak = std::max(peak, std::abs(buf[i]));
			finite &= std::isfinite(buf[i]);
		}
		CHECK(finite && peak > 0.05f && peak <= 1.0f);
		// pitch: the autocorrelation's peak over a stretch of the ringing string (A3 = 220 Hz)
		s.Render(buf.data(), 48000);
		int    lag = 0;
		double bestAc = -1.0;
		for (int l = 100; l < 500; ++l) {
			double ac = 0.0;
			for (int i = 10000; i < 30000; ++i) {
				ac += buf[i] * buf[i + l];
			}
			if (ac > bestAc) {
				bestAc = ac;
				lag = l;
			}
		}
		const double hz = 48000.0 / lag;
		std::printf("lute A3: %.1f Hz\n", hz);
		CHECK(std::abs(hz - 220.0) < 2.0);
		// it dies away once let go
		s.NoteOff(Instrument::Lute, 57);
		for (int k = 0; k < 6; ++k) {
			s.Render(buf.data(), 48000);
		}
		CHECK(s.ActiveVoices() == 0);
		// the live flute is one voice that slurs between notes
		s.NoteOn(Instrument::Flute, 72, 0.8f);
		s.Render(buf.data(), 2400);
		s.NoteOn(Instrument::Flute, 74, 0.8f);
		s.Render(buf.data(), 2400);
		CHECK(s.ActiveVoices() == 1);
		s.NoteOff(Instrument::Flute, 74);
		s.Render(buf.data(), 2400);
		CHECK(s.ActiveVoices() == 1);  // back to the 72 still held
		s.NoteOff(Instrument::Flute, 72);
		s.Render(buf.data(), 48000);
		CHECK(s.ActiveVoices() == 0);
		// a big chord stays in range and voices are capped
		for (int p = 40; p < 100; ++p) {
			s.NoteOn(Instrument::Lute, p, 1.0f);
		}
		s.Coin(1.0f);
		s.Render(buf.data(), 9600);
		peak = 0.0f;
		for (int i = 0; i < 9600; ++i) {
			peak = std::max(peak, std::abs(buf[i]));
			finite &= std::isfinite(buf[i]);
		}
		CHECK(finite && peak <= 1.0f);
		CHECK(s.ActiveVoices() <= 40);
		s.Silence();
		s.Render(buf.data(), 16);
		CHECK(s.ActiveVoices() == 0);
	}
}

int main()
{
	TestMidi();
	TestMelodyFromChords();
	TestSongs();
	TestKeymap();
	TestPadmap();
	TestFreeJudge();
	TestChartJudge();
	TestCrowd();
	TestSynth();
	if (g_fail) {
		std::printf("%d check(s) failed\n", g_fail);
		return 1;
	}
	std::printf("all music tests passed\n");
	return 0;
}
