// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"
#include "common/SmallString.h"

#include "fmt/format.h"

#include <ctime>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class ProgressCallback;
class SettingsInterface;

namespace Host
{
	static constexpr float OSD_CRITICAL_ERROR_DURATION = 20.0f;
	static constexpr float OSD_ERROR_DURATION = 15.0f;
	static constexpr float OSD_WARNING_DURATION = 10.0f;
	static constexpr float OSD_INFO_DURATION = 5.0f;
	static constexpr float OSD_QUICK_DURATION = 2.5f;

	const char* TranslateToCString(const std::string_view context, const std::string_view msg);

	std::string_view TranslateToStringView(const std::string_view context, const std::string_view msg);

	std::string TranslateToString(const std::string_view context, const std::string_view msg);

	std::string TranslatePluralToString(const char* context, const char* msg, const char* disambiguation, int count);

	void ClearTranslationCache();

	void AddOSDMessage(std::string message, float duration = 2.0f);
	void AddKeyedOSDMessage(std::string key, std::string message, float duration = 2.0f);
	void AddIconOSDMessage(std::string key, const char* icon, const std::string_view message, float duration = 2.0f);
	void RemoveKeyedOSDMessage(std::string key);
	void ClearOSDMessages();

	void ReportInfoAsync(const std::string_view title, const std::string_view message);
	void ReportFormattedInfoAsync(const std::string_view title, const char* format, ...);

	void ReportErrorAsync(const std::string_view title, const std::string_view message);
	void ReportFormattedErrorAsync(const std::string_view title, const char* format, ...);

	bool InBatchMode();

	bool InNoGUIMode();

	void OpenURL(const std::string_view url);

	bool CopyTextToClipboard(const std::string_view text);

	std::string GetTextFromClipboard();

	bool RequestResetSettings(bool folders, bool core, bool controllers, bool hotkeys, bool ui);

	void RequestResizeHostDisplay(s32 width, s32 height);

	void RunOnCPUThread(std::function<void()> function, bool block = false);

	void RunOnGSThread(std::function<void()> function);

	void RefreshGameListAsync(bool invalidate_cache);

	void CancelGameListRefresh();

	void RequestVMShutdown(bool allow_confirm, bool allow_save_state, bool default_save_state);

	std::string GetHTTPUserAgent();

	std::string GetBaseStringSettingValue(const char* section, const char* key, const char* default_value = "");
	SmallString GetBaseSmallStringSettingValue(const char* section, const char* key, const char* default_value = "");
	TinyString GetBaseTinyStringSettingValue(const char* section, const char* key, const char* default_value = "");
	bool GetBaseBoolSettingValue(const char* section, const char* key, bool default_value = false);
	int GetBaseIntSettingValue(const char* section, const char* key, int default_value = 0);
	uint GetBaseUIntSettingValue(const char* section, const char* key, uint default_value = 0);
	float GetBaseFloatSettingValue(const char* section, const char* key, float default_value = 0.0f);
	double GetBaseDoubleSettingValue(const char* section, const char* key, double default_value = 0.0);
	std::vector<std::string> GetBaseStringListSetting(const char* section, const char* key);

	void SetBaseBoolSettingValue(const char* section, const char* key, bool value);
	void SetBaseIntSettingValue(const char* section, const char* key, int value);
	void SetBaseUIntSettingValue(const char* section, const char* key, uint value);
	void SetBaseFloatSettingValue(const char* section, const char* key, float value);
	void SetBaseStringSettingValue(const char* section, const char* key, const char* value);
	void SetBaseStringListSettingValue(const char* section, const char* key, const std::vector<std::string>& values);
	bool AddBaseValueToStringList(const char* section, const char* key, const char* value);
	bool RemoveBaseValueFromStringList(const char* section, const char* key, const char* value);
	bool ContainsBaseSettingValue(const char* section, const char* key);
	void RemoveBaseSettingValue(const char* section, const char* key);
	void CommitBaseSettingChanges();

	std::string GetStringSettingValue(const char* section, const char* key, const char* default_value = "");
	SmallString GetSmallStringSettingValue(const char* section, const char* key, const char* default_value = "");
	TinyString GetTinyStringSettingValue(const char* section, const char* key, const char* default_value = "");
	bool GetBoolSettingValue(const char* section, const char* key, bool default_value = false);
	int GetIntSettingValue(const char* section, const char* key, int default_value = 0);
	uint GetUIntSettingValue(const char* section, const char* key, uint default_value = 0);
	float GetFloatSettingValue(const char* section, const char* key, float default_value = 0.0f);
	double GetDoubleSettingValue(const char* section, const char* key, double default_value = 0.0);
	std::vector<std::string> GetStringListSetting(const char* section, const char* key);

	std::unique_lock<std::mutex> GetSettingsLock();
	std::unique_lock<std::mutex> GetSecretsSettingsLock();
	SettingsInterface* GetSettingsInterface();

	void SetDefaultUISettings(SettingsInterface& si);

	std::unique_ptr<ProgressCallback> CreateHostProgressCallback();

	int LocaleSensitiveCompare(std::string_view lhs, std::string_view rhs);

	namespace Internal
	{
		SettingsInterface* GetBaseSettingsLayer();

		SettingsInterface* GetSecretsSettingsLayer();

		SettingsInterface* GetGameSettingsLayer();

		SettingsInterface* GetInputSettingsLayer();

		void SetBaseSettingsLayer(SettingsInterface* sif);

		void SetSecretsSettingsLayer(SettingsInterface* sif);

		void SetGameSettingsLayer(SettingsInterface* sif, std::unique_lock<std::mutex>& settings_lock);

		void SetInputSettingsLayer(SettingsInterface* sif, std::unique_lock<std::mutex>& settings_lock);

		s32 GetTranslatedStringImpl(const std::string_view context, const std::string_view msg, char* tbuf, size_t tbuf_space);
	}
}

#define TRANSLATE(context, msg) Host::TranslateToCString(context, msg)
#define TRANSLATE_SV(context, msg) Host::TranslateToStringView(context, msg)
#define TRANSLATE_STR(context, msg) Host::TranslateToString(context, msg)
#define TRANSLATE_FS(context, msg) fmt::runtime(Host::TranslateToStringView(context, msg))
#define TRANSLATE_PLURAL_STR(context, msg, disambiguation, count) \
	Host::TranslatePluralToString(context, msg, disambiguation, count)
#define TRANSLATE_PLURAL_FS(context, msg, disambiguation, count) \
	fmt::runtime(Host::TranslatePluralToString(context, msg, disambiguation, count))

#define TRANSLATE_NOOP(context, msg) msg
