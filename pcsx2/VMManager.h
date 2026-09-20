// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/Pcsx2Defs.h"

#include "Config.h"

enum class CDVD_SourceType : uint8_t;

enum class VMState
{
	Shutdown,
	Initializing,
	Running,
	Paused,
	Resetting,
	Stopping,
};

struct VMBootParameters
{
	std::string filename;
	std::string elf_override;
	std::string save_state;
	std::optional<s32> state_index;
	std::optional<CDVD_SourceType> source_type;

	std::optional<bool> fast_boot;
	std::optional<bool> fullscreen;
	std::optional<bool> start_turbo;
	std::optional<bool> start_unlimited;
	bool disable_achievements_hardcore_mode = false;
};

enum class VMBootResult
{
	StartupSuccess,
	StartupFailure,
	PromptDisableHardcoreMode
};

using VMBootRestartCallback = std::function<void()>;

using VMBootHardcoreDisableCallback = std::function<void(std::string reason, VMBootRestartCallback restart_callback)>;

using VMBootDoneCallback = std::function<void(VMBootResult result, const Error& error)>;

namespace VMManager
{
	static constexpr s32 NUM_SAVE_STATE_SLOTS = 10;

	static constexpr std::size_t EMU_THREAD_STACK_SIZE = 2 * 1024 * 1024;

	bool PerformEarlyHardwareChecks(const char** error);

	VMState GetState();

	void SetState(VMState state);

	bool HasValidVM();

	std::string GetDiscPath();

	std::string GetDiscSerial();

	std::string GetDiscELF();

	std::string GetTitle(bool prefer_en);

	u32 GetDiscCRC();

	std::string GetDiscVersion();

	u32 GetCurrentCRC();

	const std::string& GetCurrentELF();

	void InitializeAsync(
		const VMBootParameters& boot_params,
		VMBootHardcoreDisableCallback hardcore_disable_callback,
		VMBootDoneCallback done_callback);

	VMBootResult Initialize(const VMBootParameters& boot_params, Error* error = nullptr);

	void Shutdown(bool save_resume_state);

	bool RequestReset();

	void Reset();

	void Execute();

	void IdlePollUpdate();

	void SetPaused(bool paused);

	void ApplySettings();

	bool ReloadGameSettings();

	void ReloadPatches(bool reload_files, bool reload_enabled_list, bool verbose, bool verbose_if_changed);

	void ReloadInputSources();

	void ReloadInputBindings(bool force = false);

	std::string GetSaveStateFileName(const char* game_serial, u32 game_crc, s32 slot, bool backup = false);

	std::string GetSaveStateFileName(const char* filename, s32 slot, bool backup = false);

	bool HasSaveStateInSlot(const char* game_serial, u32 game_crc, s32 slot);

	bool LoadState(const char* filename, Error* error = nullptr);

	bool LoadStateFromSlot(s32 slot, bool backup = false, Error* error = nullptr);

	void SaveState(const char* filename, bool zip_on_thread, bool backup_old_state,
		std::function<void(const std::string&)> error_callback);

	void SaveStateToSlot(s32 slot, bool zip_on_thread, std::function<void(const std::string&)> error_callback);

	void WaitForSaveStateFlush();

	u32 DeleteSaveStates(const char* game_serial, u32 game_crc, bool also_backups = true);

	LimiterModeType GetLimiterMode();

	void SetLimiterMode(LimiterModeType type);

	float GetTargetSpeed();

	void UpdateTargetSpeed();

	bool IsTargetSpeedAdjustedToHost();

	float GetFrameRate();

	GSVSyncMode GetEffectiveVSyncMode();

	bool ShouldAllowPresentThrottle();

	void FrameAdvance(u32 num_frames = 1);

	bool ChangeDisc(CDVD_SourceType source, std::string path);

	bool SetELFOverride(std::string path);

	bool ChangeGSDump(const std::string& path);

	bool IsElfFileName(const std::string_view path);

	bool IsBlockDumpFileName(const std::string_view path);

	bool IsGSDumpFileName(const std::string_view path);

	bool IsSaveStateFileName(const std::string_view path);

	bool IsDiscFileName(const std::string_view path);

	bool IsLoadableFileName(const std::string_view path);

	std::string GetSerialForGameSettings();

	std::string GetGameSettingsPath(const std::string_view game_serial, u32 game_crc);

	std::string GetDiscOverrideFromGameSettings(const std::string& elf_path);

	std::string GetInputProfilePath(const std::string_view name);

	std::string GetDebuggerSettingsFilePath(const std::string_view game_serial, u32 game_crc);

	std::string GetDebuggerSettingsFilePathForCurrentGame();

	void RequestDisplaySize(float scale = 0.0f);

	void SetDefaultSettings(SettingsInterface& si, bool folders, bool core, bool controllers, bool hotkeys, bool ui);

	u64 GetSessionPlayedTime();

	void UpdateDiscordPresence(bool update_session_time);

	bool WriteBytesToEESIORXFIFO(const std::span<const u8> data);

	namespace Internal
	{
		bool CheckSettingsVersion();

		void LoadStartupSettings();

		void SetFileLogPath(std::string path);

		void SetBlockSystemConsole(bool block);

		bool CPUThreadInitialize();

		void CPUThreadShutdown();

		void ResetVMHotkeyState();

		void UpdateEmuFolders();

		bool WasFastBooted();

		bool IsFastBootInProgress();

		void DisableFastBoot();

		bool HasBootedELF();

		u32 GetCurrentELFEntryPoint();

		void FrameRateChanged();

		void Throttle();

		void ClearCPUExecutionCaches();

		const std::vector<u32>& GetSoftwareRendererProcessorList();

		const std::string& GetELFOverride();
		bool IsExecutionInterrupted();
		void ELFLoadingOnCPUThread(std::string elf_path);
		void EntryPointCompilingOnCPUThread();
		void VSyncOnCPUThread();
		void PollInputOnCPUThread();
	}
}


namespace Host
{
	void LoadSettings(SettingsInterface& si, std::unique_lock<std::mutex>& lock);

	void CheckForSettingsChanges(const Pcsx2Config& old_config);

	void OnVMStarting();

	void OnVMStarted();

	void OnVMDestroyed();

	void OnVMPaused();

	void OnVMResumed();

	void OnPerformanceMetricsUpdated();

	void OnSaveStateLoading(const std::string_view filename);

	void OnSaveStateLoaded(const std::string_view filename, bool was_successful);

	void OnSaveStateSaved(const std::string_view filename);

	void OnGameChanged(const std::string& title, const std::string& elf_override, const std::string& disc_path,
		const std::string& disc_serial, u32 disc_crc, u32 current_crc);

	void PumpMessagesOnCPUThread();
}
