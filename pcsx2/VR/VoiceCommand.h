// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "common/Pcsx2Types.h"

#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace VR::VoiceCommand
{

	inline constexpr float kRejectBelow = 0.6f;
	inline constexpr float kAcceptFrom = 0.85f;
	inline constexpr u64 kCommandWindowMs = 5000;
	inline constexpr u64 kRateCapMs = 400;
	inline constexpr int kMaxAudiblesPerSnap = 2;

	inline constexpr u32 kPressTransactions = 2;
	inline constexpr u32 kModifierLeadTransactions = 2;
	inline constexpr u32 kModifierTailTransactions = 2;

	namespace Btn
	{
		inline constexpr u16 L2 = 1u << 0;
		inline constexpr u16 R2 = 1u << 1;
		inline constexpr u16 L1 = 1u << 2;
		inline constexpr u16 R1 = 1u << 3;
		inline constexpr u16 Triangle = 1u << 4;
		inline constexpr u16 Circle = 1u << 5;
		inline constexpr u16 Cross = 1u << 6;
		inline constexpr u16 Square = 1u << 7;
		inline constexpr u16 Select = 1u << 8;
		inline constexpr u16 L3 = 1u << 9;
		inline constexpr u16 R3 = 1u << 10;
		inline constexpr u16 Start = 1u << 11;
	}

	inline constexpr u16 kAudibleSlotButton[3] = {Btn::Square, Btn::Cross, Btn::Circle};

	struct Heard
	{
		std::string text;
		float confidence = 0.f;
	};

	class Recogniser
	{
	public:
		virtual ~Recogniser();
		virtual void SetVocabulary(const std::vector<std::string>& words) = 0;
		virtual std::optional<Heard> Next() = 0;
	};

	class ScriptedRecogniser final : public Recogniser
	{
	public:
		void Push(std::string text, float confidence);
		void SetVocabulary(const std::vector<std::string>& words) override;
		std::optional<Heard> Next() override;
		const std::vector<std::string>& Vocabulary() const { return m_vocabulary; }

	private:
		std::deque<Heard> m_queue;
		std::vector<std::string> m_vocabulary;
	};

	enum class Verb : u8
	{
		None = 0,
		Audible,
		HotRoute,
		Package,
		Substitution,
	};

	struct Command
	{
		Verb verb = Verb::None;
		int slot = 0;
		std::string name;
		u16 receiver = 0;
		int jerseyOff = -1;
		int jerseyOn = -1;

		bool operator==(const Command& o) const
		{
			return verb == o.verb && slot == o.slot && name == o.name && receiver == o.receiver &&
				   jerseyOff == o.jerseyOff && jerseyOn == o.jerseyOn;
		}
		bool operator!=(const Command& o) const { return !(*this == o); }

		std::string Describe() const;
	};

	struct AudiblePhrase
	{
		std::string phrase;
		int slot = 0;
	};

	struct HotRoutePhrase
	{
		std::string phrase;
		std::string route;
		u16 receiver = 0;
	};

	struct Vocabulary
	{
		std::vector<AudiblePhrase> audibles;
		std::vector<HotRoutePhrase> hotRoutes;
	};

	std::vector<std::string> Tokenise(std::string_view text);

	std::optional<int> ParseNumber(const std::vector<std::string>& tokens, size_t* pos);

	std::optional<Command> ParseOne(std::string_view text, const Vocabulary& vocab);

	struct Parsed
	{
		Command command;
		int count = 1;
	};
	std::optional<Parsed> Parse(std::string_view text, const Vocabulary& vocab);

	struct PressStep
	{
		u16 held = 0;
		u16 pressed = 0;
		u32 transactions = 0;
		u16 Down() const { return static_cast<u16>(held | pressed); }
		bool operator==(const PressStep& o) const
		{
			return held == o.held && pressed == o.pressed && transactions == o.transactions;
		}
	};

	struct MenuWalkRequest
	{
		Verb verb = Verb::None;
		bool routeKnown = false;
		std::string name;
		u16 receiver = 0;
		int jerseyOff = -1;
		int jerseyOn = -1;
	};

	struct PressPlan
	{
		Command command;
		std::vector<PressStep> steps;
		std::optional<MenuWalkRequest> walk;
	};

	std::vector<PressStep> AudiblePresses(int slot);

	PressPlan PlanFor(const Command& command);

	class MicArbiter
	{
	public:
		bool Claim(std::string_view owner);
		void Release(std::string_view owner);
		bool HeldBy(std::string_view owner) const;
		const std::string& Owner() const { return m_owner; }

	private:
		std::string m_owner;
#ifdef VR_VOICE_PLANTED_TWO_MIC_OWNERS
		std::vector<std::string> m_planted_owners;
#endif
	};

	inline constexpr std::string_view kMicOwnerName = "voiceCommand";

	struct Gates
	{
		std::function<bool()> micFree;
		std::function<bool()> playCallScreen;
		std::function<bool()> preSnap;
		std::function<bool()> notInPlay;
		std::function<bool(int off, int on)> rosterCheck;
		std::function<std::vector<std::string>()> packageList;
		std::function<bool()> pushToTalkHeld;
	};

	struct Config
	{
		Vocabulary vocabulary;
		bool pushToTalk = false;
	};

	enum class Outcome : u8
	{
		Ignored = 0,
		NotArmed,
		PushToTalkUp,
		LowConfidence,
		NoParse,
		Gated,
		NotInList,
		NotOnRoster,
		AudibleLimit,
		RateCapped,
		Pending,
		Mismatch,
		Confirmed,
		Rejected,
		Expired,
	};

	enum class Gate : u8
	{
		None = 0,
		MicFree,
		Screen,
		NotInPlay,
		Roster,
		PackageList,
	};

	struct Event
	{
		Outcome outcome = Outcome::Ignored;
		Gate gate = Gate::None;
		std::string card;
		std::optional<PressPlan> plan;
	};

	class Engine
	{
	public:
		Engine(Config config, Gates gates);
		~Engine();
		Engine(const Engine&) = delete;
		Engine& operator=(const Engine&) = delete;

		bool Arm(MicArbiter& arbiter);
		void Disarm();
		bool Armed() const { return m_arbiter != nullptr; }

		Event OnHeard(const Heard& heard, u64 nowMs);
		Event OnConfirm(bool nod, u64 nowMs);
		Event Tick(u64 nowMs);
		void OnNewSnap() { m_audibles_this_snap = 0; }

		std::vector<Event> Pump(Recogniser& recogniser, u64 nowMs);
		void RefreshVocabulary(Recogniser& recogniser) const;
		std::vector<std::string> VocabularyWords() const;

		bool HasPending() const { return m_pending.has_value(); }
		const std::optional<Command>& PendingCommand() const { return m_pending; }

	private:
		Gate CheckGates(const Command& c) const;
		Event Refusal(Gate g, const Command& c) const;
		Event Emit(const Command& c, u64 nowMs);
		bool NeedsConfirm(const Command& c, float confidence) const;
		void ExpireIfOld(u64 nowMs);

		Config m_config;
		Gates m_gates;
		MicArbiter* m_arbiter = nullptr;
		std::optional<Command> m_pending;
		u64 m_pending_at = 0;
		std::optional<u64> m_last_emit;
		int m_audibles_this_snap = 0;
	};
}
