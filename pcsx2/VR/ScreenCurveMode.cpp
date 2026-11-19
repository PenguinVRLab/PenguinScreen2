// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#include "VR/ScreenCurveMode.h"

#include <cctype>

namespace VR::ScreenCurve
{
	namespace
	{
		bool EqualsNoCase(std::string_view a, std::string_view b)
		{
			if (a.size() != b.size())
				return false;
			for (size_t i = 0; i < a.size(); i++)
			{
				if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
					return false;
			}
			return true;
		}
	}

	std::optional<Fallback> ParseFallback(std::string_view s)
	{
		if (EqualsNoCase(s, "flat"))
			return Fallback::Flat;
		if (EqualsNoCase(s, "facets"))
			return Fallback::Facets;
		if (EqualsNoCase(s, "projector"))
			return Fallback::Projector;
		return std::nullopt;
	}

	std::optional<Force> ParseForce(std::string_view s)
	{
		if (s.empty() || EqualsNoCase(s, "none"))
			return Force::None;
		if (EqualsNoCase(s, "facets"))
			return Force::Facets;
		if (EqualsNoCase(s, "projector"))
			return Force::Projector;
		return std::nullopt;
	}

	const char* FallbackName(Fallback f)
	{
		switch (f)
		{
			case Fallback::Flat: return "flat";
			case Fallback::Facets: return "facets";
			case Fallback::Projector: return "projector";
		}
		return "?";
	}

	const char* ForceName(Force f)
	{
		switch (f)
		{
			case Force::None: return "none";
			case Force::Facets: return "facets";
			case Force::Projector: return "projector";
		}
		return "?";
	}

	const char* ModeName(Mode m)
	{
		switch (m)
		{
			case Mode::Flat: return "flat";
			case Mode::NativeCylinder: return "native-cylinder";
			case Mode::Facets: return "facets";
			case Mode::Projector: return "projector";
		}
		return "?";
	}

	Decision Decide(float arc_deg, bool has_cylinder_ext, Fallback fallback, Force force, bool facets_available)
	{
		Decision d;
		if (!(arc_deg >= kCurveThresholdDeg))
			return d;
		if (has_cylinder_ext && force == Force::None)
		{
			d.mode = Mode::NativeCylinder;
			return d;
		}
		Fallback want = fallback;
		if (force == Force::Facets)
			want = Fallback::Facets;
		else if (force == Force::Projector)
			want = Fallback::Projector;
		if (want == Fallback::Facets)
		{
			if (facets_available)
			{
				d.mode = Mode::Facets;
				return d;
			}
			d.facets_unavailable = true;
			want = Fallback::Projector;
		}
		d.mode = (want == Fallback::Projector) ? Mode::Projector : Mode::Flat;
		return d;
	}
}
