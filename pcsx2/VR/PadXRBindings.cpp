// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#include "VR/PadXRBindings.h"

#include <algorithm>

namespace VR::PadXRBindings
{
	namespace PA = VR::PadXRActions;

	namespace
	{
		const std::vector<ProfileInfo> s_profiles = {
			{"touch", PA::kTouchProfile, "Oculus / Meta Touch"},
			{"simple", PA::kSimpleProfile, "Khronos simple controller"},
			{"frame", PA::kFrameProfile, "Steam Frame (UNTESTED)"},
		};

		const char* HandName(int hand)
		{
			return hand == 0 ? "left" : hand == 1 ? "right" : nullptr;
		}

		const char* HandPath(int hand)
		{
			return hand == 0 ? PA::kLeftHand : PA::kRightHand;
		}

		const ProfileInfo* FindProfile(std::string_view path)
		{
			for (const ProfileInfo& p : s_profiles)
			{
				if (path == p.path)
					return &p;
			}
			return nullptr;
		}

		bool Valid(std::string_view profile, int action, int hand)
		{
			return FindProfile(profile) && action >= 0 && action < PA::ActionCount && (hand == 0 || hand == 1);
		}

		const std::vector<std::string> s_empty;
	}

	const std::vector<ProfileInfo>& Profiles()
	{
		return s_profiles;
	}

	std::string SlotKey(std::string_view profile_path, int action, int hand)
	{
		if (!Valid(profile_path, action, hand))
			return {};
		return std::string(FindProfile(profile_path)->id) + "." + PA::Actions()[action].name + "." + HandName(hand);
	}

	std::string Encode(const std::vector<std::string>& subpaths)
	{
		if (subpaths.empty())
			return "none";
		std::string out;
		for (size_t i = 0; i < subpaths.size(); i++)
		{
			if (i)
				out += ';';
			out += subpaths[i];
		}
		return out;
	}

	std::optional<std::vector<std::string>> Decode(std::string_view value)
	{
		if (value == "none")
			return std::vector<std::string>{};
		std::vector<std::string> out;
		size_t start = 0;
		while (start <= value.size())
		{
			const size_t end = std::min(value.find(';', start), value.size());
			const std::string_view part = value.substr(start, end - start);
			if (part.empty() || part.front() != '/')
				return std::nullopt;
			out.emplace_back(part);
			start = end + 1;
		}
		if (out.empty() || out.size() > static_cast<size_t>(kMaxPathsPerSlot))
			return std::nullopt;
		return out;
	}

	Model::Model()
	{
		for (const ProfileInfo& p : s_profiles)
		{
			for (int a = 0; a < PA::ActionCount; a++)
			{
				for (int h = 0; h < 2; h++)
					m_defaults[SlotKey(p.path, a, h)];
			}
			const std::string left = PA::kLeftHand;
			for (const PA::Binding& b : PA::DefaultPadXRBindings(p.path))
			{
				const int a = PA::FindAction(b.action);
				const int h = b.path.compare(0, left.size(), left) == 0 ? 0 : 1;
				const size_t prefix = std::string(HandPath(h)).size();
				m_defaults[SlotKey(p.path, a, h)].push_back(b.path.substr(prefix));
			}
		}
		m_slots = m_defaults;
	}

	void Model::ResetToDefaults()
	{
		m_slots = m_defaults;
	}

	const std::vector<std::string>& Model::Paths(std::string_view profile_path, int action, int hand) const
	{
		const auto it = m_slots.find(SlotKey(profile_path, action, hand));
		return it == m_slots.end() ? s_empty : it->second;
	}

	bool Model::IsDefault(std::string_view profile_path, int action, int hand) const
	{
		const std::string key = SlotKey(profile_path, action, hand);
		const auto a = m_slots.find(key), d = m_defaults.find(key);
		return a != m_slots.end() && d != m_defaults.end() && a->second == d->second;
	}

	bool Model::SetPaths(std::string_view profile_path, int action, int hand, std::vector<std::string> subpaths,
		std::string* error)
	{
		const auto fail = [&](std::string why) {
			if (error)
				*error = std::move(why);
			return false;
		};
		if (!Valid(profile_path, action, hand))
			return fail("unknown profile, action or hand");
		if (subpaths.size() > static_cast<size_t>(kMaxPathsPerSlot))
			return fail("more than " + std::to_string(kMaxPathsPerSlot) + " inputs");
		const PA::Component::Kind kind = PA::KindForAction(PA::Actions()[action].name);
		for (size_t i = 0; i < subpaths.size(); i++)
		{
			if (std::find(subpaths.begin(), subpaths.begin() + i, subpaths[i]) != subpaths.begin() + i)
				return fail(subpaths[i] + " repeats");
			if (!PA::PathAllowed(profile_path, std::string(HandPath(hand)) + subpaths[i], kind))
				return fail(subpaths[i] + " is not an input this controller offers for this action");
		}
		m_slots[SlotKey(profile_path, action, hand)] = std::move(subpaths);
		return true;
	}

	std::vector<std::string> Model::Choices(std::string_view profile_path, int action, int hand) const
	{
		std::vector<std::string> out;
		if (!Valid(profile_path, action, hand))
			return out;
		const PA::Component::Kind kind = PA::KindForAction(PA::Actions()[action].name);
		for (const PA::Component& c : PA::ProfileComponents(profile_path, hand))
		{
			if (PA::PathAllowed(profile_path, std::string(HandPath(hand)) + c.subpath, kind))
				out.emplace_back(c.subpath);
		}
		return out;
	}

	std::vector<PA::Binding> Model::BindingsFor(std::string_view profile_path) const
	{
		std::vector<PA::Binding> out;
		if (!FindProfile(profile_path))
			return out;
		for (int a = 0; a < PA::ActionCount; a++)
		{
			for (int h = 0; h < 2; h++)
			{
				for (const std::string& sub : Paths(profile_path, a, h))
					out.push_back({PA::Actions()[a].name, std::string(HandPath(h)) + sub});
			}
		}
		return out;
	}

	LoadReport Model::Load(const Getter& get)
	{
		LoadReport report;
		m_slots = m_defaults;
		for (const ProfileInfo& p : s_profiles)
		{
			for (int a = 0; a < PA::ActionCount; a++)
			{
				for (int h = 0; h < 2; h++)
				{
					const std::string key = SlotKey(p.path, a, h);
					const std::optional<std::string> value = get(key);
					if (!value)
						continue;
					const std::optional<std::vector<std::string>> paths = Decode(*value);
					if (!paths)
					{
						report.rejected.push_back(key + "=" + *value + ": not a binding list");
						continue;
					}
#ifdef VR_PADXR_PLANTED_BINDINGS_ACCEPT_ANY
					m_slots[key] = *paths;
					report.overridden++;
#else
					std::string why;
					if (!SetPaths(p.path, a, h, *paths, &why))
					{
						report.rejected.push_back(key + "=" + *value + ": " + why);
						continue;
					}
					report.overridden++;
#endif
				}
			}
		}
		return report;
	}

	void Model::Save(const Setter& set) const
	{
		for (const auto& [key, paths] : m_slots)
		{
			const auto d = m_defaults.find(key);
			if (d != m_defaults.end() && d->second == paths)
				set(key, std::nullopt);
			else
				set(key, Encode(paths));
		}
	}
}
