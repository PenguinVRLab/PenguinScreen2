// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "common/Pcsx2Types.h"

#include <mutex>

namespace VR::VoiceCommand
{
	struct Event;
	struct PressPlan;
}

namespace VR::VoiceGlue
{
	enum class SinkState : u8
	{
		Idle = 0,
		Waiting,
		Pressing,
		Done,
		Cancelled,
	};

	struct SinkReport
	{
		u32 id = 0;
		SinkState state = SinkState::Idle;
		u32 delivered = 0;
	};

	class PulseSink
	{
	public:
		virtual ~PulseSink();
		virtual bool Arm(u16 buttons, u32 count, u32* id) = 0;
		virtual SinkReport Report() const = 0;
		virtual void Cancel() = 0;
	};

	inline constexpr size_t kMaxSteps = 8;

	enum class Result : u8
	{
		Started = 0,
		NotConfirmed,
		WalkNotPressed,
		EmptyPlan,
		BadStep,
		Busy,
		ArmRefused,
	};

	enum class Phase : u8
	{
		Idle = 0,
		Running,
		Finished,
		Aborted,
	};

	const char* ResultName(Result r);
	const char* PhaseName(Phase p);

	class Player
	{
	public:
		explicit Player(PulseSink& sink);

		Result Submit(const VoiceCommand::Event& event);
		void Tick();
		void Cancel();

		Phase GetPhase() const;
		u32 StepsDone() const;

	private:
		bool ArmStep(size_t index);

		mutable std::mutex m_lock;
		PulseSink& m_sink;
		Phase m_phase = Phase::Idle;
		u16 m_buttons[kMaxSteps] = {};
		u32 m_counts[kMaxSteps] = {};
		size_t m_step_count = 0;
		size_t m_step = 0;
		u32 m_armed_id = 0;
		u32 m_steps_done = 0;
	};

}
