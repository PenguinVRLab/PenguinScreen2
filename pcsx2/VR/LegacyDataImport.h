// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include <string>

namespace VR::LegacyDataImport
{
	static constexpr const char* OLD_APP_ID = "org.penguinvr.penguinscreen2";
	static constexpr const char* NEW_APP_ID = "com.penguinvrlabs.penguinscreen2";

	static constexpr const char* SETTINGS_REL = "config/PenguinScreen2/inis/PenguinScreen2.ini";
	static constexpr const char* MARKER_REL = "config/PenguinScreen2/MIGRATED-FROM-org.penguinvr.penguinscreen2.txt";
	static constexpr const char* PART_SUFFIX = ".rc2-import.part";

	struct Result
	{
		bool imported = false;
		int files_copied = 0;
		int files_kept = 0;
		int files_failed = 0;
		int inis_repointed = 0;
		std::string reason;
	};

	Result Import(const std::string& old_app_dir, const std::string& new_app_dir);

	void ImportFromRc2IfNeeded();
}
