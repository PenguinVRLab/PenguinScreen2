// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#include "VR/VoiceCommand.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace VR::VoiceCommand
{

	Recogniser::~Recogniser() = default;

	void ScriptedRecogniser::Push(std::string text, float confidence)
	{
		m_queue.push_back(Heard{std::move(text), confidence});
	}

	void ScriptedRecogniser::SetVocabulary(const std::vector<std::string>& words)
	{
		m_vocabulary = words;
	}

	std::optional<Heard> ScriptedRecogniser::Next()
	{
		if (m_queue.empty())
			return std::nullopt;
		Heard h = std::move(m_queue.front());
		m_queue.pop_front();
		return h;
	}

	namespace
	{
		constexpr const char* kUnits[20] = {"zero", "one", "two", "three", "four", "five", "six", "seven", "eight",
			"nine", "ten", "eleven", "twelve", "thirteen", "fourteen", "fifteen", "sixteen", "seventeen", "eighteen",
			"nineteen"};
		constexpr const char* kTens[10] = {"", "", "twenty", "thirty", "forty", "fifty", "sixty", "seventy", "eighty",
			"ninety"};

		int UnitWord(const std::string& w)
		{
			for (int i = 0; i < 20; i++)
				if (w == kUnits[i])
					return i;
			return -1;
		}

		int TensWord(const std::string& w)
		{
			for (int i = 2; i < 10; i++)
				if (w == kTens[i])
					return i * 10;
			return -1;
		}

		std::string Join(const std::vector<std::string>& t, size_t from, size_t to)
		{
			std::string s;
			for (size_t i = from; i < to; i++)
			{
				if (!s.empty())
					s += ' ';
				s += t[i];
			}
			return s;
		}

		std::string Normal(std::string_view s)
		{
			const std::vector<std::string> t = Tokenise(s);
			return Join(t, 0, t.size());
		}

		std::string Lower(std::string_view s)
		{
			std::string out(s);
			for (char& c : out)
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			return out;
		}

		bool ParseSubstitution(const std::vector<std::string>& t, Command* out)
		{
			size_t p = 0;
			auto word = [&](const char* w) {
				if (p < t.size() && t[p] == w)
				{
					p++;
					return true;
				}
				return false;
			};
			if (word("sub") && word("out"))
			{
				const std::optional<int> a = ParseNumber(t, &p);
				if (!a || !word("for"))
					return false;
				const std::optional<int> b = ParseNumber(t, &p);
				if (!b || p != t.size())
					return false;
				out->verb = Verb::Substitution;
				out->jerseyOff = *a;
				out->jerseyOn = *b;
				return true;
			}
			p = 0;
			word("put");
			const std::optional<int> b = ParseNumber(t, &p);
			if (!b || !word("in") || !word("for"))
				return false;
			const std::optional<int> a = ParseNumber(t, &p);
			if (!a || p != t.size())
				return false;
			out->verb = Verb::Substitution;
			out->jerseyOff = *a;
			out->jerseyOn = *b;
			return true;
		}
	}

	std::vector<std::string> Tokenise(std::string_view text)
	{
		std::string s = Lower(text);
		for (char& c : s)
			if (c == ',' || c == '.' || c == '!' || c == '?' || c == '-')
				c = ' ';
		std::vector<std::string> out;
		std::string cur;
		for (char c : s)
		{
			if (std::isspace(static_cast<unsigned char>(c)))
			{
				if (!cur.empty())
					out.push_back(std::move(cur));
				cur.clear();
			}
			else
				cur += c;
		}
		if (!cur.empty())
			out.push_back(std::move(cur));
		return out;
	}

	std::optional<int> ParseNumber(const std::vector<std::string>& t, size_t* pos)
	{
		size_t p = *pos;
		if (p >= t.size())
			return std::nullopt;
		const std::string& w = t[p];
		if (!w.empty() && w.size() <= 2 && std::all_of(w.begin(), w.end(), [](char c) { return c >= '0' && c <= '9'; }))
		{
			*pos = p + 1;
			return std::stoi(w);
		}
		if (const int u = UnitWord(w); u >= 0)
		{
			*pos = p + 1;
			return u;
		}
		if (const int tens = TensWord(w); tens >= 0)
		{
			p++;
			if (p < t.size())
			{
				const int u = UnitWord(t[p]);
				if (u >= 1 && u <= 9)
				{
					*pos = p + 1;
					return tens + u;
				}
			}
			*pos = p;
			return tens;
		}
		return std::nullopt;
	}

	std::string Command::Describe() const
	{
		switch (verb)
		{
			case Verb::Audible:
				return "audible " + std::to_string(slot);
			case Verb::HotRoute:
				return "hot route " + name;
			case Verb::Package:
				return name + " package";
			case Verb::Substitution:
				return "#" + std::to_string(jerseyOn) + " in for #" + std::to_string(jerseyOff);
			default:
				return "?";
		}
	}

	std::optional<Command> ParseOne(std::string_view text, const Vocabulary& vocab)
	{
		const std::vector<std::string> t = Tokenise(text);
		if (t.empty())
			return std::nullopt;
		const std::string joined = Join(t, 0, t.size());

		for (const AudiblePhrase& a : vocab.audibles)
		{
			if (a.slot >= 1 && a.slot <= 3 && Normal(a.phrase) == joined)
			{
				Command c;
				c.verb = Verb::Audible;
				c.slot = a.slot;
				return c;
			}
		}
		for (const HotRoutePhrase& h : vocab.hotRoutes)
		{
			if (Normal(h.phrase) == joined)
			{
				Command c;
				c.verb = Verb::HotRoute;
				c.name = Lower(h.route);
				c.receiver = h.receiver;
				return c;
			}
		}

		if (t.size() >= 2 && t.back() == "package")
		{
			bool ok = true;
			for (size_t i = 0; i + 1 < t.size(); i++)
				ok = ok && t[i] != "package";
			if (ok)
			{
				Command c;
				c.verb = Verb::Package;
				c.name = Join(t, 0, t.size() - 1);
				return c;
			}
		}

		Command c;
		if (ParseSubstitution(t, &c))
			return c;
		return std::nullopt;
	}

	std::optional<Parsed> Parse(std::string_view text, const Vocabulary& vocab)
	{
		const std::vector<std::string> t = Tokenise(text);
		for (size_t k = 1; k < t.size(); k++)
		{
			std::optional<Command> a = ParseOne(Join(t, 0, k), vocab);
			if (!a)
				continue;
			const std::optional<Command> b = ParseOne(Join(t, k, t.size()), vocab);
			if (b && *a == *b)
				return Parsed{std::move(*a), 2};
		}
		if (std::optional<Command> c = ParseOne(text, vocab))
			return Parsed{std::move(*c), 1};
		return std::nullopt;
	}

	std::vector<PressStep> AudiblePresses(int slot)
	{
		if (slot < 1 || slot > 3)
			return {};
		const u16 b = kAudibleSlotButton[slot - 1];
		return {
			PressStep{Btn::R1, 0, kModifierLeadTransactions},
			PressStep{Btn::R1, b, kPressTransactions},
			PressStep{Btn::R1, 0, kModifierTailTransactions},
		};
	}

	PressPlan PlanFor(const Command& c)
	{
		PressPlan p;
		p.command = c;
		switch (c.verb)
		{
			case Verb::Audible:
				p.steps = AudiblePresses(c.slot);
				break;
			case Verb::HotRoute:
			{
				MenuWalkRequest w;
				w.verb = Verb::HotRoute;
				w.name = c.name;
				w.receiver = c.receiver;
				p.walk = w;
				break;
			}
			case Verb::Package:
			{
				MenuWalkRequest w;
				w.verb = Verb::Package;
				w.name = c.name;
				p.walk = w;
				break;
			}
			case Verb::Substitution:
			{
				MenuWalkRequest w;
				w.verb = Verb::Substitution;
				w.jerseyOff = c.jerseyOff;
				w.jerseyOn = c.jerseyOn;
				p.walk = w;
				break;
			}
			default:
				break;
		}
		return p;
	}

	bool MicArbiter::Claim(std::string_view owner)
	{
		if (owner.empty())
			return false;
#ifdef VR_VOICE_PLANTED_TWO_MIC_OWNERS
		m_planted_owners.emplace_back(owner);
		if (m_owner.empty())
			m_owner = std::string(owner);
		return true;
#else
		if (m_owner.empty())
		{
			m_owner = std::string(owner);
			return true;
		}
		return m_owner == owner;
#endif
	}

	void MicArbiter::Release(std::string_view owner)
	{
		if (m_owner == owner)
			m_owner.clear();
	}

	bool MicArbiter::HeldBy(std::string_view owner) const
	{
#ifdef VR_VOICE_PLANTED_TWO_MIC_OWNERS
		for (const std::string& o : m_planted_owners)
			if (o == owner)
				return true;
#endif
		return !m_owner.empty() && m_owner == owner;
	}

	Engine::Engine(Config config, Gates gates)
		: m_config(std::move(config))
		, m_gates(std::move(gates))
	{
	}

	Engine::~Engine()
	{
		Disarm();
	}

	bool Engine::Arm(MicArbiter& arbiter)
	{
		if (m_arbiter == &arbiter)
			return true;
		Disarm();
		if (!arbiter.Claim(kMicOwnerName))
			return false;
		m_arbiter = &arbiter;
		return true;
	}

	void Engine::Disarm()
	{
		if (m_arbiter)
			m_arbiter->Release(kMicOwnerName);
		m_arbiter = nullptr;
		m_pending.reset();
	}

	namespace
	{
		bool Pass(const std::function<bool()>& f)
		{
			return f && f();
		}
	}

	Gate Engine::CheckGates(const Command& c) const
	{
		if (!Pass(m_gates.micFree))
			return Gate::MicFree;
#ifndef VR_VOICE_PLANTED_NO_SNAP_GATE
		if (!Pass(m_gates.notInPlay))
			return Gate::NotInPlay;
		switch (c.verb)
		{
			case Verb::Audible:
				if (!Pass(m_gates.playCallScreen) && !Pass(m_gates.preSnap))
					return Gate::Screen;
				break;
			case Verb::HotRoute:
				if (!Pass(m_gates.preSnap))
					return Gate::Screen;
				break;
			case Verb::Package:
			case Verb::Substitution:
				if (!Pass(m_gates.playCallScreen))
					return Gate::Screen;
				break;
			default:
				return Gate::Screen;
		}
#endif
		if (c.verb == Verb::Package)
		{
#ifndef VR_VOICE_PLANTED_ANY_PACKAGE
			if (!m_gates.packageList)
				return Gate::PackageList;
			const std::vector<std::string> list = m_gates.packageList();
			const bool offered = std::any_of(list.begin(), list.end(), [&](const std::string& n) {
				return Normal(n) == c.name;
			});
			if (!offered)
				return Gate::PackageList;
#endif
		}
		if (c.verb == Verb::Substitution)
		{
			if (!m_gates.rosterCheck || !m_gates.rosterCheck(c.jerseyOff, c.jerseyOn))
				return Gate::Roster;
		}
		return Gate::None;
	}

	Event Engine::Refusal(Gate g, const Command& c) const
	{
		Event e;
		e.gate = g;
		switch (g)
		{
			case Gate::PackageList:
				e.outcome = Outcome::NotInList;
				e.card = "heard: " + c.name + ", not in this list";
				break;
			case Gate::Roster:
				e.outcome = Outcome::NotOnRoster;
				e.card = "heard: " + c.Describe() + ", not on the roster";
				break;
			default:
				e.outcome = Outcome::Gated;
				e.card = "heard: " + c.Describe() + ", not now";
				break;
		}
		return e;
	}

	bool Engine::NeedsConfirm(const Command& c, float confidence) const
	{
#ifdef VR_VOICE_PLANTED_MIDBAND_GOES_OUT
		(void)c;
		(void)confidence;
		return false;
#else
		if (c.verb == Verb::Package || c.verb == Verb::Substitution)
			return true;
		return confidence < kAcceptFrom;
#endif
	}

	void Engine::ExpireIfOld(u64 nowMs)
	{
		if (m_pending && nowMs >= m_pending_at && nowMs - m_pending_at > kCommandWindowMs)
			m_pending.reset();
	}

	Event Engine::Emit(const Command& c, u64 nowMs)
	{
		m_pending.reset();
		if (const Gate g = CheckGates(c); g != Gate::None)
			return Refusal(g, c);
		if (m_last_emit && nowMs >= *m_last_emit && nowMs - *m_last_emit < kRateCapMs)
		{
			Event e;
			e.outcome = Outcome::RateCapped;
			e.card = "heard: " + c.Describe() + ", too soon";
			return e;
		}
		if (c.verb == Verb::Audible && m_audibles_this_snap >= kMaxAudiblesPerSnap)
		{
			Event e;
			e.outcome = Outcome::AudibleLimit;
			e.card = "heard: " + c.Describe() + ", no more audibles this play";
			return e;
		}
		if (c.verb == Verb::Audible)
			m_audibles_this_snap++;
		m_last_emit = nowMs;
		Event e;
		e.outcome = Outcome::Confirmed;
		e.card = "heard: " + c.Describe();
		e.plan = PlanFor(c);
		return e;
	}

	Event Engine::OnHeard(const Heard& heard, u64 nowMs)
	{
		ExpireIfOld(nowMs);
		Event e;
		if (!m_arbiter || !m_arbiter->HeldBy(kMicOwnerName))
		{
			e.outcome = Outcome::NotArmed;
			return e;
		}
		if (m_config.pushToTalk && !Pass(m_gates.pushToTalkHeld))
		{
			e.outcome = Outcome::PushToTalkUp;
			return e;
		}
		const float conf = std::isfinite(heard.confidence) ? heard.confidence : 0.f;
		if (conf < kRejectBelow)
		{
			e.outcome = Outcome::LowConfidence;
			return e;
		}
		const std::optional<Parsed> parsed = Parse(heard.text, m_config.vocabulary);
		if (!parsed)
		{
			e.outcome = Outcome::NoParse;
			e.card = "heard: " + Lower(heard.text) + ", not a command";
			return e;
		}
		const Command& c = parsed->command;
		if (const Gate g = CheckGates(c); g != Gate::None)
		{
			m_pending.reset();
			return Refusal(g, c);
		}

		if (m_pending)
		{
			if (*m_pending == c)
				return Emit(c, nowMs);
			m_pending = c;
			m_pending_at = nowMs;
			e.outcome = Outcome::Mismatch;
			e.card = "heard: " + c.Describe() + " (say it again or nod)";
			return e;
		}
		if (parsed->count >= 2 || !NeedsConfirm(c, conf))
			return Emit(c, nowMs);
		m_pending = c;
		m_pending_at = nowMs;
		e.outcome = Outcome::Pending;
		e.card = "heard: " + c.Describe() + " (say it again or nod)";
		return e;
	}

	Event Engine::OnConfirm(bool nod, u64 nowMs)
	{
		ExpireIfOld(nowMs);
		Event e;
		if (!m_pending)
			return e;
		if (!nod)
		{
			m_pending.reset();
			e.outcome = Outcome::Rejected;
			e.card = "";
			return e;
		}
		const Command c = *m_pending;
		return Emit(c, nowMs);
	}

	Event Engine::Tick(u64 nowMs)
	{
		Event e;
		if (m_pending && nowMs >= m_pending_at && nowMs - m_pending_at > kCommandWindowMs)
		{
			m_pending.reset();
			e.outcome = Outcome::Expired;
		}
		return e;
	}

	std::vector<Event> Engine::Pump(Recogniser& recogniser, u64 nowMs)
	{
		std::vector<Event> out;
		while (std::optional<Heard> h = recogniser.Next())
			out.push_back(OnHeard(*h, nowMs));
		return out;
	}

	std::vector<std::string> Engine::VocabularyWords() const
	{
		std::vector<std::string> w = {"sub", "out", "for", "in", "put", "package"};
		for (const char* u : kUnits)
			w.emplace_back(u);
		for (int i = 2; i < 10; i++)
			w.emplace_back(kTens[i]);
		for (const AudiblePhrase& a : m_config.vocabulary.audibles)
			for (std::string& t : Tokenise(a.phrase))
				w.push_back(std::move(t));
		for (const HotRoutePhrase& h : m_config.vocabulary.hotRoutes)
			for (std::string& t : Tokenise(h.phrase))
				w.push_back(std::move(t));
		if (m_gates.packageList)
			for (const std::string& n : m_gates.packageList())
				for (std::string& t : Tokenise(n))
					w.push_back(std::move(t));
		std::sort(w.begin(), w.end());
		w.erase(std::unique(w.begin(), w.end()), w.end());
		return w;
	}

	void Engine::RefreshVocabulary(Recogniser& recogniser) const
	{
		recogniser.SetVocabulary(VocabularyWords());
	}
}
