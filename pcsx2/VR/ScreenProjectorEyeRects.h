// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "VR/screenproj/vr_screen_projector.h"

#include <algorithm>
#include <cmath>

namespace VR::ScreenProjector
{
	inline vrsp_fov FovOfRect(const vrsp_fov& located, int width, int height, const vrsp_rect& r)
	{
		const double tl = std::tan(static_cast<double>(located.angle_left));
		const double tr = std::tan(static_cast<double>(located.angle_right));
		const double tu = std::tan(static_cast<double>(located.angle_up));
		const double td = std::tan(static_cast<double>(located.angle_down));
		const double px = (tr - tl) / width, py = (tu - td) / height;
		vrsp_fov f;
		f.angle_left = static_cast<float>(std::atan(tl + r.x * px));
		f.angle_right = static_cast<float>(std::atan(tl + (r.x + r.w) * px));
		f.angle_up = static_cast<float>(std::atan(tu - r.y * py));
		f.angle_down = static_cast<float>(std::atan(tu - (r.y + r.h) * py));
		return f;
	}

	inline bool MatchEyeRects(const vrsp_fov located[2], int width, int height, vrsp_rect rect[2], vrsp_fov declared[2])
	{
#ifdef VR_EYE_RECTS_PLANTED_FAULT_NO_MATCH
		if (rect[0].w > 0 && rect[1].w > 0)
			return false;
#endif
		const int w = std::max(rect[0].w, rect[1].w);
		const int h = std::max(rect[0].h, rect[1].h);
		if (w <= 0 || h <= 0)
		{
			for (int eye = 0; eye < 2; eye++)
			{
				rect[eye] = {0, 0, width, height};
				declared[eye] = located[eye];
			}
			return true;
		}
		bool changed = false;
		for (int eye = 0; eye < 2; eye++)
		{
			vrsp_rect& r = rect[eye];
			if (r.w == w && r.h == h)
				continue;
			r.x = std::min(r.x, width - w);
			r.y = std::min(r.y, height - h);
			r.w = w;
			r.h = h;
			declared[eye] = FovOfRect(located[eye], width, height, r);
			changed = true;
		}
		return changed;
	}
}
