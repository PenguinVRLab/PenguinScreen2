// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#include "VR/VoiceGlue.h"

#include "VR/VoiceCommand.h"


namespace VR::VoiceGlue
{
	PulseSink::~PulseSink() = default;

	const char* ResultName(Result r)
	{
		switch (r)
		{
			case Result::Started: return "started";
			case Result::NotConfirmed: return "not confirmed";
			case Result::WalkNotPressed: return "walk not pressed";
			case Result::EmptyPlan: return "empty plan";
			case Result::BadStep: return "bad step";
			case Result::Busy: return "busy";
			case Result::ArmRefused: return "arm refused";
		}
		return "?";
	}

	const char* PhaseName(Phase p)
	{
		switch (p)
		{
			case Phase::Idle: return "idle";
			case Phase::Running: return "running";
			case Phase::Finished: return "finished";
			case Phase::Aborted: return "aborted";
		}
		return "?";
	}

	Player::Player(PulseSink& sink)
		: m_sink(sink)
	{
	}

	bool Player::ArmStep(size_t index)
	{
		u32 id = 0;
		if (!m_sink.Arm(m_buttons[index], m_counts[index], &id))
			return false;
		m_step = index;
		m_armed_id = id;
		return true;
	}

	Result Player::Submit(const VoiceCommand::Event& event)
	{
		std::lock_guard<std::mutex> guard(m_lock);
		if (event.outcome != VoiceCommand::Outcome::Confirmed || !event.plan)
			return Result::NotConfirmed;
		const VoiceCommand::PressPlan& plan = *event.plan;
		if (m_phase == Phase::Running)
			return Result::Busy;

		if (plan.walk)
		{
#ifdef VR_VOICE_PLANTED_WALK_PRESSES
			if (plan.walk->verb == VoiceCommand::Verb::HotRoute && plan.walk->receiver != 0)
			{
				m_buttons[0] = plan.walk->receiver;
				m_counts[0] = VoiceCommand::kPressTransactions;
				m_step_count = 1;
				m_steps_done = 0;
				if (!ArmStep(0))
					return Result::ArmRefused;
				m_phase = Phase::Running;
				return Result::Started;
			}
#endif
			return Result::WalkNotPressed;
		}
		if (plan.steps.empty())
			return Result::EmptyPlan;
		if (plan.steps.size() > kMaxSteps)
			return Result::BadStep;
		for (const VoiceCommand::PressStep& s : plan.steps)
		{
			if (s.transactions == 0 || s.Down() == 0)
				return Result::BadStep;
		}

		m_step_count = plan.steps.size();
		for (size_t i = 0; i < m_step_count; i++)
		{
			m_buttons[i] = plan.steps[i].Down();
			m_counts[i] = plan.steps[i].transactions;
		}
		m_steps_done = 0;
		if (!ArmStep(0))
			return Result::ArmRefused;
		m_phase = Phase::Running;
		return Result::Started;
	}

	void Player::Tick()
	{
		std::lock_guard<std::mutex> guard(m_lock);
		if (m_phase != Phase::Running)
			return;
		const SinkReport r = m_sink.Report();
		if (r.id != m_armed_id)
		{
			m_phase = Phase::Aborted;
			return;
		}
		if (r.state == SinkState::Cancelled)
		{
			m_phase = Phase::Aborted;
			return;
		}
		if (r.state != SinkState::Done)
			return;
		m_steps_done++;
#ifdef VR_VOICE_PLANTED_NO_CHAIN
		m_phase = Phase::Finished;
		return;
#else
		if (m_step + 1 >= m_step_count)
		{
			m_phase = Phase::Finished;
			return;
		}
		if (!ArmStep(m_step + 1))
			m_phase = Phase::Aborted;
#endif
	}

	void Player::Cancel()
	{
		std::lock_guard<std::mutex> guard(m_lock);
		if (m_phase != Phase::Running)
			return;
		const SinkReport r = m_sink.Report();
		if (r.id == m_armed_id && (r.state == SinkState::Waiting || r.state == SinkState::Pressing))
			m_sink.Cancel();
		m_phase = Phase::Aborted;
	}

	Phase Player::GetPhase() const
	{
		std::lock_guard<std::mutex> guard(m_lock);
		return m_phase;
	}

	u32 Player::StepsDone() const
	{
		std::lock_guard<std::mutex> guard(m_lock);
		return m_steps_done;
	}

}
