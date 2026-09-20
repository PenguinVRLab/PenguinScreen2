// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"

#include "Config.h"

#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <utility>
#include <vector>

class Error;

class SaveStateBase;

namespace Achievements
{
	enum class LoginRequestReason
	{
		UserInitiated,
		TokenInvalid,
	};

	std::unique_lock<std::recursive_mutex> GetLock();

	bool Initialize();

	void UpdateSettings(const Pcsx2Config::AchievementsOptions& old_config);

	void ResetClient();

	bool ConfirmSystemReset();

	bool Shutdown(bool allow_cancel);

	void OnVMPaused(bool paused);

	void FrameUpdate();

	void IdleUpdate();

	void LoadState(std::span<const u8> data);
	void SaveState(SaveStateBase& writer);

	bool Login(const char* username, const char* password, Error* error);

	void Logout();

	void GameChanged(u32 disc_crc, u32 crc);

	void PlayAchievementSound(bool is_specific_sound_enabled, const std::string& custom_sound_name, const std::string& default_sound_name);

	bool ResetHardcoreMode(bool is_booting);

	void DisableHardcoreMode();

	const char* GetHardcoreModeDisableTitle();

	std::string GetHardcoreModeDisableText(const char* reason);

	bool IsHardcoreModeActive();

	bool IsUsingRAIntegration();

	bool IsActive();

	bool HasActiveGame();

	u32 GetGameID();

	bool HasAchievementsOrLeaderboards();

	bool HasAchievements();

	bool HasLeaderboards();

	bool HasRichPresence();

	const std::string& GetRichPresenceString();

	const std::string& GetGameIconURL();

	const std::string& GetGameTitle();

	const char* GetLoggedInUserName();

	std::string GetLoggedInUserBadgePath();

	void ClearUIState();

	void DrawGameOverlays();

	void DrawPauseMenuOverlays();

	bool PrepareAchievementsWindow();

	void DrawAchievementsWindow();

	bool PrepareLeaderboardsWindow();

	void DrawLeaderboardsWindow();

#ifdef ENABLE_RAINTEGRATION
	void SwitchToRAIntegration();

	namespace RAIntegration
	{
		void MainWindowChanged(void* new_handle);
		void GameChanged();
		std::vector<std::tuple<int, std::string, bool>> GetMenuItems();
		void ActivateMenuItem(int item);
	}
#endif
}

namespace Host
{
	void OnAchievementsLoginRequested(Achievements::LoginRequestReason reason);

	void OnAchievementsLoginSuccess(const char* display_name, u32 points, u32 sc_points, u32 unread_messages);

	void OnAchievementsRefreshed();

	void OnAchievementsHardcoreModeChanged(bool enabled);
}
