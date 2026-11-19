// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "VR/XRFrameHealth.h"

#include "common/Pcsx2Defs.h"

#include <openxr/openxr.h>

namespace VR::SwapchainWait
{
	enum class Outcome
	{
		Ready,
		TimedOut,
		Failed,
	};

	constexpr Outcome Classify(XrResult res)
	{
#ifdef VR_FORKBUGS_PLANTED_TIMEOUT_AS_READY
		return XR_FAILED(res) ? Outcome::Failed : Outcome::Ready;
#else
		if (res == XR_TIMEOUT_EXPIRED)
			return Outcome::TimedOut;
		return XR_SUCCEEDED(res) ? Outcome::Ready : Outcome::Failed;
#endif
	}

	constexpr XrDuration kPresentWaitBudgetNs = 4'000'000;

	constexpr u32 kMaxWaitFailures = 30;

	class Budget
	{
	public:
		explicit constexpr Budget(XrDuration total) : m_left(total) {}

		constexpr XrDuration Next() const
		{
#ifdef VR_FORKBUGS_PLANTED_WAITBUDGET
			return 1'000'000'000;
#else
			return m_left > 0 ? m_left : 0;
#endif
		}

		constexpr void Spend(XrDuration ns) { m_left -= (ns > 0 ? ns : 0); }

	private:
		XrDuration m_left;
	};

	enum class Action
	{
		Use,
		Hold,
		Reacquire,
		Lost,
	};

	struct FailureTracker
	{
		u32 failures = 0;
	};

	inline Action OnWait(FailureTracker& t, XrResult res)
	{
		switch (Classify(res))
		{
			case Outcome::Ready:
				t.failures = 0;
				return Action::Use;
			case Outcome::TimedOut:
				return Action::Hold;
			case Outcome::Failed:
			default:
#ifdef VR_FORKBUGS_PLANTED_WAITRETRY
				return Action::Hold;
#else
				if (XRFrameHealth::IsTerminal(res))
					return Action::Lost;
				if (res == XR_ERROR_CALL_ORDER_INVALID || ++t.failures >= kMaxWaitFailures)
				{
					t.failures = 0;
					return Action::Reacquire;
				}
				return Action::Hold;
#endif
		}
	}

	struct PairPlan
	{
		bool copy[2] = {false, false};
	};

	constexpr PairPlan PlanEyePair(bool left_ready, bool right_ready)
	{
		PairPlan p;
#ifdef VR_FORKBUGS_PLANTED_EYEPAIR
		p.copy[0] = left_ready;
		p.copy[1] = right_ready;
#else
		p.copy[0] = p.copy[1] = left_ready && right_ready;
#endif
		return p;
	}
}
