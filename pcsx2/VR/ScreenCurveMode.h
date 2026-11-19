// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include <optional>
#include <string_view>

namespace VR::ScreenCurve
{
	enum class Fallback
	{
		Flat,
		Facets,
		Projector,
	};

	enum class Force
	{
		None,
		Facets,
		Projector,
	};

	enum class Mode
	{
		Flat,
		NativeCylinder,
		Facets,
		Projector,
	};

	inline constexpr float kCurveThresholdDeg = 5.0f;

	std::optional<Fallback> ParseFallback(std::string_view s);
	std::optional<Force> ParseForce(std::string_view s);

	const char* FallbackName(Fallback f);
	const char* ForceName(Force f);
	const char* ModeName(Mode m);

	struct Decision
	{
		Mode mode = Mode::Flat;
		bool facets_unavailable = false;
	};

	Decision Decide(float arc_deg, bool has_cylinder_ext, Fallback fallback, Force force, bool facets_available = false);
}
