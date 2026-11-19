// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "common/Pcsx2Defs.h"

#include "VR/VRInputState.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>

namespace VR::PadXR
{
	enum class Family : u8
	{
		PAD,
		PER,
		RF,
		STREAM,
		HOST,
		CAM
	};

	enum class Route : u8
	{
		Merge,
		Adapter,
		Device,
		Stream,
		Host,
		None,
		Camera
	};

	enum class Res : u8
	{
		Mic,
		Layers,
		Haptics,
		RightStick,
		LeftStick,
		Speed,
		Count
	};
	static constexpr size_t kResCount = static_cast<size_t>(Res::Count);

	enum class Claim : u8
	{
		None,
		Shared,
		Tee,
		Exclusive,
		Add,
		Set,
		Own,
	};

	enum class Online : u8
	{
		Any,
		Private,
		Offline
	};

	enum class Clock : u8
	{
		Game,
		Wall
	};

	namespace Prim
	{
		static constexpr u16 R1Source = 1u << 0;
		static constexpr u16 R2Frame = 1u << 1;
		static constexpr u16 R3Zone = 1u << 2;
		static constexpr u16 R4Ray = 1u << 3;
		static constexpr u16 R5Solver = 1u << 4;
		static constexpr u16 R6Detector = 1u << 5;
		static constexpr u16 R7Output = 1u << 6;
		static constexpr u16 R8Gate = 1u << 7;
		static constexpr u16 R9Port = 1u << 8;
		static constexpr u16 R9Device = 1u << 9;
		static constexpr u16 R9Resource = 1u << 10;
		static constexpr u16 R10Feedback = 1u << 11;
		static constexpr u16 R11Seat = 1u << 12;
		static constexpr u16 R12GameState = 1u << 13;
	}

	namespace Need
	{
		static constexpr u32 HeadPose = 1u << 0;
		static constexpr u32 Head6Dof = 1u << 1;
		static constexpr u32 Controllers = 1u << 2;
		static constexpr u32 AnalogTrigger = 1u << 3;
		static constexpr u32 CapacitiveTouch = 1u << 4;
		static constexpr u32 Hands = 1u << 5;
		static constexpr u32 FingerCurl = 1u << 6;
		static constexpr u32 TwoHands = 1u << 7;
		static constexpr u32 Haptics = 1u << 8;
		static constexpr u32 Mic = 1u << 9;
		static constexpr u32 EyeGaze = 1u << 10;
		static constexpr u32 AngularVelocity = 1u << 11;
		static constexpr u32 ScreenHit = 1u << 12;
		static constexpr u32 SeatCalibration = 1u << 13;
		static constexpr u32 Standing = 1u << 14;
		static constexpr u32 Roomscale = 1u << 15;
		static constexpr u32 Pad = 1u << 16;
	}

	namespace SessionGate
	{
		static constexpr u8 LiveSnapshot = 1u << 0;
		static constexpr u8 ActionsActive = 1u << 1;
		static constexpr u8 NotStale = 1u << 2;
		static constexpr u8 NotSplit = 1u << 3;
		static constexpr u8 NotReplaying = 1u << 4;
		static constexpr u8 All = 0x1F;
	}

	static constexpr std::array<const char*, 16> kButtonNames = {"L2", "R2", "L1", "R1", "Triangle", "Circle", "Cross",
		"Square", "Select", "L3", "R3", "Start", "Up", "Right", "Down", "Left"};

	struct MemberDesc
	{
		std::string_view id;
		std::string_view kind;
		u16 primitives = 0;
		std::array<u32, 4> needs{};
		std::array<Claim, kResCount> res{};
		u8 layers = 0;
		std::array<Claim, 16> buttons{};
		Family family = Family::PAD;
		Route route = Route::Merge;
		Online online = Online::Any;
		Clock clock = Clock::Game;
		u8 session_gates = SessionGate::All;
		bool harness = false;
		bool skeleton_gates_needs = true;

		Claim Of(Res r) const { return res[static_cast<size_t>(r)]; }
	};

	inline bool NeedsMet(const MemberDesc& d, u32 caps, u32* missing = nullptr)
	{
		for (const u32 group : d.needs)
		{
			if (group != 0 && (group & caps) == 0)
			{
				if (missing)
					*missing = group;
				return false;
			}
		}
		return true;
	}

	inline bool ClaimPairCompatible(Claim a, Claim b)
	{
		if (a == Claim::None || b == Claim::None)
			return true;
		if (a == Claim::Exclusive || b == Claim::Exclusive)
			return false;
#ifndef VR_PADXR_PLANTED_TWO_OWN
		if (a == Claim::Own && b == Claim::Own)
			return false;
#endif
		return true;
	}

	inline bool ClaimsCompatible(const MemberDesc& a, const MemberDesc& b, Res* clash)
	{
		if (a.harness || b.harness)
			return true;
		for (size_t i = 0; i < kResCount; i++)
		{
			if (static_cast<Res>(i) == Res::Layers)
				continue;
			if (!ClaimPairCompatible(a.res[i], b.res[i]))
			{
				if (clash)
					*clash = static_cast<Res>(i);
				return false;
			}
		}
		return true;
	}

	inline bool ButtonClaimsCompatible(const MemberDesc& a, const MemberDesc& b, int* bit)
	{
		if (a.harness || b.harness)
			return true;
		for (size_t i = 0; i < a.buttons.size(); i++)
		{
#ifndef VR_PADXR_PLANTED_BUTTON_TWO_OWN
			if (a.buttons[i] == Claim::Own && b.buttons[i] == Claim::Own)
			{
				if (bit)
					*bit = static_cast<int>(i);
				return false;
			}
#endif
		}
		return true;
	}

	inline bool LayersFit(u32 claimed, const MemberDesc& d, u32 budget) { return claimed + d.layers <= budget; }

	inline u32 CapsFromSnapshot(const VRInputSnapshot& s)
	{
		u32 caps = Need::Pad;
		if (s.generation == 0)
			return caps;
		if (s.head_pose.valid)
			caps |= Need::HeadPose | Need::Head6Dof;
		const bool l = s.hands[VRInputSnapshot::LEFT].aim_pose.valid || s.hands[VRInputSnapshot::LEFT].grip_pose.valid;
		const bool r = s.hands[VRInputSnapshot::RIGHT].aim_pose.valid || s.hands[VRInputSnapshot::RIGHT].grip_pose.valid;
		if (l || r)
			caps |= Need::Controllers | Need::AnalogTrigger | Need::Haptics;
		if (l && r)
			caps |= Need::TwoHands;
		return caps;
	}

	enum class Axis : u8
	{
		LX,
		RX,
		LY,
		RY,
		Count
	};
	static constexpr size_t kAxisCount = static_cast<size_t>(Axis::Count);

	static constexpr int kButtonBits = 16;
	static_assert(kButtonNames.size() == kButtonBits && sizeof(MemberDesc::buttons) / sizeof(Claim) == kButtonBits);

	static constexpr std::array<int, 12> kPressureBit = {13, 15, 12, 14, 4, 5, 6, 7, 2, 3, 0, 1};

	inline int PressureBitOfByte(size_t poll_byte)
	{
		return (poll_byte >= 9 && poll_byte <= 20) ? kPressureBit[poll_byte - 9] : -1;
	}

	inline u8 MergeStickByte(u8 real, Claim mode, float value)
	{
		if (!std::isfinite(value))
			value = 0.0f;
		value = std::clamp(value, -1.0f, 1.0f);
		switch (mode)
		{
			case Claim::Add:
				return static_cast<u8>(std::clamp(static_cast<int>(real) + static_cast<int>(std::lround(value * 127.0f)), 0, 255));
			case Claim::Set:
			case Claim::Own:
				return static_cast<u8>(std::clamp(static_cast<int>(std::lround((value + 1.0f) * 127.5f)), 0, 255));
			default:
				return real;
		}
	}

	struct StickCell
	{
		bool has_own = false;
		u8 own = 0x80;
		bool has_set = false;
		u8 set = 0x80;
		int add = 0;

		void Clear() { *this = StickCell{}; }
		bool Empty() const { return !has_own && !has_set && add == 0; }

		void Write(Claim mode, float value)
		{
			switch (mode)
			{
				case Claim::Own:
					if (!has_own)
					{
						has_own = true;
						own = MergeStickByte(0x80, Claim::Own, value);
					}
					break;
				case Claim::Set:
					if (!has_set)
					{
						has_set = true;
						set = MergeStickByte(0x80, Claim::Set, value);
					}
					break;
				case Claim::Add:
					if (std::isfinite(value))
						add += static_cast<int>(std::lround(std::clamp(value, -1.0f, 1.0f) * 127.0f));
					break;
				default:
					break;
			}
		}

		void OwnByte(u8 b)
		{
			if (!has_own)
			{
				has_own = true;
				own = b;
			}
		}

		void AddUnits(int units) { add += units; }

		void Merge(const StickCell& o)
		{
			if (o.has_own && !has_own)
			{
				has_own = true;
				own = o.own;
			}
			if (o.has_set && !has_set)
			{
				has_set = true;
				set = o.set;
			}
			add += o.add;
		}

		u8 Apply(u8 real) const
		{
			if (has_own)
				return own;
			if (has_set)
				return set;
			if (add == 0)
				return real;
			return static_cast<u8>(std::clamp(static_cast<int>(real) + add, 0, 255));
		}
	};

	struct WordCell
	{
		u16 press = 0;
		u16 own_mask = 0;
		u16 own_down = 0;

		void Clear() { *this = WordCell{}; }
		bool Empty() const { return press == 0 && own_mask == 0; }
	};

	inline u32 MergeWord(u32 real, const WordCell& c)
	{
#ifdef VR_PADXR_PLANTED_LAST_WRITER
		if (!c.Empty())
			return (real & ~0xFFFFu) | (0xFFFFu & ~static_cast<u32>(c.press | (c.own_mask & c.own_down)));
		return real;
#else
		u32 w = real & ~static_cast<u32>(c.press);
		w = (w & ~static_cast<u32>(c.own_mask)) | (static_cast<u32>(c.own_mask) & ~static_cast<u32>(c.own_down));
		return w;
#endif
	}

	struct PressureCell
	{
		u16 mask = 0;
		u16 own_mask = 0;
		std::array<u8, kButtonBits> value{};

		void Clear() { *this = PressureCell{}; }
		bool Empty() const { return mask == 0 && own_mask == 0; }

		void Write(int bit, u8 v, bool own)
		{
			if (bit < 0 || bit >= kButtonBits)
				return;
			const u16 b = static_cast<u16>(1u << bit);
			if (own)
			{
				if (!(own_mask & b))
				{
					own_mask |= b;
					value[bit] = v;
				}
				return;
			}
			if (own_mask & b)
				return;
			value[bit] = (mask & b) ? std::max(value[bit], v) : v;
			mask |= b;
		}
	};

	inline void MergePressureCell(PressureCell& into, const PressureCell& o)
	{
		for (int bit = 0; bit < kButtonBits; bit++)
		{
			const u16 b = static_cast<u16>(1u << bit);
			if (o.own_mask & b)
				into.Write(bit, o.value[bit], true);
			else if (o.mask & b)
				into.Write(bit, o.value[bit], false);
		}
	}

	inline int MergePressure(const PressureCell& c, int bit, u8 real)
	{
		if (bit < 0 || bit >= kButtonBits)
			return -1;
		const u16 b = static_cast<u16>(1u << bit);
		if (c.own_mask & b)
			return c.value[bit];
		if (c.mask & b)
			return std::max<int>(real, c.value[bit]);
		return -1;
	}

	struct Cue
	{
		u8 hand = 0;
		u8 locus = 0;
		float amplitude = 0.0f;
		u16 ms = 0;
	};

	inline bool CueValid(const Cue& c)
	{
		return c.hand < 2 && std::isfinite(c.amplitude) && c.amplitude > 0.0f && c.ms > 0;
	}

	struct EdgeScheduler
	{
		std::array<u32, kButtonBits> remaining{};
		u16 fresh = 0;
		u16 down_last = 0;

		void Press(int bit, u32 count)
		{
			if (bit < 0 || bit >= kButtonBits)
				return;
			remaining[bit] = count;
			if (count)
				fresh |= static_cast<u16>(1u << bit);
		}

		void Reset() { *this = EdgeScheduler{}; }

		u16 Step()
		{
			u16 down = 0;
			for (int bit = 0; bit < kButtonBits; bit++)
			{
				if (remaining[bit] == 0)
					continue;
				const u16 b = static_cast<u16>(1u << bit);
				if ((fresh & b) && (down_last & b))
					continue;
				down |= b;
				remaining[bit]--;
			}
			fresh = 0;
			down_last = down;
			return down;
		}

		bool Idle() const
		{
			for (const u32 r : remaining)
				if (r)
					return false;
			return true;
		}
	};
}
