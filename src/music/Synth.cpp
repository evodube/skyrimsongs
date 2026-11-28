// SkyrimSongs - play the lute and flute in Skyrim.
// Copyright (C) 2026 evodube. GPL-3.0 (LICENSE.txt).
#include "Synth.h"
#include "Text.h"

#include <algorithm>
#include <cmath>

namespace music
{
	namespace
	{
		constexpr double kPi = 3.14159265358979323846;
		constexpr double kTau = 2.0 * kPi;

		float PitchHz(int a_pitch) { return 440.0f * std::pow(2.0f, (a_pitch - 69) / 12.0f); }

		// 0 at the instrument's lowest key, 1 at its highest (37 keys: lute C2-C5, flute C4-C7)
		float Register(int a_pitch, int a_low) { return std::clamp((a_pitch - a_low) / 36.0f, 0.0f, 1.0f); }

		// loudness by key (dB at MIDI notes 24, 30, 36 ... 108), measured with tests/voicing so every note a song
		// can play (C2-A7, the range of the songbook) comes out about as loud as the others
		constexpr float kLuteNorm[] = { -5.7f, -5.7f, -5.6f, -5.3f, -5.6f, -2.0f, -1.5f, 0.2f, 1.6f, 1.8f, 3.4f, 6.9f, 9.2f, 10.4f, 21.9f };
		constexpr float kFluteNorm[] = { 1.3f, 1.3f, 1.3f, 1.3f, 1.3f, 1.3f, 1.3f, 0.7f, 0.4f, 0.0f, -0.6f, -1.8f, -3.1f, -3.6f, -3.8f };
		float NormDb(const float* a_table, int a_pitch)
		{
			const float x = std::clamp((a_pitch - 24) / 6.0f, 0.0f, 13.999f);
			const int   i = static_cast<int>(x);
			return a_table[i] + (a_table[i + 1] - a_table[i]) * (x - i);
		}
		constexpr int kLuteLow = 36, kFluteLow = 60;
	}

	const char* InstrumentName(Instrument a_i)
	{
		switch (a_i) {
		case Instrument::Lute:
			return Tr("InstLute", "Lute");
		case Instrument::Flute:
			return Tr("InstFlute", "Flute");
		default:
			return "?";
		}
	}

	Synth::Synth(float a_rate) :
		m_rate(a_rate)
	{
		auto bandpass = [&](Biquad& b, float f, float q, float gain) {
			const double w = kTau * f / m_rate, al = std::sin(w) / (2.0 * q), a0 = 1.0 + al;
			b.b0 = static_cast<float>(al / a0 * gain);
			b.b1 = 0.0f;
			b.b2 = static_cast<float>(-al / a0 * gain);
			b.a1 = static_cast<float>(-2.0 * std::cos(w) / a0);
			b.a2 = static_cast<float>((1.0 - al) / a0);
		};
		// a lute's body: the air resonance, the top plate and a higher wood mode
		bandpass(m_body[0], 110.0f, 3.5f, 0.9f);
		bandpass(m_body[1], 245.0f, 4.0f, 0.6f);
		bandpass(m_body[2], 540.0f, 2.5f, 0.35f);
		{
			const double w = kTau * 5200.0 / m_rate, al = std::sin(w) / (2.0 * 0.707), a0 = 1.0 + al, c = std::cos(w);
			m_tone.b0 = static_cast<float>((1.0 - c) / 2.0 / a0);
			m_tone.b1 = static_cast<float>((1.0 - c) / a0);
			m_tone.b2 = m_tone.b0;
			m_tone.a1 = static_cast<float>(-2.0 * c / a0);
			m_tone.a2 = static_cast<float>((1.0 - al) / a0);
		}
		const float k = m_rate / 44100.0f;
		const int   combs[4] = { 1116, 1188, 1277, 1356 };
		const int   aps[2] = { 556, 441 };
		for (int i = 0; i < 4; ++i) {
			m_comb[i].buf.assign(static_cast<std::size_t>(combs[i] * k), 0.0f);
		}
		for (int i = 0; i < 2; ++i) {
			m_ap[i].buf.assign(static_cast<std::size_t>(aps[i] * k), 0.0f);
		}
		m_queue.reserve(256);
		m_work.reserve(256);
	}

	// ------------------------------------------------------------------------------------------------ events
	void Synth::NoteOn(Instrument a_inst, int a_pitch, float a_velocity, bool a_live)
	{
		std::lock_guard lk(m_mtx);
		m_queue.push_back({ Event::Kind::On, a_inst, a_live, std::clamp(a_pitch, 12, 120), std::clamp(a_velocity, 0.05f, 1.0f) });
	}
	void Synth::NoteOff(Instrument a_inst, int a_pitch, bool a_live)
	{
		std::lock_guard lk(m_mtx);
		m_queue.push_back({ Event::Kind::Off, a_inst, a_live, std::clamp(a_pitch, 12, 120), 0.0f });
	}
	void Synth::Sustain(bool a_on)
	{
		std::lock_guard lk(m_mtx);
		m_queue.push_back({ Event::Kind::Sustain, Instrument::Lute, true, 0, a_on ? 1.0f : 0.0f });
	}
	void Synth::AllOff()
	{
		std::lock_guard lk(m_mtx);
		m_queue.push_back({ Event::Kind::AllOff, Instrument::Lute, true, 0, 0.0f });
	}
	void Synth::Silence()
	{
		std::lock_guard lk(m_mtx);
		m_queue.push_back({ Event::Kind::Silence, Instrument::Lute, true, 0, 0.0f });
	}
	void Synth::Coin(float a_volume)
	{
		std::lock_guard lk(m_mtx);
		m_queue.push_back({ Event::Kind::Coin, Instrument::Lute, false, 0, a_volume });
	}
	void Synth::SetVolume(float a_v) { m_volume = std::clamp(a_v, 0.0f, 2.0f); }
	void Synth::SetReverb(float a_wet) { m_wet = std::clamp(a_wet, 0.0f, 1.0f); }

	float Synth::Noise()
	{
		m_rng ^= m_rng << 13;
		m_rng ^= m_rng >> 17;
		m_rng ^= m_rng << 5;
		return static_cast<float>(m_rng) / 2147483648.0f - 1.0f;
	}

	Synth::Voice* Synth::Allocate()
	{
		Voice* best = nullptr;
		for (auto& v : m_voices) {
			if (!v.on) {
				best = &v;
				break;
			}
		}
		if (!best) {
			// steal: the oldest note that's ringing out, else the oldest of all
			for (auto& v : m_voices) {
				if (v.coin) {
					continue;
				}
				if (!best || (v.released && !best->released) || (v.released == best->released && v.serial < best->serial)) {
					best = &v;
				}
			}
		}
		if (!best) {
			best = &m_voices[0];
		}
		*best = Voice{};
		best->on = true;
		best->serial = ++m_serial;
		return best;
	}

	void Synth::SetLuteDecay(Voice& a_v, float a_t60)
	{
		a_v.t60 = a_t60;
		for (int k = 0; k < 2; ++k) {
			const float f = m_rate / (a_v.s[k].len + a_v.s[k].damp + 0.5f);  // near enough for the decay
			a_v.s[k].gain = static_cast<float>(std::pow(10.0, -3.0 / (static_cast<double>(a_t60) * f)));
		}
	}

	void Synth::StartLute(Voice& a_v, int a_pitch, float a_vel)
	{
		a_v.inst = Instrument::Lute;
		a_v.pitch = a_pitch;
		a_v.vel = a_vel;
		a_v.freq = PitchHz(a_pitch);
		a_v.held = true;
		const float reg = Register(a_pitch, kLuteLow);
		// how the course is strung: bass in octaves, middle in unison pairs, the top a single string
		const bool   octave = a_pitch < 53;   // below F3
		const bool   single = a_pitch >= 67;  // G4 and up
		const double ratio2 = octave ? 2.0 : std::pow(2.0, 1.8 / 1200.0);
		const float  amp2 = single ? 0.0f : octave ? 0.42f : 0.38f;
		for (int k = 0; k < 2; ++k) {
			auto&        s = a_v.s[k];
			const double f = a_v.freq * (k == 0 ? 1.0 : ratio2);
			// the loop filter (1-damp, damp) delays by damp samples and loses highs every trip round: high strings go
			// round thousands of times a second, so they get a lighter one or they'd die almost at once
			s.damp = static_cast<float>(std::clamp(0.5 - 0.4 * (f - 500.0) / 2500.0, 0.1, 0.5));
			const double d = std::clamp(m_rate / f - s.damp, 2.2, static_cast<double>(kMaxDelay - 2));
			s.len = static_cast<int>(d - 0.1);
			const double frac = d - s.len;
			s.apC = static_cast<float>((1.0 - frac) / (1.0 + frac));
			s.apX = s.apY = s.prev = 0.0f;
			s.idx = 0;
			const float amp = k == 0 ? 0.62f : amp2;
			if (amp <= 0.0f) {
				std::fill(s.buf.begin(), s.buf.begin() + s.len, 0.0f);
				continue;
			}
			// the pluck: noise, darker for a soft touch and for the bass, with the harmonics a pluck at that point
			// on the string leaves out (nearer the bridge as the notes go up)
			const float a = std::clamp((0.12f + 0.55f * a_vel) * (0.55f + 0.7f * reg) * (1.0f + 0.05f * m_quality[0]), 0.05f, 0.95f);
			float       y1 = 0.0f, y2 = 0.0f;
			for (int i = 0; i < s.len; ++i) {
				y1 += a * (Noise() - y1);
				y2 += a * (y1 - y2);
				s.buf[i] = y2;
			}
			const int m = (std::max)(1, static_cast<int>(s.len * (0.21f - 0.09f * reg)));
			std::array<float, kMaxDelay> tmp{};
			for (int i = 0; i < s.len; ++i) {
				tmp[i] = s.buf[i] - s.buf[(i - m + s.len) % s.len];
			}
			double mean = 0.0;
			float  peak = 1e-6f;
			for (int i = 0; i < s.len; ++i) {
				mean += tmp[i];
			}
			mean /= s.len;
			for (int i = 0; i < s.len; ++i) {
				tmp[i] -= static_cast<float>(mean);
				peak = (std::max)(peak, std::abs(tmp[i]));
			}
			const float g = (0.25f + 0.75f * a_vel) * amp / peak;
			for (int i = 0; i < s.len; ++i) {
				s.buf[i] = tmp[i] * g;
			}
		}
		// the finger's click
		a_v.chiff = 0.10f * a_vel * (0.6f + 0.6f * reg);
		// bass rings long, treble short
		const float t60 = std::clamp(7.0f * std::pow(65.4f / a_v.freq, 0.45f), 1.2f, 8.0f) * (0.8f + 0.2f * a_vel) * (1.0f + 0.2f * m_quality[0]);
		SetLuteDecay(a_v, t60);
		a_v.norm = std::pow(10.0f, NormDb(kLuteNorm, a_pitch) / 20.0f);
	}

	void Synth::Apply(const Event& e)
	{
		switch (e.kind) {
		case Event::Kind::On:
			if (e.inst == Instrument::Lute) {
				// re-plucking a string that's still ringing stops it first
				for (auto& v : m_voices) {
					if (v.on && !v.coin && v.inst == Instrument::Lute && v.pitch == e.pitch && v.live == e.live) {
						v.released = true;
						v.held = false;
						SetLuteDecay(v, 0.06f);
					}
				}
				auto* v = Allocate();
				v->live = e.live;
				StartLute(*v, e.pitch, e.value);
			} else {
				if (e.live) {
					std::erase(m_fluteHeld, e.pitch);
					m_fluteHeld.push_back(e.pitch);
					for (auto& v : m_voices) {
						if (v.on && !v.coin && v.inst == Instrument::Flute && v.live && !v.released) {
							// slur to the new note: no new breath, a light tongue
							v.pitch = e.pitch;
							v.targetFreq = PitchHz(e.pitch);
							v.vel = e.value;
							v.norm = std::pow(10.0f, NormDb(kFluteNorm, e.pitch) / 20.0f);
							v.held = true;
							v.chiff = (std::max)(v.chiff, 0.18f * e.value);
							return;
						}
					}
				}
				auto* v = Allocate();
				v->inst = Instrument::Flute;
				v->live = e.live;
				v->pitch = e.pitch;
				v->vel = e.value;
				v->held = true;
				v->curFreq = v->targetFreq = PitchHz(e.pitch);
				v->chiff = 0.5f * e.value * (1.0f - 0.5f * Register(e.pitch, kFluteLow));
				v->norm = std::pow(10.0f, NormDb(kFluteNorm, e.pitch) / 20.0f);
				v->phase = 0.0;
			}
			break;
		case Event::Kind::Off:
			if (e.inst == Instrument::Flute && e.live) {
				std::erase(m_fluteHeld, e.pitch);
				for (auto& v : m_voices) {
					if (v.on && !v.coin && v.inst == Instrument::Flute && v.live && !v.released && v.pitch == e.pitch) {
						if (!m_fluteHeld.empty()) {
							v.pitch = m_fluteHeld.back();
							v.targetFreq = PitchHz(v.pitch);
							v.norm = std::pow(10.0f, NormDb(kFluteNorm, v.pitch) / 20.0f);
						} else {
							v.held = false;
							v.released = !m_sustain;
						}
					}
				}
				break;
			}
			for (auto& v : m_voices) {
				if (v.on && !v.coin && v.inst == e.inst && v.live == e.live && v.pitch == e.pitch && v.held) {
					v.held = false;
					if (!m_sustain) {
						v.released = true;
						if (v.inst == Instrument::Lute) {
							SetLuteDecay(v, (std::min)(v.t60, 0.9f));
						}
					}
				}
			}
			break;
		case Event::Kind::Sustain:
			m_sustain = e.value > 0.5f;
			if (!m_sustain) {
				for (auto& v : m_voices) {
					if (v.on && !v.coin && !v.held && !v.released) {
						v.released = true;
						if (v.inst == Instrument::Lute) {
							SetLuteDecay(v, (std::min)(v.t60, 0.9f));
						}
					}
				}
			}
			break;
		case Event::Kind::AllOff:
			m_fluteHeld.clear();
			for (auto& v : m_voices) {
				if (v.on && !v.coin && !v.released) {
					v.released = true;
					v.held = false;
					if (v.inst == Instrument::Lute) {
						SetLuteDecay(v, (std::min)(v.t60, 0.9f));
					}
				}
			}
			break;
		case Event::Kind::Silence:
			m_fluteHeld.clear();
			for (auto& v : m_voices) {
				v.on = false;
			}
			break;
		case Event::Kind::Coin:
			{
				auto* v = Allocate();
				v->coin = true;
				v->coinVol = e.value;
				for (auto& p : v->phases) {
					p = (Noise() + 1.0f) * kPi;
				}
			}
			break;
		}
	}

	// ------------------------------------------------------------------------------------------------ voices
	float Synth::LuteSample(Voice& v)
	{
		float out = 0.0f;
		for (int k = 0; k < 2; ++k) {
			auto&       s = v.s[k];
			const float y = s.buf[s.idx];
			const float lp = (1.0f - s.damp) * y + s.damp * s.prev;
			s.prev = y;
			const float ap = s.apC * lp + s.apX - s.apC * s.apY;
			s.apX = lp;
			s.apY = ap;
			s.buf[s.idx] = ap * s.gain;
			if (++s.idx >= s.len) {
				s.idx = 0;
			}
			out += y;
		}
		if (v.chiff > 1e-4f) {
			out += Noise() * v.chiff;
			v.chiff *= 0.9948f;  // ~4 ms at 48 kHz
		}
		return out;
	}

	float Synth::FluteSample(Voice& v)
	{
		const double dt = 1.0 / m_rate;
		v.age += dt;
		const float glide = 1.0f - std::exp(static_cast<float>(-dt / 0.035));
		v.curFreq += (v.targetFreq - v.curFreq) * glide;
		// where in the flute's range this is: 0 low register (C4) .. 1 top (C7); below C4, "deep"
		const float reg = std::clamp(std::log2(v.curFreq / 261.63f) / 3.0f, 0.0f, 1.0f);
		const float lo = 1.0f - reg;
		const float deep = std::clamp((261.63f - v.curFreq) / (261.63f - 65.41f), 0.0f, 1.0f);

		// the breath: rises (and falls) with a little overshoot-free swell
		const float target = v.released ? 0.0f : (0.55f + 0.45f * v.vel);
		const float tau = v.released ? 0.08f : (0.06f - 0.03f * reg);
		v.env += (target - v.env) * (1.0f - std::exp(static_cast<float>(-dt / tau)));
		const float breath = v.env / (0.55f + 0.45f * (std::max)(v.vel, 0.05f));  // 0..1 of this note's full breath

		// a player is never quite steady: slow random wander (a few Hz) in pitch and loudness
		const float q = static_cast<float>(m_quality[1]);
		const float steady = 1.0f - 0.3f * q;  // a finer flute: a steadier tone
		v.jitP += (Noise() - v.jitP) * 0.0004f;
		v.jitA += (Noise() - v.jitA) * 0.0006f;
		const float scoop = 1.0f - 0.012f * std::exp(static_cast<float>(-v.age / 0.06));
		const float bloom = std::clamp(static_cast<float>((v.age - 0.35) / 0.7), 0.0f, 1.0f);
		const float rate = 5.2f + 0.5f * reg;
		const float vib = 0.0045f * (1.0f + 0.12f * q) * bloom * static_cast<float>(std::sin(kTau * rate * v.age));
		const float f = v.curFreq * scoop * (1.0f + vib + 0.0025f * steady * v.jitP);
		v.phase += f * dt;
		v.phase -= std::floor(v.phase);
		const double p = kTau * v.phase;

		// the tone: harmonics fill in as the breath does (a soft note is nearly pure), the low register richer
		const float b2 = breath * breath, b3 = b2 * breath;
		const float h1 = 1.0f - 0.7f * deep;
		const float h2 = (0.10f + 0.30f * lo + 0.30f * deep) * b2 * (1.0f + 0.12f * q);
		const float h3 = (0.04f + 0.14f * lo + 0.18f * deep) * b3;
		const float h4 = (0.05f * lo + 0.05f * deep) * b3;
		const float tone = static_cast<float>(h1 * std::sin(p) + h2 * std::sin(2.0 * p + 0.6) + h3 * std::sin(3.0 * p + 1.3) + h4 * std::sin(4.0 * p + 0.2));

		// the air: noise resonating in the tube at the first harmonics (narrow band-passes), and a high hiss
		const float n = Noise();
		float       air = 0.0f;
		for (int k = 0; k < 3; ++k) {
			const float fk = (std::min)(f * (k + 1), 12000.0f);
			const float c = 2.0f * static_cast<float>(std::sin(kPi * fk / m_rate));
			const float q = 0.03f;  // narrow: a breath of pitch, not noise
			auto&       st = v.rz[k];
			const float hp = n - st[0] - q * st[1];
			st[1] += c * hp;
			st[0] += c * st[1];
			air += st[1] * (k == 0 ? 1.0f : k == 1 ? 0.6f : 0.35f);
		}
		// (no broadband hiss: it read as white noise behind every note)
		const float airLevel = (0.008f + 0.012f * lo + 0.015f * deep) * (1.0f - 0.2f * q);

		// the chiff: a puff of air at the start of each note (and lighter on a slur)
		v.chiff *= std::exp(static_cast<float>(-dt / 0.035));

		const float level = v.env * (1.0f + 0.04f * steady * v.jitA) * (1.0f - 0.3f * deep);
		const float trem = 1.0f + 0.025f * bloom * static_cast<float>(std::sin(kTau * rate * v.age + 0.6));
		// the chiff: the same tuned air, briefly louder at the start of a note
		const float out = tone * level * trem + air * (airLevel * v.env + v.chiff * 0.05f);
		return out * 0.44f * v.norm;  // (0.44: level with the lute)
	}

	float Synth::CoinSample(Voice& v)
	{
		static constexpr float kF[6] = { 2350.0f, 3810.0f, 5530.0f, 7210.0f, 2520.0f, 4100.0f };
		static constexpr float kT[6] = { 0.25f, 0.16f, 0.10f, 0.07f, 0.22f, 0.12f };
		static constexpr float kA[6] = { 1.0f, 0.7f, 0.45f, 0.3f, 0.6f, 0.4f };
		const double dt = 1.0 / m_rate;
		v.age += dt;
		float out = 0.0f;
		for (int i = 0; i < 6; ++i) {
			const double t = i < 4 ? v.age : v.age - 0.075;  // a second, lighter bounce
			if (t < 0.0) {
				continue;
			}
			v.phases[i] += kTau * kF[i] * dt;
			out += kA[i] * std::exp(static_cast<float>(-t / kT[i])) * static_cast<float>(std::sin(v.phases[i]));
		}
		if (v.age > 0.9) {
			v.on = false;
		}
		return out * 0.12f * v.coinVol;
	}

	float Synth::Reverb(float a_in)
	{
		float out = 0.0f;
		for (auto& c : m_comb) {
			const float y = c.buf[c.idx];
			c.store = y * 0.7f + c.store * 0.3f;
			c.buf[c.idx] = a_in + c.store * 0.78f;
			if (++c.idx >= static_cast<int>(c.buf.size())) {
				c.idx = 0;
			}
			out += y;
		}
		out *= 0.25f;
		for (auto& a : m_ap) {
			const float b = a.buf[a.idx];
			const float y = -out + b;
			a.buf[a.idx] = out + b * 0.5f;
			if (++a.idx >= static_cast<int>(a.buf.size())) {
				a.idx = 0;
			}
			out = y;
		}
		return out;
	}

	void Synth::Render(float* a_out, int a_frames)
	{
		{
			std::lock_guard lk(m_mtx);
			m_work.swap(m_queue);
		}
		for (const auto& e : m_work) {
			Apply(e);
		}
		m_work.clear();

		const double dt = 1.0 / m_rate;
		for (int i = 0; i < a_frames; ++i) {
			float lute = 0.0f, flute = 0.0f, coin = 0.0f;
			for (auto& v : m_voices) {
				if (!v.on) {
					continue;
				}
				float s = 0.0f;
				if (v.coin) {
					coin += CoinSample(v);
					continue;
				}
				if (v.inst == Instrument::Lute) {
					v.age += dt;
					s = LuteSample(v) * v.norm;
					lute += s;
				} else {
					s = FluteSample(v);
					flute += s;
				}
				v.level = v.level * 0.999f + std::abs(s) * 0.001f;
				if (v.age > 0.15 && v.level < 2e-5f && (v.released || v.inst == Instrument::Lute)) {
					v.on = false;
				}
			}
			// the lute through its body
			const float dc = lute - m_dcX + 0.995f * m_dcY;
			m_dcX = lute;
			m_dcY = dc;
			float body = dc * 0.7f;
			const float fuller = 1.0f + 0.3f * m_quality[0];  // a finer lute's body sings more
			for (auto& b : m_body) {
				body += b.Run(dc) * fuller;
			}
			body = m_tone.Run(body) * 3.2f;
			// the flute through a gentle high-pass (~160 Hz): a flute has no bass, so nothing boomy gets through
			const float fhp = 0.979f * (m_fluteHpY + flute - m_fluteHpX);
			m_fluteHpX = flute;
			m_fluteHpY = fhp;
			const float dry = body + fhp * 0.8f + coin;
			const float wet = Reverb(dry) * m_wet;
			// a limiter (instant attack, 120 ms release) keeps loud chords under the soft-clip, which on its own
			// crackled on dense passages; the tanh is only a last safety
			const float x = (dry + wet) * m_volume * 0.6f;
			const float ax = std::abs(x);
			m_limEnv = ax > m_limEnv ? ax : m_limEnv * 0.99983f;
			const float g = m_limEnv > 0.8f ? 0.8f / m_limEnv : 1.0f;
			a_out[i] = std::tanh(x * g);
		}
		int n = 0;
		for (const auto& v : m_voices) {
			n += v.on ? 1 : 0;
		}
		m_active = n;
	}
}
