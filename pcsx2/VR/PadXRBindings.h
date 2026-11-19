// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "VR/PadXRActions.h"

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace VR::PadXRBindings
{
	inline constexpr const char* kSettingsSection = "PadXRBindings";
	inline constexpr int kMaxPathsPerSlot = 2;

	struct ProfileInfo
	{
		const char* id;
		const char* path;
		const char* title;
	};
	const std::vector<ProfileInfo>& Profiles();

	std::string SlotKey(std::string_view profile_path, int action, int hand);

	std::string Encode(const std::vector<std::string>& subpaths);
	std::optional<std::vector<std::string>> Decode(std::string_view value);

	struct LoadReport
	{
		int overridden = 0;
		std::vector<std::string> rejected;
	};

	class Model
	{
	public:
		Model();

		void ResetToDefaults();

		const std::vector<std::string>& Paths(std::string_view profile_path, int action, int hand) const;
		bool IsDefault(std::string_view profile_path, int action, int hand) const;

		bool SetPaths(std::string_view profile_path, int action, int hand, std::vector<std::string> subpaths,
			std::string* error = nullptr);

		std::vector<std::string> Choices(std::string_view profile_path, int action, int hand) const;

		std::vector<PadXRActions::Binding> BindingsFor(std::string_view profile_path) const;

		using Getter = std::function<std::optional<std::string>(const std::string& key)>;
		LoadReport Load(const Getter& get);

		using Setter = std::function<void(const std::string& key, const std::optional<std::string>& value)>;
		void Save(const Setter& set) const;

	private:
		std::map<std::string, std::vector<std::string>> m_slots;
		std::map<std::string, std::vector<std::string>> m_defaults;
	};
}
