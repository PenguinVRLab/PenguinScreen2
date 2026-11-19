// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "common/Pcsx2Types.h"

#include <cmath>
#include <cstring>

namespace VR
{
	enum class ExecAction : u8
	{
		AddGpr = 1,
		AddFpr = 2,
		ComposeView = 3,
		AimLookAt = 4,
		Record = 5,
	};

	enum class ExecOwner : u8
	{
		Look = 0,
		Projection = 1,
	};

	struct ExecVisitRecord
	{
		u64 seq = 0;
		u32 pc = 0;
		u32 base = 0;
		float pre[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
		float lever[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
		bool has_lever = false;
	};

	template <typename Read32>
	inline bool ExecReadVisit(Read32 read32, u32 base, u32 offset, u32 lever_address, u32 ram_end, ExecVisitRecord* out)
	{
		const auto read3x3 = [&read32](u32 at, float m[9]) {
			for (int r = 0; r < 3; r++)
				for (int c = 0; c < 3; c++)
				{
#ifdef VR_EXECHOOK_PLANTED_FAULT_RECORD_COLUMNS
					const u32 w = read32(at + static_cast<u32>((c * 4 + r) * 4));
#else
					const u32 w = read32(at + static_cast<u32>((r * 4 + c) * 4));
#endif
					float f;
					std::memcpy(&f, &w, 4);
					if (!std::isfinite(f))
						return false;
					m[r * 3 + c] = f;
				}
			return true;
		};
		const u64 at = static_cast<u64>(base) + offset;
		if (base < 0x00010000u || (at & 3) != 0 || at + 64 > ram_end)
			return false;
		ExecVisitRecord r;
		r.base = base;
		if (!read3x3(static_cast<u32>(at), r.pre))
			return false;
		if (lever_address != 0)
		{
			if (lever_address < 0x00010000u || (lever_address & 3) != 0 || static_cast<u64>(lever_address) + 64 > ram_end ||
				!read3x3(lever_address, r.lever))
				return false;
			r.has_lever = true;
		}
		r.seq = out->seq;
		r.pc = out->pc;
		*out = r;
		return true;
	}

	struct ExecGate
	{
		u32 address = 0;
		u32 equals = 0;
		u8 width = 0;
		bool not_equals = false;
	};
	static constexpr int EXEC_GATES_MAX = 2;

	inline bool ExecGateHolds(const ExecGate& g, u32 value)
	{
#ifdef VR_EXECHOOK_PLANTED_FAULT_GATE
		(void)g;
		(void)value;
		return true;
#else
		const u32 mask = (g.width == 1) ? 0xFFu : ((g.width == 2) ? 0xFFFFu : 0xFFFFFFFFu);
		const bool same = ((value & mask) == (g.equals & mask));
		return g.not_equals ? !same : same;
#endif
	}

	struct ExecHookSpec
	{
		u32 pc = 0;
		u32 expect_op = 0;
		ExecAction action = ExecAction::AddGpr;
		u8 reg = 0;
		u8 reg2 = 0;
		u32 address = 0;
		float yaw_units = 0.0f;
		float pitch_units = 0.0f;
		bool use_pitch = false;
		u32 caller_sp_offset = 0;
		u32 caller_expect = 0;
		ExecGate gates[EXEC_GATES_MAX];
		u32 record_offset = 0;
		u32 record_lever = 0;
		ExecOwner owner = ExecOwner::Look;
	};

	static constexpr int EXECHOOK_MAX = 4;

	static constexpr float EXEC_BINARY_ANGLE_PER_RAD = 65536.0f / 6.28318530717958647692f;

	inline s32 ExecAngleUnits(float rad, float units_per_rad)
	{
		const float v = rad * units_per_rad;
		if (!std::isfinite(v))
			return 0;
		if (v > 2147483000.0f)
			return 2147483000;
		if (v < -2147483000.0f)
			return -2147483000;
		return static_cast<s32>(std::lround(v));
	}

	inline void ExecComposeView(float m[16], float yaw_rad, float pitch_rad)
	{
		if (!std::isfinite(yaw_rad) || !std::isfinite(pitch_rad))
			return;
		if (yaw_rad == 0.0f && pitch_rad == 0.0f)
			return;
		const float cy = std::cos(yaw_rad), sy = std::sin(yaw_rad);
		const float cp = std::cos(pitch_rad), sp = std::sin(pitch_rad);
#ifdef VR_EXECHOOK_PLANTED_FAULT_ROWS3
		const int rows = 3;
#else
		const int rows = 4;
#endif
		for (int r = 0; r < rows; r++)
		{
			float* v = &m[r * 4];
#ifdef VR_EXECHOOK_PLANTED_FAULT_SIGN
			const float x = v[0] * cy - v[2] * sy;
			const float z = v[0] * sy + v[2] * cy;
#else
			const float x = v[0] * cy + v[2] * sy;
			const float z = -v[0] * sy + v[2] * cy;
#endif
			const float y = v[1] * cp - z * sp;
			const float z2 = v[1] * sp + z * cp;
			v[0] = x;
			v[1] = y;
			v[2] = z2;
		}
	}

	static constexpr float EXEC_AIM_MAX_ELEVATION = 1.5533430342749532f;

	inline bool ExecAimLookAt(const float eye[3], float target[3], float yaw_rad, float pitch_rad)
	{
		if (!std::isfinite(yaw_rad) || !std::isfinite(pitch_rad))
			return false;
		for (int i = 0; i < 3; i++)
		{
			if (!std::isfinite(eye[i]) || !std::isfinite(target[i]))
				return false;
		}
		const float dx = target[0] - eye[0], dy = target[1] - eye[1], dz = target[2] - eye[2];
		const float h = std::sqrt(dx * dx + dz * dz);
		const float len = std::sqrt(h * h + dy * dy);
		if (!std::isfinite(len) || !(len > 0.0f) || !(h > 1e-5f * len))
			return false;
		if (yaw_rad == 0.0f && pitch_rad == 0.0f)
			return true;
		const float cy = std::cos(yaw_rad), sy = std::sin(yaw_rad);
#ifdef VR_EXECHOOK_PLANTED_FAULT_AIM_SIGN
		const float x = dx * cy - dz * sy;
		const float z = dx * sy + dz * cy;
#else
		const float x = dx * cy + dz * sy;
		const float z = -dx * sy + dz * cy;
#endif
		float e = std::atan2(dy, h) + pitch_rad;
		if (e > EXEC_AIM_MAX_ELEVATION)
			e = EXEC_AIM_MAX_ELEVATION;
		if (e < -EXEC_AIM_MAX_ELEVATION)
			e = -EXEC_AIM_MAX_ELEVATION;
#ifdef VR_EXECHOOK_PLANTED_FAULT_AIM_LENGTH
		const float k = 1.0f;
#else
		const float k = len * std::cos(e) / h;
#endif
		target[0] = eye[0] + x * k;
		target[1] = eye[1] + len * std::sin(e);
		target[2] = eye[2] + z * k;
		return true;
	}

	int ExecHookArm(const ExecHookSpec& spec);

	void ExecHookClearAll(ExecOwner owner = ExecOwner::Look);

	void ExecHookForgetVisits(ExecOwner owner);

	inline bool ExecHookSameAddressAllowed(const ExecHookSpec& armed, const ExecHookSpec& want)
	{
#ifdef VR_EXECHOOK_PLANTED_FAULT_OWNERS
		(void)armed;
		(void)want;
		return true;
#else
		return armed.pc != want.pc || armed.owner == want.owner;
#endif
	}

	bool ExecHookLastVisit(u32 pc, ExecVisitRecord* out);

	void ExecHookSetHead(float yaw_rad, float pitch_rad, bool active);

	bool ExecHookOnHit(u32 pc);

	u32 ExecHookArmedCount();
	u64 ExecHookHits(int id);

	bool ExecHookIsArmedAt(u32 pc);


}
