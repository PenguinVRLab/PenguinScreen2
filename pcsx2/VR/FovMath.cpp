// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#include "VR/FovMath.h"

#include "fmt/format.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace VR::FovMath
{
	namespace
	{
		constexpr float PI_F = 3.14159265358979323846f;

		float DegToRad(float d) { return d * (PI_F / 180.0f); }
		float RadToDeg(float r) { return r * (180.0f / PI_F); }

		float HalfTan(float deg) { return std::tan(DegToRad(deg) * 0.5f); }

		float Nan() { return std::numeric_limits<float>::quiet_NaN(); }
	}

	bool Fit(ProfileDB::FovCalibration& cal, std::string* why)
	{
		cal.k = 0.0f;
		cal.fit_error_deg = 0.0f;

		if (cal.pairs.size() < 2)
		{
			if (why)
				*why = "a calibration needs at least two measured (constant, degrees) pairs";
			return false;
		}

		double sum_k = 0.0;
		for (const auto& [constant, deg] : cal.pairs)
		{
			if (!std::isfinite(constant) || !std::isfinite(deg))
			{
				if (why)
					*why = "a calibration pair is not a finite number";
				return false;
			}
			if (deg < kMinFovDeg || deg > kMaxFovDeg)
			{
				if (why)
					*why = fmt::format("a calibration pair's field of view {:.3f}deg is outside {:.0f}-{:.0f}",
						deg, kMinFovDeg, kMaxFovDeg);
				return false;
			}
			const float t = HalfTan(deg);
			if (t <= 0.0f)
			{
				if (why)
					*why = "a calibration pair's field of view has no usable tangent";
				return false;
			}
			switch (cal.model)
			{
				case ProfileDB::FovModel::Tan:
					sum_k += static_cast<double>(constant) / static_cast<double>(t);
					break;
				case ProfileDB::FovModel::InvTan:
					if (constant == 0.0f)
					{
						if (why)
							*why = "a calibration pair has constant 0, which the invtan model cannot use";
						return false;
					}
					sum_k += static_cast<double>(constant) * static_cast<double>(t);
					break;
			}
		}

		const double k = sum_k / static_cast<double>(cal.pairs.size());
		if (!std::isfinite(k) || k == 0.0)
		{
			if (why)
				*why = "the calibration pairs do not give a usable fit";
			return false;
		}
		cal.k = static_cast<float>(k);

		float worst = 0.0f;
		for (const auto& [constant, deg] : cal.pairs)
		{
			const float predicted = DegFromConstant(cal, constant);
			if (!std::isfinite(predicted))
			{
				if (why)
					*why = "the fitted calibration cannot reproduce its own pairs";
				cal.k = 0.0f;
				return false;
			}
			worst = std::max(worst, std::fabs(predicted - deg));
		}
		cal.fit_error_deg = worst;
		return true;
	}

	float DegFromConstant(const ProfileDB::FovCalibration& cal, float constant)
	{
		if (cal.k == 0.0f || !std::isfinite(cal.k) || !std::isfinite(constant))
			return Nan();

		switch (cal.model)
		{
			case ProfileDB::FovModel::Tan:
			{
				const float t = constant / cal.k;
				if (!(t > 0.0f) || !std::isfinite(t))
					return Nan();
				return RadToDeg(2.0f * std::atan(t));
			}
			case ProfileDB::FovModel::InvTan:
			{
				if (constant == 0.0f)
					return Nan();
				const float t = cal.k / constant;
				if (!(t > 0.0f) || !std::isfinite(t))
					return Nan();
				return RadToDeg(2.0f * std::atan(t));
			}
		}
		return Nan();
	}

	float ConstantFromDeg(const ProfileDB::FovCalibration& cal, float deg)
	{
		if (cal.k == 0.0f || !std::isfinite(cal.k) || !std::isfinite(deg))
			return Nan();
		if (deg < kMinFovDeg || deg > kMaxFovDeg)
			return Nan();

		const float t = HalfTan(deg);
		if (!(t > 0.0f))
			return Nan();

		switch (cal.model)
		{
			case ProfileDB::FovModel::Tan:
				return cal.k * t;
			case ProfileDB::FovModel::InvTan:
				return cal.k / t;
		}
		return Nan();
	}

	float WidenDeg(float game_deg, float factor)
	{
		if (!std::isfinite(game_deg) || !std::isfinite(factor) || factor <= 0.0f)
			return Nan();
		const float t = HalfTan(std::clamp(game_deg, kMinFovDeg, kMaxFovDeg)) * factor;
		if (!(t > 0.0f) || !std::isfinite(t))
			return Nan();
		return std::clamp(RadToDeg(2.0f * std::atan(t)), kMinFovDeg, kMaxFovDeg);
	}

	float LerpDeg(float from_deg, float to_deg, float t)
	{
		if (!std::isfinite(from_deg) || !std::isfinite(to_deg))
			return Nan();
		const float u = std::clamp(t, 0.0f, 1.0f);
		const float a = HalfTan(std::clamp(from_deg, kMinFovDeg, kMaxFovDeg));
		const float b = HalfTan(std::clamp(to_deg, kMinFovDeg, kMaxFovDeg));
		const float m = a + (b - a) * u;
		if (!(m > 0.0f) || !std::isfinite(m))
			return Nan();
		return RadToDeg(2.0f * std::atan(m));
	}
}
