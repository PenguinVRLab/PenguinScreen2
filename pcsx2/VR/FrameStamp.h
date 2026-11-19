// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0
// (x-wave-salvage Source/Core/VR/VRFrameStamp.h, "Copyright 2026 Dolphin Emulator Project",

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace VR
{
	template <typename Pose>
	struct FrameStamp
	{
		std::uint64_t latch_seq = 0;
		std::int64_t display_time = 0;
		Pose pose{};
	};

	enum class StampSource : int
	{
		None = 0,
		Keyed = 1,
		LastDeclared = 2,
		TooOld = 3,
	};

	struct LatchDecision
	{
		bool latched = false;
		bool begin_xr_frame = false;
		bool discards_open_xr_frame = false;
	};

	enum class CloseOutcome : int
	{
		Stamped = 0,
		NoOpenLatch = 1,
	};

	template <typename Pose>
	struct PresentDecision
	{
		StampSource source = StampSource::None;
		FrameStamp<Pose> stamp{};
		std::uint64_t age = 0;
		bool begin_xr_frame = false;

		bool Declares() const { return source == StampSource::Keyed || source == StampSource::LastDeclared; }
	};

	enum class XrFrameOnInvalidate : int
	{
		Keep = 0,
		Drop = 1,
	};

	template <typename Pose>
	class FrameStampMachine
	{
	public:
		static constexpr std::size_t kKeySlots = 8;
		static constexpr std::uint64_t kDefaultMaxAge = 3;
		static constexpr std::uint64_t kNoAgeLimit = std::numeric_limits<std::uint64_t>::max();

		explicit FrameStampMachine(std::uint64_t max_age = kDefaultMaxAge)
			: m_max_age(max_age)
		{
		}

		void SetMaxAge(std::uint64_t max_age) { m_max_age = max_age; }
		std::uint64_t MaxAge() const { return m_max_age; }

		LatchDecision OnLatch(const Pose& pose, std::int64_t display_time, bool paced_frame_ready)
		{
			TakeRequest();
			LatchDecision decision;
			if (!m_enabled || m_latch_open)
				return decision;
#ifdef VR_FRAMESTAMP_PLANTED_DEDUPE
			if (m_seq != 0 && pose == m_latch.pose)
				return decision;
#endif

			m_latch = FrameStamp<Pose>{++m_seq, display_time, pose};
			m_latch_open = true;
			decision.latched = true;

			if (paced_frame_ready)
			{
				decision.begin_xr_frame = true;
				decision.discards_open_xr_frame = m_xr_frame_open;
				m_xr_frame_open = true;
			}
			return decision;
		}

		CloseOutcome OnClose(std::uint64_t key, bool closes_frame = true)
		{
			TakeRequest();
			if (!m_latch_open)
			{
				Forget(key);
				return CloseOutcome::NoOpenLatch;
			}
			Store(key, m_latch);
			if (closes_frame)
				m_latch_open = false;
			return CloseOutcome::Stamped;
		}

		PresentDecision<Pose> OnPresent(std::uint64_t key)
		{
			TakeRequest();
			PresentDecision<Pose> decision;
			if (m_enabled)
			{
#ifdef VR_FRAMESTAMP_PLANTED_LIVELATCH
				if (m_latch_open)
				{
					decision.source = StampSource::Keyed;
					decision.stamp = m_latch;
				}
				else
#endif
				if (const Slot* slot = Find(key))
				{
					decision.source = StampSource::Keyed;
					decision.stamp = slot->stamp;
				}
				else if (m_last_declared.latch_seq != 0)
				{
					decision.source = StampSource::LastDeclared;
					decision.stamp = m_last_declared;
				}
			}

			if (decision.source != StampSource::None)
			{
				decision.age = AgeOf(decision.stamp);
				if (IsFresh(decision.stamp))
					m_last_declared = decision.stamp;
				else
				{
					decision.source = StampSource::TooOld;
#ifdef VR_FRAMESTAMP_PLANTED_TOOOLDLAST
					m_last_declared = decision.stamp;
#endif
				}
			}

			decision.begin_xr_frame = !m_xr_frame_open;
			m_xr_frame_open = false;
			return decision;
		}

		void Invalidate(XrFrameOnInvalidate xr = XrFrameOnInvalidate::Keep)
		{
			for (Slot& slot : m_slots)
				slot = Slot{};
			m_latch_open = false;
			m_last_declared = FrameStamp<Pose>{};
			m_floor = m_seq;
#ifdef VR_FRAMESTAMP_PLANTED_SEQRESET
			m_seq = 0;
			m_floor = 0;
#endif
			if (xr == XrFrameOnInvalidate::Drop)
			{
#ifndef VR_FRAMESTAMP_PLANTED_KEEPXR
				m_xr_frame_open = false;
#endif
			}
		}

		void RequestInvalidate(XrFrameOnInvalidate xr = XrFrameOnInvalidate::Keep)
		{
			m_request.fetch_or(kRequestInvalidate | (xr == XrFrameOnInvalidate::Drop ? kRequestDropXr : 0u),
				std::memory_order_acq_rel);
		}

		void SetEnabled(bool on)
		{
			if (m_enabled && !on)
			{
#ifndef VR_FRAMESTAMP_PLANTED_NOFORGET
				Invalidate(XrFrameOnInvalidate::Keep);
#endif
			}
			m_enabled = on;
		}
		bool IsEnabled() const { return m_enabled; }

		std::uint64_t AgeOf(const FrameStamp<Pose>& stamp) const
		{
			return stamp.latch_seq <= m_seq ? m_seq - stamp.latch_seq : 0;
		}

		bool IsFresh(const FrameStamp<Pose>& stamp) const
		{
			if (stamp.latch_seq == 0 || stamp.latch_seq <= m_floor || stamp.latch_seq > m_seq)
				return false;
#ifdef VR_FRAMESTAMP_PLANTED_NOAGE
			return true;
#else
			return m_seq - stamp.latch_seq <= m_max_age;
#endif
		}

		void DropLatch() { m_latch_open = false; }

		std::uint64_t NewestSeq() const { return m_seq; }
		bool IsLatchOpen() const { return m_latch_open; }
		const FrameStamp<Pose>& CurrentLatch() const { return m_latch; }
		bool IsXrFrameOpen() const { return m_xr_frame_open; }

	private:
		static constexpr unsigned kRequestInvalidate = 1u;
		static constexpr unsigned kRequestDropXr = 2u;

		struct Slot
		{
			bool used = false;
			std::uint64_t key = 0;
			FrameStamp<Pose> stamp{};
		};

		void TakeRequest()
		{
			if (m_request.load(std::memory_order_relaxed) == 0)
				return;
			const unsigned request = m_request.exchange(0, std::memory_order_acq_rel);
			if (request & kRequestInvalidate)
				Invalidate((request & kRequestDropXr) ? XrFrameOnInvalidate::Drop : XrFrameOnInvalidate::Keep);
		}

		const Slot* Find(std::uint64_t key) const
		{
			for (const Slot& slot : m_slots)
			{
				if (slot.used && slot.key == key)
					return &slot;
			}
			return nullptr;
		}

		void Store(std::uint64_t key, const FrameStamp<Pose>& stamp)
		{
			Slot* target = nullptr;
			for (Slot& slot : m_slots)
			{
				if (slot.used && slot.key == key)
				{
					target = &slot;
					break;
				}
				if (!slot.used && target == nullptr)
					target = &slot;
			}
			if (target == nullptr)
			{
				target = &m_slots[0];
				for (Slot& slot : m_slots)
				{
					if (slot.stamp.latch_seq < target->stamp.latch_seq)
						target = &slot;
				}
			}
			*target = Slot{true, key, stamp};
		}

		void Forget(std::uint64_t key)
		{
			for (Slot& slot : m_slots)
			{
				if (slot.used && slot.key == key)
					slot = Slot{};
			}
		}

		std::array<Slot, kKeySlots> m_slots{};
		FrameStamp<Pose> m_latch{};
		FrameStamp<Pose> m_last_declared{};
		std::uint64_t m_seq = 0;
		std::uint64_t m_floor = 0;
		std::uint64_t m_max_age;
		std::atomic<unsigned> m_request{0};
		bool m_latch_open = false;
		bool m_xr_frame_open = false;
		bool m_enabled = true;
	};
}
