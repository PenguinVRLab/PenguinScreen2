// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#include "VR/ControlQuads.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace VR::ControlQuads
{
	namespace
	{
		std::mutex s_mutex;
		std::array<Slot, kSlots> s_slots{};
	}

	void Publish(int slot, const Slot& state)
	{
		if (slot < 0 || slot >= kSlots)
			return;
		std::lock_guard lock(s_mutex);
		s_slots[slot] = state;
	}

	void ClearAll()
	{
		std::lock_guard lock(s_mutex);
		for (Slot& s : s_slots)
			s = Slot{};
	}

	Slot Get(int slot)
	{
		if (slot < 0 || slot >= kSlots)
			return Slot{};
		std::lock_guard lock(s_mutex);
		return s_slots[slot];
	}

	u32 RasterKey(float t, bool grabbed, bool broke_away)
	{
		if (!std::isfinite(t))
			t = 0.0f;
		const int q = static_cast<int>(std::lround(std::clamp(t, -1.0f, 1.0f) * 200.0f));
		return static_cast<u32>(q + 200) | (grabbed ? 0x10000u : 0u) | (broke_away ? 0x20000u : 0u);
	}

	namespace
	{
		constexpr float kPi = 3.14159265358979323846f;

		constexpr u32 kTransparent = PackRgba(0, 0, 0, 0);
		constexpr u32 kCardIdle = PackRgba(0x1c, 0x1f, 0x29);
		constexpr u32 kCardGrab = PackRgba(0x1e, 0x26, 0x3a);
		constexpr u32 kBorderIdle = PackRgba(0x4a, 0x4f, 0x5c);
		constexpr u32 kBorderGrab = PackRgba(0x5c, 0xb0, 0xff);
		constexpr u32 kTrack = PackRgba(0x5e, 0x63, 0x70);
		constexpr u32 kTick = PackRgba(0x6a, 0x70, 0x7e);
		constexpr u32 kIdleTick = PackRgba(0xc8, 0xcc, 0xd6);
		constexpr u32 kText = PackRgba(0xb8, 0xbd, 0xc8);
		constexpr u32 kTextDim = PackRgba(0x8a, 0x90, 0x9c);
		constexpr u32 kArmIdle = PackRgba(0x78, 0x7e, 0x8c);
		constexpr u32 kArmGrab = PackRgba(0x62, 0xa0, 0xdc);
		constexpr u32 kPivot = PackRgba(0x30, 0x34, 0x40);
		constexpr u32 kPivotRing = PackRgba(0x8a, 0x90, 0x9c);
		constexpr u32 kGripEdgeIdle = PackRgba(0xd0, 0xd4, 0xdc);
		constexpr u32 kGripEdgeGrab = PackRgba(0xe6, 0xf3, 0xff);
		constexpr u32 kGripText = PackRgba(0x12, 0x16, 0x20);
		constexpr u32 kHandEdge = PackRgba(0x5a, 0x38, 0x14);

		constexpr int kCardInset = 2;
		constexpr int kCardRadius = 14;
		constexpr int kPivotInset = 34;
		constexpr int kPivotY = kHeight / 2;
		constexpr float kArmLen = 168.0f;
		constexpr float kHalfSweepDeg = 30.0f;
		constexpr float kArmHalfWidth = 4.0f;
		constexpr int kGripHalfW = 36;
		constexpr int kGripHalfH = 18;
		constexpr int kGripRadius = 9;
		constexpr float kTickLen = 12.0f;
		constexpr float kIdleTickLen = 22.0f;
		constexpr int kPalmRadius = 13;
		constexpr int kFingerHalfW = 4;
		constexpr int kFingerLen = 20;

		struct Pt
		{
			float x, y;
		};

		float Outward(int slot)
		{
			return (slot == kRight) ? 1.0f : -1.0f;
		}

		Pt PivotOf(int slot)
		{
			return {(slot == kRight) ? static_cast<float>(kPivotInset) : static_cast<float>(kWidth - 1 - kPivotInset),
				static_cast<float>(kPivotY)};
		}

		Pt TipAt(int slot, float t, float radius = kArmLen)
		{
			t = std::clamp(std::isfinite(t) ? t : 0.0f, -1.0f, 1.0f);
			const float th = t * kHalfSweepDeg * kPi / 180.0f;
			const Pt p = PivotOf(slot);
			return {p.x + Outward(slot) * radius * std::cos(th), p.y - radius * std::sin(th)};
		}

		struct Canvas
		{
			std::vector<u32>& px;
			void Put(int x, int y, u32 c)
			{
				if (x < 0 || y < 0 || x >= static_cast<int>(kWidth) || y >= static_cast<int>(kHeight))
					return;
				px[static_cast<size_t>(y) * kWidth + x] = c;
			}
			void Rect(int x0, int y0, int x1, int y1, u32 c)
			{
				for (int y = std::max(y0, 0); y <= std::min(y1, static_cast<int>(kHeight) - 1); ++y)
					for (int x = std::max(x0, 0); x <= std::min(x1, static_cast<int>(kWidth) - 1); ++x)
						px[static_cast<size_t>(y) * kWidth + x] = c;
			}
			void RoundRect(int x0, int y0, int x1, int y1, int radius, u32 fill, u32 edge, int border)
			{
				for (int y = y0; y <= y1; ++y)
				{
					for (int x = x0; x <= x1; ++x)
					{
						const float cx = std::clamp(static_cast<float>(x), static_cast<float>(x0 + radius), static_cast<float>(x1 - radius));
						const float cy = std::clamp(static_cast<float>(y), static_cast<float>(y0 + radius), static_cast<float>(y1 - radius));
						const float dx = static_cast<float>(x) - cx, dy = static_cast<float>(y) - cy;
						const float d = std::sqrt(dx * dx + dy * dy);
						if (d > static_cast<float>(radius) + 0.5f)
							continue;
						const bool on_edge = (d > static_cast<float>(radius - border) + 0.5f) || x < x0 + border || x > x1 - border ||
											 y < y0 + border || y > y1 - border;
						Put(x, y, on_edge ? edge : fill);
					}
				}
			}
			void Disc(Pt c, float r, u32 colour)
			{
				const int x0 = static_cast<int>(std::floor(c.x - r)), x1 = static_cast<int>(std::ceil(c.x + r));
				const int y0 = static_cast<int>(std::floor(c.y - r)), y1 = static_cast<int>(std::ceil(c.y + r));
				for (int y = y0; y <= y1; ++y)
					for (int x = x0; x <= x1; ++x)
					{
						const float dx = static_cast<float>(x) - c.x, dy = static_cast<float>(y) - c.y;
						if (dx * dx + dy * dy <= r * r)
							Put(x, y, colour);
					}
			}
			void ThickLine(Pt a, Pt b, float hw, u32 colour)
			{
				const float dx = b.x - a.x, dy = b.y - a.y;
				const float len = std::sqrt(dx * dx + dy * dy);
				const int steps = std::max(1, static_cast<int>(std::ceil(len)));
				for (int i = 0; i <= steps; ++i)
				{
					const float s = static_cast<float>(i) / static_cast<float>(steps);
					Disc({a.x + dx * s, a.y + dy * s}, hw, colour);
				}
			}
			void Arc(int slot, float t0, float t1, float radius, float hw, u32 colour)
			{
				constexpr int kSegments = 48;
				Pt prev = TipAt(slot, t0, radius);
				for (int i = 1; i <= kSegments; ++i)
				{
					const float t = t0 + (t1 - t0) * static_cast<float>(i) / static_cast<float>(kSegments);
					const Pt next = TipAt(slot, t, radius);
					ThickLine(prev, next, hw, colour);
					prev = next;
				}
			}
		};

		struct Glyph
		{
			char ch;
			const char* rows[7];
		};
		constexpr Glyph kFont[] = {
			{'0', {" ### ", "#   #", "#  ##", "# # #", "##  #", "#   #", " ### "}},
			{'1', {"  #  ", " ##  ", "  #  ", "  #  ", "  #  ", "  #  ", " ### "}},
			{'2', {" ### ", "#   #", "    #", "   # ", "  #  ", " #   ", "#####"}},
			{'3', {"#####", "   # ", "  #  ", "   # ", "    #", "#   #", " ### "}},
			{'4', {"   # ", "  ## ", " # # ", "#  # ", "#####", "   # ", "   # "}},
			{'5', {"#####", "#    ", "#### ", "    #", "    #", "#   #", " ### "}},
			{'6', {"  ## ", " #   ", "#    ", "#### ", "#   #", "#   #", " ### "}},
			{'7', {"#####", "    #", "   # ", "  #  ", " #   ", " #   ", " #   "}},
			{'8', {" ### ", "#   #", "#   #", " ### ", "#   #", "#   #", " ### "}},
			{'9', {" ### ", "#   #", "#   #", " ####", "    #", "   # ", " ##  "}},
			{'+', {"     ", "  #  ", "  #  ", "#####", "  #  ", "  #  ", "     "}},
			{'-', {"     ", "     ", "     ", "#####", "     ", "     ", "     "}},
			{'.', {"     ", "     ", "     ", "     ", "     ", " ##  ", " ##  "}},
			{'F', {"#####", "#    ", "#    ", "#### ", "#    ", "#    ", "#    "}},
			{'W', {"#   #", "#   #", "#   #", "# # #", "# # #", "## ##", "#   #"}},
			{'D', {"#### ", "#   #", "#   #", "#   #", "#   #", "#   #", "#### "}},
			{'R', {"#### ", "#   #", "#   #", "#### ", "# #  ", "#  # ", "#   #"}},
			{'E', {"#####", "#    ", "#    ", "#### ", "#    ", "#    ", "#####"}},
			{'V', {"#   #", "#   #", "#   #", "#   #", " # # ", " # # ", "  #  "}},
			{'I', {" ### ", "  #  ", "  #  ", "  #  ", "  #  ", "  #  ", " ### "}},
			{'L', {"#    ", "#    ", "#    ", "#    ", "#    ", "#    ", "#####"}},
			{'O', {" ### ", "#   #", "#   #", "#   #", "#   #", "#   #", " ### "}},
			{' ', {"     ", "     ", "     ", "     ", "     ", "     ", "     "}},
		};

		const Glyph* FindGlyph(char ch)
		{
			for (const Glyph& g : kFont)
				if (g.ch == ch)
					return &g;
			return nullptr;
		}

		void Text(Canvas& c, int cx, int cy, const char* text, int scale, u32 colour)
		{
			const int n = static_cast<int>(std::strlen(text));
			const int advance = 6 * scale;
			const int width = n * advance - scale;
			int x = cx - width / 2;
			const int y0 = cy - (7 * scale) / 2;
			for (int i = 0; i < n; ++i, x += advance)
			{
				const Glyph* g = FindGlyph(text[i]);
				if (!g)
					continue;
				for (int r = 0; r < 7; ++r)
					for (int col = 0; col < 5; ++col)
						if (g->rows[r][col] == '#')
							c.Rect(x + col * scale, y0 + r * scale, x + col * scale + scale - 1, y0 + r * scale + scale - 1, colour);
			}
		}

		void Hand(Canvas& c, Pt grip, int slot)
		{
			const float inward = -Outward(slot);
			const Pt palm{grip.x + inward * static_cast<float>(kGripHalfW + 8), grip.y + 4.0f};
			c.Disc(palm, static_cast<float>(kPalmRadius + 2), kHandEdge);
			c.Disc(palm, static_cast<float>(kPalmRadius), kColourHand);
			for (int k = 0; k < 4; ++k)
			{
				const float fx = grip.x + inward * static_cast<float>(kGripHalfW - 12 - k * 12);
				const int x0 = static_cast<int>(std::lround(fx)) - kFingerHalfW;
				const int y0 = static_cast<int>(grip.y) - kGripHalfH - 10;
				c.RoundRect(x0 - 1, y0 - 1, x0 + 2 * kFingerHalfW + 1, y0 + kFingerLen + 1, kFingerHalfW + 1, kHandEdge, kHandEdge, 1);
				c.RoundRect(x0, y0, x0 + 2 * kFingerHalfW, y0 + kFingerLen, kFingerHalfW, kColourHand, kColourHand, 1);
			}
		}
	}

	void RasterLever(std::vector<u32>& out, float t, bool grabbed, bool broke_away, int slot)
	{
		out.assign(static_cast<size_t>(kWidth) * kHeight, kTransparent);
		Canvas c{out};
		t = std::clamp(std::isfinite(t) ? t : 0.0f, -1.0f, 1.0f);
		slot = (slot == kRight) ? kRight : kLeft;
		if (broke_away)
			grabbed = false;

		const u32 border = broke_away ? kColourBreakAway : (grabbed ? kBorderGrab : kBorderIdle);
		c.RoundRect(kCardInset, kCardInset, kWidth - 1 - kCardInset, kHeight - 1 - kCardInset, kCardRadius,
			grabbed ? kCardGrab : kCardIdle, border, broke_away ? 4 : (grabbed ? 3 : 2));

		c.Arc(slot, -1.0f, 1.0f, kArmLen, 2.0f, kTrack);
		for (int i = -4; i <= 4; ++i)
		{
			const float ti = static_cast<float>(i) / 4.0f;
			const bool idle = (i == 0);
			const float len = idle ? kIdleTickLen : kTickLen;
			c.ThickLine(TipAt(slot, ti, kArmLen - len * 0.5f), TipAt(slot, ti, kArmLen + len * 0.5f), idle ? 2.0f : 1.5f,
				idle ? kIdleTick : kTick);
		}
		const Pt tip_fwd = TipAt(slot, 1.0f), tip_rev = TipAt(slot, -1.0f), tip_idle = TipAt(slot, 0.0f);
		Text(c, static_cast<int>(tip_fwd.x), 30, "FWD", 2, kText);
		Text(c, static_cast<int>(tip_rev.x), kHeight - 30, "REV", 2, kTextDim);
		Text(c, static_cast<int>(tip_idle.x - Outward(slot) * 58.0f), kPivotY + 16, "IDLE", 1, kTextDim);

		const Pt pivot = PivotOf(slot);
		const Pt tip = TipAt(slot, t);
		c.Disc(pivot, 10.0f, kPivotRing);
		c.Disc(pivot, 7.0f, kPivot);
		c.ThickLine(pivot, tip, kArmHalfWidth, grabbed ? kArmGrab : kArmIdle);
		const int gx = static_cast<int>(std::lround(tip.x)), gy = static_cast<int>(std::lround(tip.y));
		c.RoundRect(gx - kGripHalfW, gy - kGripHalfH, gx + kGripHalfW - 1, gy + kGripHalfH - 1, kGripRadius,
			grabbed ? kColourGripGrab : kColourGripIdle, grabbed ? kGripEdgeGrab : kGripEdgeIdle, 2);
		char label[8];
		std::snprintf(label, sizeof(label), "%+.2f", t);
		Text(c, gx, gy, label, 2, kGripText);

		if (grabbed)
		{
			Hand(c, {static_cast<float>(gx), static_cast<float>(gy)}, slot);
		}
		else if (broke_away)
		{
			const float ax = pivot.x - tip.x, ay = pivot.y - tip.y;
			const float al = std::max(std::sqrt(ax * ax + ay * ay), 1.0f);
			const Pt off{tip.x + ax / al * 70.0f, tip.y + ay / al * 70.0f};
			Hand(c, off, slot);
			Text(c, static_cast<int>(off.x), static_cast<int>(off.y) - kGripHalfH - 18, "OFF", 1, kColourBreakAway);
		}
	}
}
