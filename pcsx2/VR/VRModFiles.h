// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"
#include "common/YAML.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace VR::ModFiles
{
	inline constexpr char kShippedDirName[] = "vr-mods";

	enum class Kind
	{
		Fix,
		Conversion,
		Feature,
	};

	struct Feature
	{
		std::string slug;
		u32 version = 0;
	};
	const std::vector<Feature>& Features();
	void SetFeaturesForTest(std::vector<Feature> features);
	std::optional<u32> FeatureVersion(std::string_view slug);

	struct Name
	{
		std::string serial;
		std::string slug;
		std::string version;
		u32 number = 0;
	};

	std::optional<Name> ParseName(std::string_view path, std::string* why);

	struct File
	{
		std::string path;
		bool user = false;
		s64 mtime = 0;
		s64 size = 0;
		std::optional<bool> carried_enabled;
		u32 carried_from = 0;
		std::string differs_from;
		std::string note;
	};

	std::vector<File> ScanAll();

	std::vector<File> Scan(std::vector<std::string>* notes);

	struct Comparator
	{
		enum Op : u8
		{
			GE,
			GT,
			LE,
			LT,
		};
		Op op = GE;
		u32 major = 0, minor = 0, patch = 0;
	};
	struct Range
	{
		std::vector<std::vector<Comparator>> sets;
	};
	std::optional<Range> ParseRange(std::string_view text, std::string* why);
	bool RangeAllows(const Range& range, u32 version);

	struct Dependency
	{
		std::string slug;
		std::string range_text;
		Range range;
	};

	struct Entry
	{
		Kind kind = Kind::Fix;
		bool list_form = false;
		bool enabled = false;
		bool restart_required = true;
		std::vector<Dependency> dependencies;
	};

	std::optional<Entry> CheckEntry(const ryml::ConstNodeRef& root, const Name& name, std::string* why);


	ryml::ConstNodeRef EntryNode(const ryml::ConstNodeRef& root, const Entry& entry);

	inline constexpr char kCannotRewriteEnabled[] = "cannot change this file's enabled: line; write it as `enabled: true` on one line";

	inline constexpr size_t kNameMaxChars = 40;
	inline constexpr size_t kDescriptionMaxChars = 100;
	size_t Utf8Chars(std::string_view text);
	std::optional<std::string> WithEnabled(std::string_view text, bool on);

	bool DiffersBeyondEnabled(std::string_view a, std::string_view b);
}
