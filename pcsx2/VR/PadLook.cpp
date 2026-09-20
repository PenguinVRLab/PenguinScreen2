// SPDX-FileCopyrightText: 2026 Patrick Carey
// SPDX-License-Identifier: GPL-3.0

#include "VR/PadLook.h"

#include "Host.h"
#include "Memory.h"
#include "VR/CameraDriver.h"
#include "VR/XRCompositor.h"
#include "VR/XRSession.h"

#include "common/Console.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace VR::PadLook
{
	namespace
	{
		std::atomic<int> s_rx_offset{0};

		struct SeqStep
		{
			int rx;
			u64 polls;
		};

		float ProbeF32(u32 addr)
		{
			if (!eeMem)
				return 0.0f;
			u32 raw;
			std::memcpy(&raw, &eeMem->Main[addr & 0x01FFFFFFu], sizeof(raw));
			return std::bit_cast<float>(raw);
		}

		u8 ProbeU8(u32 addr)
		{
			if (!eeMem)
				return 0;
			return eeMem->Main[addr & 0x01FFFFFFu];
		}

		class Sequencer
		{
		public:
			static Sequencer& Get()
			{
				static Sequencer s;
				return s;
			}

			bool Armed() const { return m_armed; }

			u8 Tick(u8 real)
			{
				if (m_step >= m_steps.size())
					return static_cast<u8>(m_hold);

				const SeqStep& st = m_steps[m_step];
				const bool last_poll_of_step = (m_in_step + 1 >= st.polls);

				Sample(real, st.rx, last_poll_of_step);

				if (last_poll_of_step)
				{
					if (!m_dump_dir.empty())
						DumpRam();
					m_in_step = 0;
					m_hold = st.rx;
					if (++m_step >= m_steps.size())
					{
						Log("# SEQ_DONE polls=%llu\n", static_cast<unsigned long long>(m_poll));
						if (m_fp)
							std::fflush(m_fp);
						Console.WriteLn("(VR) padLook measurement sequence complete (%llu polls).",
							static_cast<unsigned long long>(m_poll));
					}
				}
				else
				{
					m_in_step++;
				}
				m_poll++;
				return static_cast<u8>(st.rx);
			}

		private:
			Sequencer()
			{
				const char* seq = std::getenv("PCSX2_VR_PADLOOK_SEQ");
				if (!seq || !*seq)
					return;
				ParseSeq(seq);
				if (m_steps.empty())
				{
					Console.Error("(VR) PCSX2_VR_PADLOOK_SEQ set but no valid steps parsed — lane stays inert.");
					return;
				}
				if (const char* pr = std::getenv("PCSX2_VR_PADLOOK_PROBE"))
					ParseProbes(pr, m_probes);
				if (const char* pr8 = std::getenv("PCSX2_VR_PADLOOK_PROBE8"))
					ParseProbes(pr8, m_probes8);
				if (const char* dd = std::getenv("PCSX2_VR_PADLOOK_DUMP"))
					m_dump_dir = dd;
				if (const char* lp = std::getenv("PCSX2_VR_PADLOOK_LOG"))
					m_fp = std::fopen(lp, "wb");

				m_armed = true;
				Log("# padLook transfer-function lane: %zu steps, %zu probes\n",
					m_steps.size(), m_probes.size());
				Log("poll,step,rx,real");
				for (size_t i = 0; i < m_probes.size(); i++)
					Log(",p%zu_%08X", i, m_probes[i]);
				for (size_t i = 0; i < m_probes8.size(); i++)
					Log(",b%zu_%08X", i, m_probes8[i]);
				Log("\n");
				Console.WriteLn("(VR) padLook measurement lane ARMED: %zu steps, %zu probes.",
					m_steps.size(), m_probes.size());
			}

			void ParseSeq(const char* s)
			{
				const char* p = s;
				while (*p)
				{
					char* end = nullptr;
					const long rx = std::strtol(p, &end, 0);
					if (end == p || *end != ':')
						break;
					p = end + 1;
					const long polls = std::strtol(p, &end, 0);
					if (end == p || polls <= 0)
						break;
					m_steps.push_back({static_cast<int>(std::clamp(rx, 0L, 255L)),
						static_cast<u64>(polls)});
					p = end;
					if (*p == ',')
						p++;
				}
			}

			void ParseProbes(const char* s, std::vector<u32>& out)
			{
				const char* p = s;
				while (*p)
				{
					char* end = nullptr;
					const unsigned long a = std::strtoul(p, &end, 0);
					if (end == p)
						break;
					out.push_back(static_cast<u32>(a));
					p = end;
					if (*p == ',')
						p++;
				}
			}

			void Sample(u8 real, int rx, bool )
			{
				Log("%llu,%zu,%d,%u", static_cast<unsigned long long>(m_poll), m_step, rx,
					static_cast<unsigned>(real));
				for (const u32 a : m_probes)
					Log(",%.6f", static_cast<double>(ProbeF32(a)));
				for (const u32 a : m_probes8)
					Log(",%u", static_cast<unsigned>(ProbeU8(a)));
				Log("\n");
				if ((m_poll & 0x3F) == 0 && m_fp)
					std::fflush(m_fp);
			}

			void DumpRam()
			{
				if (!eeMem)
					return;
				char path[1024];
				std::snprintf(path, sizeof(path), "%s/step%02zu_rx%03d.eeMemory.bin",
					m_dump_dir.c_str(), m_step, m_steps[m_step].rx);
				if (std::FILE* f = std::fopen(path, "wb"))
				{
					std::fwrite(eeMem->Main, 1, Ps2MemSize::MainRam, f);
					std::fclose(f);
					Log("# DUMP step=%zu rx=%d -> %s\n", m_step, m_steps[m_step].rx, path);
				}
				else
				{
					Log("# DUMP-FAILED step=%zu path=%s\n", m_step, path);
				}
			}

			void Log(const char* fmt, ...)
			{
				va_list ap;
				va_start(ap, fmt);
				std::vfprintf(m_fp ? m_fp : stderr, fmt, ap);
				va_end(ap);
			}

			bool m_armed = false;
			std::vector<SeqStep> m_steps;
			std::vector<u32> m_probes;
			std::vector<u32> m_probes8;
			std::string m_dump_dir;
			std::FILE* m_fp = nullptr;
			size_t m_step = 0;
			u64 m_in_step = 0;
			u64 m_poll = 0;
			int m_hold = 127;
		};
	}

	void Publish(float deflection)
	{
		if (!std::isfinite(deflection))
			deflection = 0.0f;
		deflection = std::clamp(deflection, -1.0f, 1.0f);
		s_rx_offset.store(-static_cast<int>(std::lround(deflection * 127.0f)),
			std::memory_order_relaxed);
	}

	u8 ProbeStick(StickAxis axis, u8 real)
	{
		struct Probe
		{
			int axis = -1;
			u8 value = 0x80;

			Probe()
			{
				const char* e = std::getenv("PCSX2_VR_PADSTICK");
				if (!e || !*e)
					return;
				const bool is_lx = (e[0] == 'l' || e[0] == 'L') && (e[1] == 'x' || e[1] == 'X');
				const bool is_rx = (e[0] == 'r' || e[0] == 'R') && (e[1] == 'x' || e[1] == 'X');
				if ((!is_lx && !is_rx) || e[2] != ':')
				{
					Console.Error("(VR) PCSX2_VR_PADSTICK: expected <lx|rx>:<byte>, got '%s' — probe inert.", e);
					return;
				}
				const long v = std::strtol(e + 3, nullptr, 0);
				if (v < 0 || v > 255)
				{
					Console.Error("(VR) PCSX2_VR_PADSTICK: byte %ld out of range 0..255 — probe inert.", v);
					return;
				}
				axis = static_cast<int>(is_lx ? StickAxis::LX : StickAxis::RX);
				value = static_cast<u8>(v);
				Console.WriteLn("(VR) padStick PROBE ARMED: %s forced to 0x%02X (headless stick-hold; no VR state armed).",
					is_lx ? "LX" : "RX", value);
			}
		};
		static const Probe s_probe;

		if (s_probe.axis != static_cast<int>(axis))
			return real;

		static std::atomic<u64> s_hits{0};
		if (s_hits.fetch_add(1, std::memory_order_relaxed) == 0)
		{
			Console.WriteLn("(VR) padStick PROBE FIRED on %s — this poll site returned the forced byte.",
				axis == StickAxis::LX ? "LX" : "RX");
		}
		return s_probe.value;
	}

	u8 ApplyRx(u8 real)
	{
		if (Sequencer::Get().Armed()) [[unlikely]]
			return Sequencer::Get().Tick(real);

		const int offset = s_rx_offset.load(std::memory_order_relaxed);
		if (offset == 0)
			return real;
		return static_cast<u8>(std::clamp(static_cast<int>(real) + offset, 0, 255));
	}

	void UpdateRecenterChord(bool l1, bool r1, bool l3, bool r3)
	{
		static bool s_chord_was_held = false;
		const bool held = l1 && r1 && l3 && r3;
		if (held && !s_chord_was_held && XRSession::IsSessionRunning())
		{
			CameraDriver::RequestRecenter();
			XRCompositor::RequestScreenReanchor();
			Host::AddKeyedOSDMessage("VRRecenter", "VR recentered (head + screen).", 2.0f);
			Console.WriteLn("(VR) Recenter chord (L1+R1+L3+R3) fired: head camera + screen re-anchor.");
		}
		s_chord_was_held = held;
	}
}
