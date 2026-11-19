// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "VR/WorldProjectionCore.h"

#include "common/Pcsx2Types.h"

#include <cstdint>
#include <cstdio>
#include <string>

namespace VR::WorldProjection
{
	enum class WhyNot : std::uint8_t
	{
		Declarable = 0,
		NoVisit,
		StaleVisit,
		NoView,
		Refused,
	};

	struct GateTally
	{
		static constexpr int kMaxGates = 16;
		static constexpr std::uint32_t kWindow = 300;

		struct Gate
		{
			std::uint32_t address = 0;
			std::uint32_t equals = 0;
			std::uint8_t width = 4;
			bool not_equals = false;
			bool chain = false;
			std::uint32_t held = 0;
			std::uint32_t first_fail = 0;
			std::uint32_t last_value = 0;
		};

		int count = 0;
		Gate gates[kMaxGates];
		std::uint32_t reads = 0;
		std::uint32_t declarable = 0;
		std::uint32_t why[5] = {0, 0, 0, 0, 0};

		void Begin(int n, const std::uint32_t* address, const std::uint32_t* equals, const std::uint8_t* width,
			const bool* not_equals, const bool* chain)
		{
			*this = GateTally{};
			count = n < kMaxGates ? n : kMaxGates;
			for (int i = 0; i < count; i++)
			{
				gates[i].address = address[i];
				gates[i].equals = equals[i];
				gates[i].width = width[i];
				gates[i].not_equals = not_equals[i];
				gates[i].chain = chain[i];
			}
		}

		void Restart()
		{
			reads = declarable = 0;
			for (std::uint32_t& w : why)
				w = 0;
			for (int i = 0; i < count; i++)
				gates[i].held = gates[i].first_fail = gates[i].last_value = 0;
		}

		void Add(const bool* held, const std::uint32_t* value, bool is_declarable, WhyNot why_not)
		{
			reads++;
			if (is_declarable)
				declarable++;
			bool all = true;
			for (int i = 0; i < count; i++)
			{
#if defined(VR_WORLDPROJ_PLANTED_GATECOUNT_OFF)
				const bool h = true;
#else
				const bool h = held[i];
#endif
				if (h)
				{
					gates[i].held++;
					continue;
				}
				if (all)
				{
					gates[i].first_fail++;
					gates[i].last_value = value[i];
				}
				all = false;
			}
			if (all && !is_declarable)
				why[static_cast<int>(why_not)]++;
		}

		bool Full() const { return reads >= kWindow; }

		std::string Line() const
		{
			std::string s;
			char b[96];
			std::snprintf(b, sizeof(b), "(VR) world projection gates: %u of the last %u reads declarable; held", declarable,
				reads);
			s += b;
			for (int i = 0; i < count; i++)
			{
				std::snprintf(b, sizeof(b), " g%d@%08X %u", i + 1, gates[i].address, gates[i].held);
				s += b;
			}
			s += "; first fail";
			bool any = false;
			for (int i = 0; i < count; i++)
			{
				if (gates[i].first_fail == 0)
					continue;
				if (gates[i].chain)
					std::snprintf(b, sizeof(b), " g%d@%08X %u last=chain", i + 1, gates[i].address, gates[i].first_fail);
				else
					std::snprintf(b, sizeof(b), " g%d@%08X %u last=0x%0*X want%s0x%0*X", i + 1, gates[i].address,
						gates[i].first_fail, gates[i].width * 2, gates[i].last_value, gates[i].not_equals ? "!=" : "=",
						gates[i].width * 2, gates[i].equals);
				s += b;
				any = true;
			}
			if (!any)
				s += " none";
			const std::uint32_t w = why[1] + why[2] + why[3] + why[4];
			std::snprintf(b, sizeof(b), "; all held, not declarable %u (no visit %u, stale visit %u, no view %u, refused %u).",
				w, why[1], why[2], why[3], why[4]);
			s += b;
			return s;
		}
	};

	void OnEeVsync();

	void OnStateLoaded();

	void OnReset();

	void OnRendererSwitch();

	bool GsActive();

	bool EeEnabled();

	void OnGsDraw(u32 frame_fbp, u32 display_fbp);

	void OnGsPresent(u32 display_fbp);

	Presented LastPresent();
}
