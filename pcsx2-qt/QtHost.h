// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <atomic>
#include <memory>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "pcsx2/Host.h"
#include "pcsx2/Input/InputManager.h"
#include "pcsx2/VMManager.h"

#include <QtCore/QList>
#include <QtCore/QEventLoop>
#include <QtCore/QMetaType>
#include <QtCore/QPair>
#include <QtCore/QString>
#include <QtCore/QSemaphore>
#include <QtCore/QString>
#include <QtCore/QTimer>
#include <QtCore/QThread>

#include <QtGui/QIcon>

class SettingsInterface;

class DisplaySurface;
struct VMBootParameters;

enum class CDVD_SourceType : uint8_t;

namespace Achievements
{
	enum class LoginRequestReason;
}

Q_DECLARE_METATYPE(std::shared_ptr<VMBootParameters>);
Q_DECLARE_METATYPE(std::optional<bool>);
Q_DECLARE_METATYPE(GSRendererType);
Q_DECLARE_METATYPE(InputBindingKey);
Q_DECLARE_METATYPE(CDVD_SourceType);
Q_DECLARE_METATYPE(Achievements::LoginRequestReason);

class EmuThread : public QThread
{
	Q_OBJECT

public:
	explicit EmuThread(QThread* ui_thread);
	~EmuThread();

	static void start();
	static void stop();

	__fi QEventLoop* getEventLoop() const { return m_event_loop; }
	__fi bool isFullscreen() const { return m_is_fullscreen; }
	__fi bool isExclusiveFullscreen() const { return m_is_exclusive_fullscreen; }
	__fi bool isRenderingToMain() const { return m_is_rendering_to_main; }
	__fi bool isSurfaceless() const { return m_is_surfaceless; }
	__fi bool isRunningFullscreenUI() const { return m_run_fullscreen_ui.load(std::memory_order_acquire); }

	__fi bool isOnEmuThread() const { return (QThread::currentThread() == this); }
	__fi bool isOnUIThread() const { return (QThread::currentThread() == m_ui_thread); }
	bool shouldRenderToMain() const;

	std::optional<WindowInfo> acquireRenderWindow(bool recreate_window);
	void connectDisplaySignals(DisplaySurface* widget);
	void releaseRenderWindow();

	void startBackgroundControllerPollTimer();
	void stopBackgroundControllerPollTimer();
	void updatePerformanceMetrics(bool force);

public Q_SLOTS:
	void loadSettings(SettingsInterface& si, std::unique_lock<std::mutex>& lock);
	void checkForSettingChanges(const Pcsx2Config& old_config);
	void startFullscreenUI(bool fullscreen);
	void stopFullscreenUI();
	void startVM(std::shared_ptr<VMBootParameters> boot_params);
	void resetVM();
	void setVMPaused(bool paused);
	void shutdownVM(bool save_state = true);
	void loadState(const QString& filename);
	void loadStateFromSlot(qint32 slot, bool load_backup = false);
	void saveState(const QString& filename);
	void saveStateToSlot(qint32 slot);
	void toggleFullscreen();
	void setFullscreen(bool fullscreen, bool allow_render_to_main);
	void setSurfaceless(bool surfaceless);
	void applySettings();
	void reloadGameSettings();
	void updateEmuFolders();
	void toggleSoftwareRendering();
	void changeDisc(CDVD_SourceType source, const QString& path);
	void setELFOverride(const QString& path);
	void changeGSDump(const QString& path);
	void reloadPatches();
	void reloadInputSources();
	void reloadInputBindings();
	void reloadInputDevices();
	void closeInputSources();
	void requestDisplaySize(float scale);
	void enumerateInputDevices();
	void enumerateVibrationMotors();
	void runOnCPUThread(const std::function<void()>& func);
	void queueSnapshot(quint32 gsdump_frames);
	void beginCapture(const QString& path);
	void endCapture();

Q_SIGNALS:
	void statusMessage(const QString& message);

	std::optional<WindowInfo> onAcquireRenderWindowRequested(bool recreate_window, bool fullscreen, bool render_to_main, bool surfaceless);
	void onResizeRenderWindowRequested(qint32 width, qint32 height);
	void onReleaseRenderWindowRequested();
	void onMouseModeRequested(bool relative_mode, bool hide_cursor);
	void onMouseLockRequested(bool state);
	void onFullscreenUIStateChange(bool running);

	void onVMStarting();

	void onVMStarted();

	void onVMPaused();

	void onVMResumed();

	void onVMStopped();

	void onGameChanged(const QString& title, const QString& elf_override, const QString& disc_path,
		const QString& serial, quint32 disc_crc, quint32 crc);

	void onInputDevicesEnumerated(const QList<QPair<QString, QString>>& devices);
	void onInputDeviceConnected(const QString& identifier, const QString& device_name);
	void onInputDeviceDisconnected(const QString& identifier);
	void onVibrationMotorsEnumerated(const QList<InputBindingKey>& motors);

	void onSaveStateLoading(const QString& path);

	void onSaveStateLoaded(const QString& path, bool was_successful);

	void onSaveStateSaved(const QString& path);

	void onAchievementsLoginRequested(Achievements::LoginRequestReason reason);

	void onAchievementsRefreshed(quint32 id, const QString& game_info_string);

	void onAchievementsHardcoreModeChanged(bool enabled);

	void onCaptureStarted(const QString& filename);
	void onCaptureStopped();

protected:
	void run();

private:
	static constexpr u32 BACKGROUND_CONTROLLER_POLLING_INTERVAL = 100;

	static constexpr u32 FULLSCREEN_UI_CONTROLLER_POLLING_INTERVAL = 8;

	void destroyVM();

	void createBackgroundControllerPollTimer();
	void destroyBackgroundControllerPollTimer();
	void connectSignals();

private Q_SLOTS:
	void stopInThread();
	void doBackgroundControllerPoll();
	void onDisplayWindowResized(u32 width, u32 height, float scale);
	void onApplicationStateChanged(Qt::ApplicationState state);
	void redrawDisplayWindow();

private:
	QThread* m_ui_thread;
	QSemaphore m_started_semaphore;
	QEventLoop* m_event_loop = nullptr;
	QTimer* m_background_controller_polling_timer = nullptr;

	std::atomic_bool m_shutdown_flag{false};
	std::atomic_bool m_run_fullscreen_ui{false};

	bool m_verbose_status = false;
	bool m_is_rendering_to_main = false;
	bool m_is_fullscreen = false;
	bool m_is_exclusive_fullscreen = false;
	bool m_is_surfaceless = false;
	bool m_save_state_on_shutdown = false;
	bool m_pause_on_focus_loss = false;

	bool m_was_paused_by_focus_loss = false;

	float m_last_speed = 0.0f;
	float m_last_gpu_usage = 0.0f;
	float m_last_game_fps = 0.0f;
	float m_last_video_fps = 0.0f;
	int m_last_internal_width = 0;
	int m_last_internal_height = 0;
	float m_last_upscale = 0.0f;
	u32 m_last_volume = 0;
	bool m_last_muted = false;
	GSRendererType m_last_renderer = GSRendererType::Auto;
	LimiterModeType m_last_limiter_mode = LimiterModeType::Nominal;
};

extern EmuThread* g_emu_thread;

namespace QtHost
{
	const char* GetDefaultThemeName();

	const char* GetDefaultLanguage();

	void UpdateApplicationTheme();

	bool IsDarkApplicationTheme();

	void SetIconThemeFromStyle();

	bool IsOnUIThread();

	bool ShouldShowAdvancedSettings();

	void RunOnUIThread(const std::function<void()>& func, bool block = false);

	std::vector<std::pair<QString, QString>> GetAvailableLanguageList();

	void InstallTranslator(QWidget* dialog_parent);

	QString GetAppNameAndVersion();

	QString GetAppConfigSuffix();

	QIcon GetAppIcon();

	QString GetResourcesBasePath();

	std::string GetRuntimeDownloadedResourceURL(std::string_view name);

	bool SaveGameSettings(SettingsInterface* sif, bool delete_if_empty);

	std::optional<bool> DownloadFile(QWidget* parent, const QString& title, std::string url, std::vector<u8>* data);

	bool DownloadFile(QWidget* parent, const QString& title, std::string url, const std::string& path);

	bool IsVMValid();
	bool IsVMPaused();

	const QString& GetCurrentGameTitle();
	const QString& GetCurrentGameSerial();
	const QString& GetCurrentGamePath();

	int LocaleSensitiveCompare(QStringView lhs, QStringView rhs);

	void LockVMWithDialog();
	void UnlockVMWithDialog();
}
