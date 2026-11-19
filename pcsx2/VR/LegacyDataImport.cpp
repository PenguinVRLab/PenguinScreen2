// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#include "VR/LegacyDataImport.h"

#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/StringUtil.h"

#include "fmt/format.h"

#include <cstdlib>

namespace VR::LegacyDataImport
{
	namespace
	{
		constexpr const char* SUBTREES[] = {"config", "data"};

		bool IsSkipped(const std::string& rel)
		{
			return rel == "config/PenguinScreen2/cache" || rel.starts_with("config/PenguinScreen2/cache/");
		}

		bool IsIni(const std::string& rel)
		{
			return (rel.starts_with("config/PenguinScreen2/") || rel.starts_with("config/PenguinScreen2-players/")) &&
				   rel.ends_with(".ini");
		}

		bool CopyOne(const std::string& src, const std::string& dst, const std::string& rel, Result& r)
		{
			const std::string part = dst + PART_SUFFIX;
			if (FileSystem::FileExists(part.c_str()))
				FileSystem::DeleteFilePath(part.c_str());
			if (FileSystem::FileExists(dst.c_str()))
			{
				r.files_kept++;
				return true;
			}
			const std::string parent(Path::GetDirectory(dst));
			if (!FileSystem::EnsureDirectoryExists(parent.c_str(), true))
			{
				r.files_failed++;
				return false;
			}
			bool written;
			if (IsIni(rel))
			{
				std::optional<std::string> text = FileSystem::ReadFileToString(src.c_str());
				if (!text.has_value())
				{
					r.files_failed++;
					return false;
				}
				const std::string from = fmt::format("/.var/app/{}/", OLD_APP_ID);
				const std::string to = fmt::format("/.var/app/{}/", NEW_APP_ID);
				if (text->find(from) != std::string::npos)
				{
					StringUtil::ReplaceAll(&text.value(), from, to);
					r.inis_repointed++;
				}
				written = FileSystem::WriteStringToFile(part.c_str(), text.value());
			}
			else
			{
				written = FileSystem::CopyFilePath(src.c_str(), part.c_str(), true);
			}
			if (!written || !FileSystem::RenamePath(part.c_str(), dst.c_str()))
			{
				FileSystem::DeleteFilePath(part.c_str());
				r.files_failed++;
				return false;
			}
			r.files_copied++;
			return true;
		}
	}

	Result Import(const std::string& old_app_dir, const std::string& new_app_dir)
	{
		Result r;
		const std::string old_settings = Path::Combine(old_app_dir, SETTINGS_REL);
		const std::string new_settings = Path::Combine(new_app_dir, SETTINGS_REL);
		const std::string marker = Path::Combine(new_app_dir, MARKER_REL);

		if (FileSystem::FileExists(new_settings.c_str()))
		{
			r.reason = "the new install already has settings";
			return r;
		}
		if (FileSystem::FileExists(marker.c_str()))
		{
			r.reason = "already imported once (marker present)";
			return r;
		}
		if (!FileSystem::FileExists(old_settings.c_str()))
		{
			r.reason = "no 1.0-rc2 settings to import";
			return r;
		}

		for (const char* sub : SUBTREES)
		{
			const std::string root = Path::Combine(old_app_dir, sub);
			if (!FileSystem::DirectoryExists(root.c_str()))
				continue;
			FileSystem::FindResultsArray found;
			FileSystem::FindFiles(root.c_str(), "*",
				FILESYSTEM_FIND_RECURSIVE | FILESYSTEM_FIND_RELATIVE_PATHS | FILESYSTEM_FIND_FILES |
					FILESYSTEM_FIND_HIDDEN_FILES | FILESYSTEM_FIND_SORT_BY_NAME,
				&found);
			for (const FILESYSTEM_FIND_DATA& fd : found)
			{
				const std::string rel = fmt::format("{}/{}", sub, fd.FileName);
				if (IsSkipped(rel) || rel == SETTINGS_REL)
					continue;
				CopyOne(Path::Combine(old_app_dir, rel), Path::Combine(new_app_dir, rel), rel, r);
			}
		}

		if (r.files_failed == 0 && CopyOne(old_settings, new_settings, SETTINGS_REL, r))
		{
			r.imported = true;
			FileSystem::WriteStringToFile(marker.c_str(),
				fmt::format("Imported from {} on first launch as {}.\n"
							"Copied {} file(s), kept {} already present, re-pointed {} .ini file(s).\n"
							"The old directory was not changed; delete it yourself once you are happy.\n",
					OLD_APP_ID, NEW_APP_ID, r.files_copied, r.files_kept, r.inis_repointed));
		}
		else
		{
			r.reason = fmt::format("{} file(s) failed to copy; the settings were not imported, so the next launch retries",
				r.files_failed);
		}
		return r;
	}

	void ImportFromRc2IfNeeded()
	{
#ifdef __linux__
		const char* flatpak_id = std::getenv("FLATPAK_ID");
		const char* home = std::getenv("HOME");
		if (!flatpak_id || std::string_view(flatpak_id) != NEW_APP_ID || !home || !*home)
			return;
		const std::string apps = Path::Combine(home, ".var/app");
		const Result r = Import(Path::Combine(apps, OLD_APP_ID), Path::Combine(apps, NEW_APP_ID));
		if (r.imported)
			Console.WriteLnFmt("(VR) Imported 1.0-rc2 data from ~/.var/app/{}: {} file(s) copied, {} kept, {} .ini re-pointed.",
				OLD_APP_ID, r.files_copied, r.files_kept, r.inis_repointed);
		else if (r.files_failed > 0)
			Console.ErrorFmt("(VR) 1.0-rc2 data import incomplete: {}.", r.reason);
		else
			Console.WriteLnFmt("(VR) No 1.0-rc2 data import: {}.", r.reason);
#endif
	}
}
