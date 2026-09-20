// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <string>
#include <string_view>
#include <vector>

namespace Path
{
	std::string ToNativePath(const std::string_view path);
	void ToNativePath(std::string* path);

	std::string BuildRelativePath(const std::string_view filename, const std::string_view new_filename);

	std::string Combine(const std::string_view base, const std::string_view next);

	std::string Canonicalize(const std::string_view path);
	void Canonicalize(std::string* path);

	std::string SanitizeFileName(const std::string_view str, bool strip_slashes = true);
	void SanitizeFileName(std::string* str, bool strip_slashes = true);

	bool IsValidFileName(const std::string_view str, bool allow_slashes = false);

	bool IsAbsolute(const std::string_view path);

	std::string RealPath(const std::string_view path);

	std::string MakeRelative(const std::string_view path, const std::string_view relative_to);

	std::string_view GetExtension(const std::string_view path);

	std::string_view StripExtension(const std::string_view path);

	std::string ReplaceExtension(const std::string_view path, const std::string_view new_extension);

	std::string_view GetDirectory(const std::string_view path);

	std::string_view GetFileName(const std::string_view path);

	std::string_view GetFileTitle(const std::string_view path);

	std::string ChangeFileName(const std::string_view path, const std::string_view new_filename);
	void ChangeFileName(std::string* path, const std::string_view new_filename);

	std::string AppendDirectory(const std::string_view path, const std::string_view new_dir);
	void AppendDirectory(std::string* path, const std::string_view new_dir);

	std::vector<std::string_view> SplitWindowsPath(const std::string_view path);
	std::string JoinWindowsPath(const std::vector<std::string_view>& components);

	std::vector<std::string_view> SplitNativePath(const std::string_view path);
	std::string JoinNativePath(const std::vector<std::string_view>& components);

	std::string URLEncode(std::string_view str);

	std::string URLDecode(std::string_view str);

	std::string CreateFileURL(std::string_view path);
}
