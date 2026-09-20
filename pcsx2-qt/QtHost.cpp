// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "AutoUpdaterDialog.h"
#include "Debugger/DebuggerWindow.h"
#include "DisplayWidget.h"
#include "GameList/GameListWidget.h"
#include "LogWindow.h"
#include "MainWindow.h"
#include "QtHost.h"
#include "QtProgressCallback.h"
#include "QtUtils.h"
#include "SetupWizardDialog.h"

#include "pcsx2/CDVD/CDVDcommon.h"
#include "pcsx2/Achievements.h"
#include "pcsx2/BuildVersion.h"
#include "pcsx2/CDVD/CDVD.h"
#include "pcsx2/Counters.h"
#include "pcsx2/DebugTools/Debug.h"
#include "pcsx2/GS.h"
#include "pcsx2/ps2/BiosTools.h"
#include "pcsx2/GS/GS.h"
#include "pcsx2/GSDumpReplayer.h"
#include "pcsx2/GameList.h"
#include "pcsx2/Host.h"
#include "pcsx2/INISettingsInterface.h"
#include "pcsx2/ImGui/FullscreenUI.h"
#include "pcsx2/ImGui/ImGuiFullscreen.h"
#include "pcsx2/ImGui/ImGuiManager.h"
#include "pcsx2/ImGui/ImGuiOverlays.h"
#include "pcsx2/Input/InputManager.h"
#include "pcsx2/MTGS.h"
#include "pcsx2/PerformanceMetrics.h"
#include "pcsx2/SPU2/spu2.h"
#include "pcsx2/VMManager.h"
#ifdef ENABLE_VR
#include "pcsx2/VR/VRManager.h"
#include "pcsx2/VR/VRProfileDB.h"
#endif

#include "common/Assertions.h"
#include "common/Console.h"
#include "common/CrashHandler.h"
#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/HTTPDownloader.h"
#include "common/Path.h"
#include "common/SettingsWrapper.h"
#include "common/StringUtil.h"
#include "common/Timer.h"

#include <QtCore/QDir>
#include <QtCore/QTimer>
#include <QtWidgets/QApplication>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QFileDialog>
#include <QtGui/QClipboard>
#include <QtGui/QInputMethod>

#include "fmt/format.h"

#include <cmath>
#include <csignal>

static constexpr u32 SETTINGS_SAVE_DELAY = 1000;
static constexpr const char* RUNTIME_RESOURCES_URL =
	"https://github.com/PCSX2/pcsx2-windows-dependencies/releases/download/runtime-resources/";

EmuThread* g_emu_thread = nullptr;

namespace QtHost
{
	static void InitializeEarlyConsole();
	static void PrintCommandLineVersion();
	static void PrintCommandLineHelp(const std::string_view progname);
	static std::shared_ptr<VMBootParameters>& AutoBoot(std::shared_ptr<VMBootParameters>& autoboot);
	static bool ParseCommandLineOptions(const QStringList& args, std::shared_ptr<VMBootParameters>& autoboot);
	static bool InitializeConfig();
	static void SaveSettings();
	static void HookSignals();
	static void RegisterTypes();
	static void InitializeClipboard();
	static bool RunSetupWizard();
	std::optional<bool> DownloadFile(QWidget* parent, const QString& title, std::string url, std::vector<u8>* data);
}

static QTimer* s_settings_save_timer = nullptr;
static std::unique_ptr<INISettingsInterface> s_base_settings_interface;
static std::unique_ptr<INISettingsInterface> s_secrets_settings_interface;
static bool s_batch_mode = false;
static bool s_nogui_mode = false;
static bool s_start_big_picture_mode = false;
static bool s_start_fullscreen = false;
static bool s_test_config_and_exit = false;
static bool s_run_setup_wizard = false;
static bool s_cleanup_after_update = false;
static bool s_boot_and_debug = false;
static std::atomic_int s_vm_locked_with_dialog = 0;
static std::string s_clipboard_cache;
static std::mutex s_clipboard_cache_mutex;

EmuThread::EmuThread(QThread* ui_thread)
	: QThread()
	, m_ui_thread(ui_thread)
{
}

EmuThread::~EmuThread() = default;

void EmuThread::start()
{
	pxAssertRel(!g_emu_thread, "Emu thread does not exist");

	g_emu_thread = new EmuThread(QThread::currentThread());
	g_emu_thread->setStackSize(VMManager::EMU_THREAD_STACK_SIZE);
	g_emu_thread->QThread::start();
	g_emu_thread->m_started_semaphore.acquire();
	g_emu_thread->moveToThread(g_emu_thread);
}

void EmuThread::stop()
{
	pxAssertRel(g_emu_thread, "Emu thread exists");
	pxAssertRel(!g_emu_thread->isOnEmuThread(), "Not called on the emu thread");

	QMetaObject::invokeMethod(g_emu_thread, &EmuThread::stopInThread, Qt::QueuedConnection);
	while (g_emu_thread->isRunning())
		QApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 1);
}

void EmuThread::stopInThread()
{
	if (VMManager::HasValidVM())
		destroyVM();

	if (m_run_fullscreen_ui)
		stopFullscreenUI();

	m_event_loop->quit();
	m_shutdown_flag.store(true);
}

void EmuThread::startFullscreenUI(bool fullscreen)
{
	if (!isOnEmuThread())
	{
		QMetaObject::invokeMethod(this, "startFullscreenUI", Qt::QueuedConnection, Q_ARG(bool, fullscreen));
		return;
	}

	if (VMManager::HasValidVM() || MTGS::IsOpen())
		return;

	ImGuiManager::InitializeFullscreenUI();
	m_run_fullscreen_ui.store(true, std::memory_order_release);
	m_is_rendering_to_main = shouldRenderToMain();
	m_is_fullscreen = fullscreen;

	if (!MTGS::WaitForOpen())
	{
		m_run_fullscreen_ui.store(false, std::memory_order_release);
		return;
	}

	emit onFullscreenUIStateChange(true);

	stopBackgroundControllerPollTimer();
	startBackgroundControllerPollTimer();
}

void EmuThread::stopFullscreenUI()
{
	if (!isOnEmuThread())
	{
		QMetaObject::invokeMethod(this, &EmuThread::stopFullscreenUI, Qt::QueuedConnection);

		while (m_run_fullscreen_ui.load(std::memory_order_acquire) || (!QtHost::IsVMValid() && MTGS::IsOpen()))
			QApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 1);

		return;
	}

	setFullscreen(false, true);

	if (MTGS::IsOpen() && !VMManager::HasValidVM())
		MTGS::WaitForClose();

	if (m_run_fullscreen_ui.load(std::memory_order_acquire))
	{
		m_run_fullscreen_ui.store(false, std::memory_order_release);
		emit onFullscreenUIStateChange(false);

		QMetaObject::invokeMethod(g_main_window, "updateGameListBackground", Qt::QueuedConnection);
	}
}

void EmuThread::startVM(std::shared_ptr<VMBootParameters> boot_params)
{
	if (!isOnEmuThread())
	{
		QMetaObject::invokeMethod(this, "startVM", Qt::QueuedConnection, Q_ARG(std::shared_ptr<VMBootParameters>, boot_params));
		return;
	}

	m_is_rendering_to_main = shouldRenderToMain();
	if (boot_params->fullscreen.has_value())
		m_is_fullscreen = boot_params->fullscreen.value();
	else
		m_is_fullscreen = Host::GetBaseBoolSettingValue("UI", "StartFullscreen", false);

	auto hardcore_disable_callback = [](std::string reason, VMBootRestartCallback restart_callback) {
		QtHost::RunOnUIThread([reason = std::move(reason), restart_callback = std::move(restart_callback)]() {
			QString title(Achievements::GetHardcoreModeDisableTitle());
			QString text(QString::fromStdString(Achievements::GetHardcoreModeDisableText(reason.c_str())));
			if (g_main_window->confirmMessage(title, text))
				Host::RunOnCPUThread(restart_callback);
		});
	};

	auto done_callback = [](VMBootResult result, const Error& error) {
		if (result != VMBootResult::StartupSuccess)
		{
			Host::ReportErrorAsync(TRANSLATE_STR("QtHost", "Startup Error"), error.GetDescription());
			return;
		}

		if (!Host::GetBoolSettingValue("UI", "StartPaused", false))
		{
			VMManager::SetState(VMState::Running);
		}
		else
		{
			g_emu_thread->redrawDisplayWindow();
			Host::OnVMPaused();
		}

		g_emu_thread->getEventLoop()->quit();
	};

	VMManager::InitializeAsync(*boot_params, std::move(hardcore_disable_callback), std::move(done_callback));
}

void EmuThread::resetVM()
{
	if (!isOnEmuThread())
	{
		QMetaObject::invokeMethod(this, &EmuThread::resetVM, Qt::QueuedConnection);
		return;
	}

	VMManager::Reset();
}

void EmuThread::setVMPaused(bool paused)
{
	if (!isOnEmuThread())
	{
		QMetaObject::invokeMethod(this, "setVMPaused", Qt::QueuedConnection, Q_ARG(bool, paused));
		return;
	}

	VMManager::SetPaused(paused);
}

void EmuThread::shutdownVM(bool save_state )
{
	if (!isOnEmuThread())
	{
		QMetaObject::invokeMethod(this, "shutdownVM", Qt::QueuedConnection, Q_ARG(bool, save_state));
		return;
	}

	const VMState state = VMManager::GetState();
	if (state == VMState::Paused)
		m_event_loop->quit();
	else if (state != VMState::Running)
		return;

	m_save_state_on_shutdown = save_state;
	VMManager::SetState(VMState::Stopping);
}

void EmuThread::loadState(const QString& filename)
{
	if (!isOnEmuThread())
	{
		QMetaObject::invokeMethod(this, "loadState", Qt::QueuedConnection, Q_ARG(const QString&, filename));
		return;
	}

	if (!VMManager::HasValidVM())
		return;

	Error error;
	if (!VMManager::LoadState(filename.toUtf8().constData(), &error))
	{
		QtHost::RunOnUIThread([message = QString::fromStdString(error.GetDescription())]() {
			g_main_window->reportStateLoadError(message, std::nullopt, false);
		});
	}
}

void EmuThread::loadStateFromSlot(qint32 slot, bool load_backup)
{
	if (!isOnEmuThread())
	{
		QMetaObject::invokeMethod(this, "loadStateFromSlot", Qt::QueuedConnection, Q_ARG(qint32, slot), Q_ARG(bool, load_backup));
		return;
	}

	if (!VMManager::HasValidVM())
		return;

	Error error;
	if (!VMManager::LoadStateFromSlot(slot, load_backup, &error))
	{
		QtHost::RunOnUIThread([message = QString::fromStdString(error.GetDescription()), slot, load_backup]() {
			g_main_window->reportStateLoadError(message, slot, load_backup);
		});
	}
}

void EmuThread::saveState(const QString& filename)
{
	if (!isOnEmuThread())
	{
		QMetaObject::invokeMethod(this, "saveState", Qt::QueuedConnection, Q_ARG(const QString&, filename));
		return;
	}

	if (!VMManager::HasValidVM())
		return;

	VMManager::SaveState(filename.toUtf8().constData(), true, false, [](const std::string& error) {
		QtHost::RunOnUIThread([message = QString::fromStdString(error)]() {
			g_main_window->reportStateSaveError(message, std::nullopt);
		});
	});
}

void EmuThread::saveStateToSlot(qint32 slot)
{
	if (!isOnEmuThread())
	{
		QMetaObject::invokeMethod(this, "saveStateToSlot", Qt::QueuedConnection, Q_ARG(qint32, slot));
		return;
	}

	if (!VMManager::HasValidVM())
		return;

	VMManager::SaveStateToSlot(slot, true, [slot](const std::string& error) {
		QtHost::RunOnUIThread([message = QString::fromStdString(error), slot]() {
			g_main_window->reportStateSaveError(message, slot);
		});
	});
}

void EmuThread::run()
{
	m_event_loop = new QEventLoop();
	m_started_semaphore.release();
	connectSignals();

	if (!VMManager::Internal::CPUThreadInitialize())
	{
		VMManager::Internal::CPUThreadShutdown();
		QMetaObject::invokeMethod(qApp, &QCoreApplication::quit, Qt::QueuedConnection);
		return;
	}

	createBackgroundControllerPollTimer();
	startBackgroundControllerPollTimer();

	while (!m_shutdown_flag.load())
	{
		switch (VMManager::GetState())
		{
			case VMState::Initializing:
				pxFailRel("Shouldn't be in the starting state");
				continue;

			case VMState::Shutdown:
			case VMState::Paused:
				m_event_loop->exec();
				continue;

			case VMState::Running:
				m_event_loop->processEvents(QEventLoop::AllEvents);
				VMManager::Execute();
				continue;

			case VMState::Resetting:
				VMManager::Reset();
				continue;

			case VMState::Stopping:
				destroyVM();
				continue;

			default:
				continue;
		}
	}

	stopBackgroundControllerPollTimer();
	destroyBackgroundControllerPollTimer();
	VMManager::Internal::CPUThreadShutdown();

	moveToThread(m_ui_thread);
	deleteLater();
}

void EmuThread::destroyVM()
{
	m_last_speed = 0.0f;
	m_last_gpu_usage = 0.0f;
	m_last_game_fps = 0.0f;
	m_last_video_fps = 0.0f;
	m_last_internal_width = 0;
	m_last_internal_height = 0;
	m_last_upscale = 0.0f;
	m_last_volume = 0;
	m_last_muted = false;
	m_last_renderer = GSRendererType::Auto;
	m_last_limiter_mode = LimiterModeType::Nominal;
	m_was_paused_by_focus_loss = false;
	VMManager::Shutdown(m_save_state_on_shutdown);
	m_save_state_on_shutdown = false;
}

void EmuThread::createBackgroundControllerPollTimer()
{
	pxAssert(!m_background_controller_polling_timer);
	m_background_controller_polling_timer = new QTimer(this);
	m_background_controller_polling_timer->setSingleShot(false);
	m_background_controller_polling_timer->setTimerType(Qt::CoarseTimer);
	connect(m_background_controller_polling_timer, &QTimer::timeout, this, &EmuThread::doBackgroundControllerPoll);
}

void EmuThread::destroyBackgroundControllerPollTimer()
{
	delete m_background_controller_polling_timer;
	m_background_controller_polling_timer = nullptr;
}

void EmuThread::startBackgroundControllerPollTimer()
{
	if (m_background_controller_polling_timer->isActive())
		return;

	m_background_controller_polling_timer->start(
		FullscreenUI::IsInitialized() ? FULLSCREEN_UI_CONTROLLER_POLLING_INTERVAL : BACKGROUND_CONTROLLER_POLLING_INTERVAL);
}

void EmuThread::stopBackgroundControllerPollTimer()
{
	if (!m_background_controller_polling_timer->isActive())
		return;

	m_background_controller_polling_timer->stop();
}

void EmuThread::doBackgroundControllerPoll()
{
	VMManager::IdlePollUpdate();
}

void EmuThread::toggleFullscreen()
{
	if (!isOnEmuThread())
	{
		QMetaObject::invokeMethod(this, &EmuThread::toggleFullscreen, Qt::QueuedConnection);
		return;
	}

	setFullscreen(!m_is_fullscreen, true);
}

void EmuThread::setFullscreen(bool fullscreen, bool allow_render_to_main)
{
	if (!isOnEmuThread())
	{
		QMetaObject::invokeMethod(this, "setFullscreen", Qt::QueuedConnection, Q_ARG(bool, fullscreen), Q_ARG(bool, allow_render_to_main));
		return;
	}

	if (s_vm_locked_with_dialog > 0)
		return;

	if (!MTGS::IsOpen() || m_is_fullscreen == fullscreen)
		return;

	m_is_fullscreen = fullscreen;
	m_is_rendering_to_main = allow_render_to_main && shouldRenderToMain();
	MTGS::UpdateDisplayWindow();
	MTGS::WaitGS();

	VMManager::UpdateTargetSpeed();
}

void EmuThread::setSurfaceless(bool surfaceless)
{
	if (!isOnEmuThread())
	{
		QMetaObject::invokeMethod(this, "setSurfaceless", Qt::QueuedConnection, Q_ARG(bool, surfaceless));
		return;
	}

	if (!MTGS::IsOpen() || m_is_surfaceless == surfaceless)
		return;

	m_is_surfaceless = surfaceless;
	MTGS::UpdateDisplayWindow();
	MTGS::WaitGS();
}

void EmuThread::applySettings()
{
	if (!isOnEmuThread())
	{
		QMetaObject::invokeMethod(this, &EmuThread::applySettings, Qt::QueuedConnection);
		return;
	}

	VMManager::ApplySettings();
}

void EmuThread::reloadGameSettings()
{
	if (!isOnEmuThread())
	{
		QMetaObject::invokeMethod(this, &EmuThread::reloadGameSettings, Qt::QueuedConnection);
		return;
	}

	VMManager::ReloadGameSettings();
}

void EmuThread::updateEmuFolders()
{
	if (!isOnEmuThread())
	{
		QMetaObject::invokeMethod(this, &EmuThread::updateEmuFolders, Qt::QueuedConnection);
		return;
	}

	VMManager::Internal::UpdateEmuFolders();
}

void EmuThread::connectSignals()
{
	connect(qApp, &QGuiApplication::applicationStateChanged, this, &EmuThread::onApplicationStateChanged);
}

void EmuThread::loadSettings(SettingsInterface& si, std::unique_lock<std::mutex>& lock)
{
	m_verbose_status = si.GetBoolValue("UI", "VerboseStatusBar", false);
	m_pause_on_focus_loss = si.GetBoolValue("UI", "PauseOnFocusLoss", false);
}

void Host::LoadSettings(SettingsInterface& si, std::unique_lock<std::mutex>& lock)
{
	g_emu_thread->loadSettings(si, lock);
}

void EmuThread::checkForSettingChanges(const Pcsx2Config& old_config)
{
	if (g_main_window)
	{
		QMetaObject::invokeMethod(g_main_window, &MainWindow::checkForSettingChanges, Qt::QueuedConnection);
		updatePerformanceMetrics(true);
	}

	if (MTGS::IsOpen())
	{
		const bool render_to_main = shouldRenderToMain();
		if (!m_is_fullscreen && m_is_rendering_to_main != render_to_main)
		{
			m_is_rendering_to_main = render_to_main;
			MTGS::UpdateDisplayWindow();
			MTGS::WaitGS();
		}
	}
}

void Host::CheckForSettingsChanges(const Pcsx2Config& old_config)
{
	g_emu_thread->checkForSettingChanges(old_config);
}

bool EmuThread::shouldRenderToMain() const
{
	return !Host::GetBoolSettingValue("UI", "RenderToSeparateWindow", false) && !Host::InNoGUIMode();
}

void EmuThread::toggleSoftwareRendering()
{
	if (!isOnEmuThread())
	{
		QMetaObject::invokeMethod(this, &EmuThread::toggleSoftwareRendering, Qt::QueuedConnection);
		return;
	}

	if (!VMManager::HasValidVM())
		return;

	MTGS::ToggleSoftwareRendering();
}

void EmuThread::changeDisc(CDVD_SourceType source, const QString& path)
{
	if (!isOnEmuThread())
	{
		QMetaObject::invokeMethod(this, "changeDisc", Qt::QueuedConnection, Q_ARG(CDVD_SourceType, source), Q_ARG(const QString&, path));
		return;
	}

	if (!VMManager::HasValidVM())
		return;

	VMManager::ChangeDisc(source, path.toStdString());
}

void EmuThread::setELFOverride(const QString& path)
{
	if (!isOnEmuThread())
	{
		QMetaObject::invokeMethod(this, "setELFOverride", Qt::QueuedConnection, Q_ARG(const QString&, path));
		return;
	}

	if (!VMManager::HasValidVM())
		return;

	VMManager::SetELFOverride(path.toStdString());
}

void EmuThread::changeGSDump(const QString& path)
{
	if (!isOnEmuThread())
	{
		QMetaObject::invokeMethod(this, "changeGSDump", Qt::QueuedConnection, Q_ARG(const QString&, path));
		return;
	}

	if (!VMManager::HasValidVM())
		return;

	VMManager::ChangeGSDump(path.toStdString());
}

void EmuThread::reloadPatches()
{
	if (!isOnEmuThread())
	{
		QMetaObject::invokeMethod(this, &EmuThread::reloadPatches, Qt::QueuedConnection);
		return;
	}

	VMManager::ReloadPatches(true, false, true, true);
}

void EmuThread::reloadInputSources()
{
	if (!isOnEmuThread())
	{
		QMetaObject::invokeMethod(this, &EmuThread::reloadInputSources, Qt::QueuedConnection);
		return;
	}

	VMManager::ReloadInputSources();
}

void EmuThread::reloadInputBindings()
{
	if (!isOnEmuThread())
	{
		QMetaObject::invokeMethod(this, &EmuThread::reloadInputBindings, Qt::QueuedConnection);
		return;
	}

	VMManager::ReloadInputBindings();
}

void EmuThread::reloadInputDevices()
{
	if (!isOnEmuThread())
	{
		QMetaObject::invokeMethod(this, &EmuThread::reloadInputDevices, Qt::QueuedConnection);
		return;
	}

	InputManager::ReloadDevices();
}

void EmuThread::closeInputSources()
{
	if (!isOnEmuThread())
	{
		QMetaObject::invokeMethod(this, &EmuThread::reloadInputDevices, Qt::BlockingQueuedConnection);
		return;
	}

	InputManager::CloseSources();
}

void EmuThread::requestDisplaySize(float scale)
{
	if (!isOnEmuThread())
	{
		QMetaObject::invokeMethod(this, "requestDisplaySize", Qt::QueuedConnection, Q_ARG(float, scale));
		return;
	}

	if (!VMManager::HasValidVM())
		return;

	VMManager::RequestDisplaySize(scale);
}

void EmuThread::enumerateInputDevices()
{
	if (!isOnEmuThread())
	{
		QMetaObject::invokeMethod(this, &EmuThread::enumerateInputDevices, Qt::QueuedConnection);
		return;
	}

	const std::vector<std::pair<std::string, std::string>> devs(InputManager::EnumerateDevices());
	QList<QPair<QString, QString>> qdevs;
	qdevs.reserve(devs.size());
	for (const std::pair<std::string, std::string>& dev : devs)
		qdevs.emplace_back(QString::fromStdString(dev.first), QString::fromStdString(dev.second));

	onInputDevicesEnumerated(qdevs);
}

void EmuThread::enumerateVibrationMotors()
{
	if (!isOnEmuThread())
	{
		QMetaObject::invokeMethod(this, &EmuThread::enumerateVibrationMotors, Qt::QueuedConnection);
		return;
	}

	const std::vector<InputBindingKey> motors(InputManager::EnumerateMotors());
	QList<InputBindingKey> qmotors;
	qmotors.reserve(motors.size());
	for (InputBindingKey key : motors)
		qmotors.push_back(key);

	onVibrationMotorsEnumerated(qmotors);
}

void EmuThread::connectDisplaySignals(DisplaySurface* widget)
{
	widget->disconnect(this);

	connect(widget, &DisplaySurface::windowResizedEvent, this, &EmuThread::onDisplayWindowResized);
	connect(widget, &DisplaySurface::windowRestoredEvent, this, &EmuThread::redrawDisplayWindow);
}

void EmuThread::onDisplayWindowResized(u32 width, u32 height, float scale)
{
	if (!MTGS::IsOpen())
		return;

	MTGS::ResizeDisplayWindow(width, height, scale);
}

void EmuThread::onApplicationStateChanged(Qt::ApplicationState state)
{
	if (!VMManager::HasValidVM())
		return;

	const bool focus_loss = (state != Qt::ApplicationActive);
	if (focus_loss)
	{
		if (m_pause_on_focus_loss && !m_was_paused_by_focus_loss && VMManager::GetState() == VMState::Running)
		{
			m_was_paused_by_focus_loss = true;
			VMManager::SetPaused(true);
		}

		InputManager::ClearBindStateFromSource(InputManager::MakeHostKeyboardKey(0));
	}
	else
	{
		if (m_was_paused_by_focus_loss)
		{
			m_was_paused_by_focus_loss = false;
			if (VMManager::GetState() == VMState::Paused)
				VMManager::SetPaused(false);
		}
	}
}

void EmuThread::redrawDisplayWindow()
{
	if (!isOnEmuThread())
	{
		QMetaObject::invokeMethod(this, &EmuThread::redrawDisplayWindow, Qt::QueuedConnection);
		return;
	}

	if (!VMManager::HasValidVM() || VMManager::GetState() == VMState::Running)
		return;

	MTGS::PresentCurrentFrame();
}

void EmuThread::runOnCPUThread(const std::function<void()>& func)
{
	func();
}

void EmuThread::queueSnapshot(quint32 gsdump_frames)
{
	if (!isOnEmuThread())
	{
		QMetaObject::invokeMethod(this, "queueSnapshot", Qt::QueuedConnection, Q_ARG(quint32, gsdump_frames));
		return;
	}

	if (!VMManager::HasValidVM())
		return;

	MTGS::RunOnGSThread([gsdump_frames]() { GSQueueSnapshot(std::string(), gsdump_frames); });
}

void EmuThread::beginCapture(const QString& path)
{
	if (!isOnEmuThread())
	{
		QMetaObject::invokeMethod(this, "beginCapture", Qt::QueuedConnection, Q_ARG(const QString&, path));
		return;
	}

	if (!VMManager::HasValidVM())
		return;

	MTGS::RunOnGSThread([path = path.toStdString()]() {
		GSBeginCapture(std::move(path));
	});

	MTGS::WaitGS(false, false, false);
}

void EmuThread::endCapture()
{
	if (!isOnEmuThread())
	{
		QMetaObject::invokeMethod(this, "endCapture", Qt::QueuedConnection);
		return;
	}

	if (!VMManager::HasValidVM())
		return;

	MTGS::RunOnGSThread(&GSEndCapture);
}

std::optional<WindowInfo> EmuThread::acquireRenderWindow(bool recreate_window)
{
	m_is_exclusive_fullscreen = m_is_fullscreen && GSWantsExclusiveFullscreen();
	const bool window_fullscreen = m_is_fullscreen && !m_is_exclusive_fullscreen;
	const bool render_to_main = !m_is_exclusive_fullscreen && !window_fullscreen && m_is_rendering_to_main;

	return emit onAcquireRenderWindowRequested(recreate_window, window_fullscreen, render_to_main, m_is_surfaceless);
}

void EmuThread::releaseRenderWindow()
{
	emit onReleaseRenderWindowRequested();
}

std::optional<WindowInfo> Host::AcquireRenderWindow(bool recreate_window)
{
	return g_emu_thread->acquireRenderWindow(recreate_window);
}

void Host::ReleaseRenderWindow()
{
	return g_emu_thread->releaseRenderWindow();
}

void Host::BeginPresentFrame()
{
}

void Host::RequestResizeHostDisplay(s32 width, s32 height)
{
	g_emu_thread->onResizeRenderWindowRequested(width, height);
}

void Host::OnVMStarting()
{
	g_emu_thread->stopBackgroundControllerPollTimer();
	emit g_emu_thread->onVMStarting();
}

void Host::OnVMStarted()
{
	g_emu_thread->updatePerformanceMetrics(true);
	emit g_emu_thread->onVMStarted();
}

void Host::OnVMDestroyed()
{
	emit g_emu_thread->onVMStopped();
	g_emu_thread->startBackgroundControllerPollTimer();
}

void Host::OnVMPaused()
{
	g_emu_thread->startBackgroundControllerPollTimer();
	emit g_emu_thread->onVMPaused();
}

void Host::OnVMResumed()
{
	g_emu_thread->getEventLoop()->quit();
	g_emu_thread->stopBackgroundControllerPollTimer();

	if (g_emu_thread->isSurfaceless())
		g_emu_thread->setSurfaceless(false);

	emit g_emu_thread->onVMResumed();
}

void Host::OnGameChanged(const std::string& title, const std::string& elf_override, const std::string& disc_path,
	const std::string& disc_serial, u32 disc_crc, u32 current_crc)
{
	emit g_emu_thread->onGameChanged(QString::fromStdString(title), QString::fromStdString(elf_override),
		QString::fromStdString(disc_path), QString::fromStdString(disc_serial), disc_crc, current_crc);
}

void EmuThread::updatePerformanceMetrics(bool force)
{
	if (!g_main_window)
		return;

	const s32 slot = SaveStateSelectorUI::GetCurrentSlot();
	u32 volume = 0;
	bool muted = false;

	if (VMManager::HasValidVM())
	{
		volume = SPU2::GetOutputVolume();
		muted = SPU2::IsOutputMuted();

		QString gs_stat;
		if (m_verbose_status)
		{
			std::string gs_stat_str;
			GSgetTitleStats(gs_stat_str);

			if (THREAD_VU1)
			{
				gs_stat = tr("Slot: %1 | %2 | EE: %3% | VU: %4% | GS: %5%")
				              .arg(slot)
				              .arg(gs_stat_str.c_str())
				              .arg(PerformanceMetrics::GetCPUThreadUsage(), 0, 'f', 0)
				              .arg(PerformanceMetrics::GetVUThreadUsage(), 0, 'f', 0)
				              .arg(PerformanceMetrics::GetGSThreadUsage(), 0, 'f', 0);
			}
			else
			{
				gs_stat = tr("Slot: %1 | %2 | EE: %3% | GS: %4%")
				              .arg(slot)
				              .arg(gs_stat_str.c_str())
				              .arg(PerformanceMetrics::GetCPUThreadUsage(), 0, 'f', 0)
				              .arg(PerformanceMetrics::GetGSThreadUsage(), 0, 'f', 0);
			}
		}

		QMetaObject::invokeMethod(g_main_window, "setStatusVerboseText", Qt::QueuedConnection, Q_ARG(const QString&, gs_stat));
	}

	const GSRendererType renderer = GSGetCurrentRenderer();
	const float upscale = EmuConfig.GS.UpscaleMultiplier;
	const float speed = std::round(PerformanceMetrics::GetSpeed());
	const LimiterModeType limiter_mode = VMManager::GetLimiterMode();
	const float gpu_usage = std::round(PerformanceMetrics::GetGPUUsage());
	const float gfps = std::round(PerformanceMetrics::GetInternalFPS());
	const float vfps = std::round(PerformanceMetrics::GetFPS());
	int iwidth, iheight;
	GSgetInternalResolution(&iwidth, &iheight);

	if (iwidth != m_last_internal_width || iheight != m_last_internal_height || upscale != m_last_upscale ||
		speed != m_last_speed || gpu_usage != m_last_gpu_usage || gfps != m_last_game_fps || vfps != m_last_video_fps || renderer != m_last_renderer ||
		volume != m_last_volume || muted != m_last_muted || force)
	{

		if (volume != m_last_volume || muted != m_last_muted || force)
		{
			QString vol_text = tr("Volume: %1%").arg(volume);
			if (muted)
				vol_text = tr("Volume: Muted");
			QMetaObject::invokeMethod(g_main_window, "setStatusVolumeText", Qt::QueuedConnection,
				Q_ARG(const QString&, vol_text), Q_ARG(int, static_cast<int>(volume)), Q_ARG(bool, muted));
			m_last_volume = volume;
			m_last_muted = muted;
		}

		if (renderer != m_last_renderer || force)
		{
			QString renderer_name = QString::fromUtf8(Pcsx2Config::GSOptions::GetRendererName(renderer));
			if (EmuConfig.GS.Renderer == GSRendererType::Auto && renderer != GSRendererType::Auto)
				renderer_name = tr("Auto (%1)").arg(renderer_name);

			QMetaObject::invokeMethod(g_main_window, "setStatusRendererText", Qt::QueuedConnection,
				Q_ARG(const QString&, renderer_name));
			m_last_renderer = renderer;
		}

		if (iwidth != m_last_internal_width || iheight != m_last_internal_height || upscale != m_last_upscale || force)
		{
			QString text;
			if (iwidth == 0 || iheight == 0)
				text = tr("No Image");
			else
				text = tr("%1x%2 (%3x)").arg(iwidth).arg(iheight).arg(upscale, 0, 'g', 3);

			QMetaObject::invokeMethod(
				g_main_window, "setStatusResolutionText", Qt::QueuedConnection, Q_ARG(const QString&, text));

			m_last_internal_width = iwidth;
			m_last_internal_height = iheight;
			m_last_upscale = upscale;
		}

		if (gpu_usage != m_last_gpu_usage || force)
		{
			QString text;
			if (gpu_usage == 0)
				text = tr("GPU: N/A");
			else
				text = tr("GPU: %1%").arg(gpu_usage, 0, 'f', 0);

			QMetaObject::invokeMethod(g_main_window, "setStatusGPUText", Qt::QueuedConnection,
				Q_ARG(const QString&, text));
			m_last_gpu_usage = gpu_usage;
		}

		if (gfps != m_last_game_fps || force)
		{
			QString text;
			if (gfps == 0)
				text = tr("FPS: N/A");
			else
				text = tr("FPS: %1").arg(gfps, 0, 'f', 0);

			QMetaObject::invokeMethod(g_main_window, "setStatusFPSText", Qt::QueuedConnection,
				Q_ARG(const QString&, text));
			m_last_game_fps = gfps;
		}

		if (vfps != m_last_video_fps || force)
		{
			QMetaObject::invokeMethod(g_main_window, "setStatusVPSText", Qt::QueuedConnection,
				Q_ARG(const QString&, vfps == 0 ? tr("VPS: N/A") : tr("VPS: %1").arg(vfps, 0, 'f', 0)));
			m_last_video_fps = vfps;
		}

		if (speed != m_last_speed || force)
		{
			QMetaObject::invokeMethod(g_main_window, "setStatusSpeedText", Qt::QueuedConnection,
				Q_ARG(const QString&, tr("Speed: %1%").arg(speed, 0, 'f', 0)));
			m_last_speed = speed;
		}

		if (limiter_mode != m_last_limiter_mode || force)
		{
			QMetaObject::invokeMethod(g_main_window, [limiter_mode]() { g_main_window->getStatusSpeedWidget()->setIcon(QIcon::fromTheme(
																			limiter_mode == LimiterModeType::Turbo     ? QStringLiteral("fast-forward-line") :
																			limiter_mode == LimiterModeType::Slomo     ? QStringLiteral("slow-mo") :
																			limiter_mode == LimiterModeType::Unlimited ? QStringLiteral("speed-line") :
																														 QStringLiteral("dashboard-line"))); }, Qt::QueuedConnection);
			m_last_limiter_mode = limiter_mode;
		}
	}
}

void Host::OnPerformanceMetricsUpdated()
{
	g_emu_thread->updatePerformanceMetrics(false);
}

void Host::OnSaveStateLoading(const std::string_view filename)
{
	emit g_emu_thread->onSaveStateLoading(QtUtils::StringViewToQString(filename));
}

void Host::OnSaveStateLoaded(const std::string_view filename, bool was_successful)
{
	emit g_emu_thread->onSaveStateLoaded(QtUtils::StringViewToQString(filename), was_successful);
}

void Host::OnSaveStateSaved(const std::string_view filename)
{
	emit g_emu_thread->onSaveStateSaved(QtUtils::StringViewToQString(filename));
}

void Host::OnAchievementsLoginRequested(Achievements::LoginRequestReason reason)
{
	emit g_emu_thread->onAchievementsLoginRequested(reason);
}

void Host::OnAchievementsLoginSuccess(const char* username, u32 points, u32 sc_points, u32 unread_messages)
{
	const QString message =
		qApp->translate("QtHost", "RA: Logged in as %1 (%2 pts, Casual: %3 pts). %4 unread messages.")
			.arg(QString::fromUtf8(username))
			.arg(points)
			.arg(sc_points)
			.arg(unread_messages);

	emit g_emu_thread->statusMessage(message);
}

void Host::OnAchievementsRefreshed()
{
	u32 game_id = 0;

	QString game_info;

	if (Achievements::HasActiveGame())
	{
		game_id = Achievements::GetGameID();

		game_info = qApp
		                ->translate("EmuThread", "Game: %1 (%2)\n")
		                .arg(QString::fromStdString(Achievements::GetGameTitle()))
		                .arg(game_id);

		const std::string& rich_presence_string = Achievements::GetRichPresenceString();
		if (!rich_presence_string.empty())
			game_info.append(QString::fromStdString(StringUtil::Ellipsise(rich_presence_string, 128)));
		else
			game_info.append(qApp->translate("EmuThread", "Rich presence inactive or unsupported."));
	}
	else
	{
		game_info = qApp->translate("EmuThread", "Game not loaded or no RetroAchievements available.");
	}

	emit g_emu_thread->onAchievementsRefreshed(game_id, game_info);
}

void Host::OnAchievementsHardcoreModeChanged(bool enabled)
{
	emit g_emu_thread->onAchievementsHardcoreModeChanged(enabled);
}

bool Host::ShouldPreferHostFileSelector()
{
#ifdef __linux__
	return (std::getenv("container") != nullptr);
#else
	return false;
#endif
}

void Host::OpenHostFileSelectorAsync(std::string_view title, bool select_directory, FileSelectorCallback callback,
	FileSelectorFilters filters, std::string_view initial_directory)
{
	const bool from_cpu_thread = g_emu_thread->isOnEmuThread();

	QString filters_str;
	if (!filters.empty())
	{
		filters_str.append(QStringLiteral("All File Types (%1)")
				.arg(QString::fromStdString(StringUtil::JoinString(filters.begin(), filters.end(), " "))));
		for (const std::string& filter : filters)
		{
			filters_str.append(
				QStringLiteral(";;%1 Files (%2)")
					.arg(
						QtUtils::StringViewToQString(std::string_view(filter).substr(filter.starts_with("*.") ? 2 : 0)).toUpper())
					.arg(QString::fromStdString(filter)));
		}
	}

	QtHost::RunOnUIThread([title = QtUtils::StringViewToQString(title), select_directory, callback = std::move(callback),
							  filters_str = std::move(filters_str),
							  initial_directory = QtUtils::StringViewToQString(initial_directory),
							  from_cpu_thread]() mutable {
		auto lock = g_main_window->pauseAndLockVM();

		QString path;

		if (select_directory)
			path = QFileDialog::getExistingDirectory(lock.getDialogParent(), title, initial_directory);
		else
			path = QFileDialog::getOpenFileName(lock.getDialogParent(), title, initial_directory, filters_str);

		if (!path.isEmpty())
			path = QDir::toNativeSeparators(path);

		if (from_cpu_thread)
			Host::RunOnCPUThread([callback = std::move(callback), path = path.toStdString()]() { callback(path); });
		else
			callback(path.toStdString());
	});
}

void Host::PumpMessagesOnCPUThread()
{
	g_emu_thread->getEventLoop()->processEvents(QEventLoop::AllEvents);
}

void Host::RunOnCPUThread(std::function<void()> function, bool block )
{
	if (block && g_emu_thread->isOnEmuThread())
	{
		function();
		return;
	}

	QMetaObject::invokeMethod(g_emu_thread, "runOnCPUThread", block ? Qt::BlockingQueuedConnection : Qt::QueuedConnection,
		Q_ARG(const std::function<void()>&, std::move(function)));
}

void Host::RunOnGSThread(std::function<void()> function)
{
	RunOnCPUThread([fn = std::move(function)] { MTGS::RunOnGSThread(std::move(fn)); });
}

void Host::RefreshGameListAsync(bool invalidate_cache)
{
	QMetaObject::invokeMethod(g_main_window, "refreshGameList", Qt::QueuedConnection, Q_ARG(bool, invalidate_cache));
}

void Host::CancelGameListRefresh()
{
	QMetaObject::invokeMethod(g_main_window, "cancelGameListRefresh", Qt::BlockingQueuedConnection);
}

void Host::RequestExitApplication(bool allow_confirm)
{
	QMetaObject::invokeMethod(g_main_window, "requestExit", Qt::QueuedConnection, Q_ARG(bool, allow_confirm));
}

void Host::RequestExitBigPicture()
{
	g_emu_thread->stopFullscreenUI();
}

void Host::RequestVMShutdown(bool allow_confirm, bool allow_save_state, bool default_save_state)
{
	if (!VMManager::HasValidVM())
		return;

	if (allow_confirm || !g_emu_thread->isOnEmuThread())
	{
		QMetaObject::invokeMethod(g_main_window, "requestShutdown", Qt::QueuedConnection, Q_ARG(bool, allow_confirm),
			Q_ARG(bool, allow_save_state), Q_ARG(bool, default_save_state));
	}
	else
	{
		g_emu_thread->shutdownVM(allow_save_state && default_save_state);

		if (Host::InBatchMode())
			QMetaObject::invokeMethod(g_main_window, "requestExit", Qt::QueuedConnection, Q_ARG(bool, false));
	}
}

bool Host::IsFullscreen()
{
	return g_emu_thread->isFullscreen();
}

void Host::SetFullscreen(bool enabled)
{
	g_emu_thread->setFullscreen(enabled, true);
}

void Host::OnCaptureStarted(const std::string& filename)
{
	emit g_emu_thread->onCaptureStarted(QString::fromStdString(filename));
}

void Host::OnCaptureStopped()
{
	emit g_emu_thread->onCaptureStopped();
}

static bool DropFolderHasBios(const std::string& dir)
{
	FileSystem::FindResultsArray files;
	if (!FileSystem::FindFiles(dir.c_str(), "*", FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_HIDDEN_FILES, &files))
		return false;

	u32 version, region;
	std::string description, zone;
	for (const FILESYSTEM_FIND_DATA& fd : files)
	{
		if (IsBIOS(fd.FileName.c_str(), version, description, region, zone))
			return true;
	}
	return false;
}

bool QtHost::InitializeConfig()
{
	Error error;

	EmuFolders::SetAppRoot();

	if (!EmuFolders::SetResourcesDirectory())
	{
		QMessageBox::critical(nullptr, QStringLiteral("PenguinScreen2"),
			QStringLiteral("Resources directory is missing, your installation is incomplete."));
		return false;
	}

	if (!EmuFolders::SetDataDirectory(&error))
	{
		QMessageBox::critical(
			nullptr, QStringLiteral("PenguinScreen2"),
			QStringLiteral("Failed to create data directory at path\n\n%1\n\n"
						   "The error was: %2\n"
						   "Please ensure this directory is writable. You can also try portable mode "
						   "by creating portable.txt in the same directory you installed PenguinScreen2 into.")
				.arg(QString::fromStdString(EmuFolders::DataRoot))
				.arg(QString::fromStdString(error.GetDescription())));
		return false;
	}

	CrashHandler::SetWriteDirectory(EmuFolders::DataRoot);

	const std::string path = Path::Combine(EmuFolders::Settings, "PenguinScreen2.ini");
	const bool settings_exists = FileSystem::FileExists(path.c_str());
	Console.WriteLnFmt("Loading config from {}.", path);

	s_base_settings_interface = std::make_unique<INISettingsInterface>(std::move(path));
	Host::Internal::SetBaseSettingsLayer(s_base_settings_interface.get());
	if (!settings_exists || !s_base_settings_interface->Load() || !VMManager::Internal::CheckSettingsVersion())
	{
		if (FileSystem::FileExists(s_base_settings_interface->GetFileName().c_str()) &&
			QMessageBox::question(nullptr, QStringLiteral("PenguinScreen2"),
				QStringLiteral("Settings failed to load, or are the incorrect version. Clicking Yes will reset all settings to defaults. "
							   "Do you want to continue?")) != QMessageBox::Yes)
		{
			return false;
		}

		VMManager::SetDefaultSettings(*s_base_settings_interface, true, true, true, true, true);

		s_base_settings_interface->SetBoolValue("UI", "SetupWizardIncomplete", true);

		const std::string drop_bios =
			Path::Combine(QDir::homePath().toStdString(), "PS2-BIOS");
		const std::string drop_games =
			Path::Combine(QDir::homePath().toStdString(), "PS2-Games");
		if (DropFolderHasBios(drop_bios))
		{
			s_base_settings_interface->SetStringValue("Folders", "Bios", drop_bios.c_str());
			if (FileSystem::DirectoryExists(drop_games.c_str()))
				s_base_settings_interface->AddToStringList("GameList", "RecursivePaths", drop_games.c_str());
			s_base_settings_interface->SetBoolValue("UI", "SetupWizardIncomplete", false);
			Console.WriteLn("First run: valid BIOS found in '%s' — adopted the drop folders and skipping the setup wizard.",
				drop_bios.c_str());
		}

		if (!s_base_settings_interface->Save(&error))
		{
			QMessageBox::critical(
				nullptr, QStringLiteral("PenguinScreen2"),
				QStringLiteral(
					"Failed to save configuration to\n\n%1\n\nThe error was: %2\n\nPlease ensure this directory is writable. You "
					"can also try portable mode by creating portable.txt in the same directory you installed PenguinScreen2 into.")
					.arg(QString::fromStdString(s_base_settings_interface->GetFileName()))
					.arg(QString::fromStdString(error.GetDescription())));
			return false;
		}

		if (!s_run_setup_wizard)
			SaveSettings();
	}

	{
		const std::string drop_bios =
			Path::Combine(QDir::homePath().toStdString(), "PS2-BIOS");
		std::string cur_bios = s_base_settings_interface->GetStringValue("Folders", "Bios", "bios");
		if (!Path::IsAbsolute(cur_bios))
			cur_bios = Path::Combine(EmuFolders::DataRoot, cur_bios);
		if (cur_bios != drop_bios && !DropFolderHasBios(cur_bios) && DropFolderHasBios(drop_bios))
		{
			Console.WriteLn("BIOS folder '%s' has no valid BIOS but '%s' does — adopting the drop folder.",
				cur_bios.c_str(), drop_bios.c_str());
			s_base_settings_interface->SetStringValue("Folders", "Bios", drop_bios.c_str());
			s_base_settings_interface->Save();
		}
	}

	const std::string secrets_path = Path::Combine(EmuFolders::Settings, "secrets.ini");
	const bool secrets_settings_exists = FileSystem::FileExists(secrets_path.c_str());
	Console.WriteLnFmt("Loading secrets from {}.", secrets_path);

	s_secrets_settings_interface = std::make_unique<INISettingsInterface>(std::move(secrets_path));
	Host::Internal::SetSecretsSettingsLayer(s_secrets_settings_interface.get());
	if (!secrets_settings_exists || !s_secrets_settings_interface->Load())
	{
		if (!s_base_settings_interface->Save(&error))
		{
			QMessageBox::critical(
				nullptr, QStringLiteral("PenguinScreen2"),
				QStringLiteral(
					"Failed to save secrets to\n\n%1\n\nThe error was: %2\n\nPlease ensure this directory is writable. You "
					"can also try portable mode by creating portable.txt in the same directory you installed PenguinScreen2 into.")
					.arg(QString::fromStdString(s_secrets_settings_interface->GetFileName()))
					.arg(QString::fromStdString(error.GetDescription())));
			return false;
		}
	}

	s_run_setup_wizard =
		s_run_setup_wizard || s_base_settings_interface->GetBoolValue("UI", "SetupWizardIncomplete", false);

	VMManager::Internal::LoadStartupSettings();
	InstallTranslator(nullptr);
	return true;
}

void Host::SetDefaultUISettings(SettingsInterface& si)
{
	si.SetBoolValue("UI", "InhibitScreensaver", true);
	si.SetBoolValue("UI", "ConfirmShutdown", true);
	si.SetBoolValue("UI", "StartPaused", false);
	si.SetBoolValue("UI", "PauseOnFocusLoss", false);
	si.SetBoolValue("UI", "StartFullscreen", false);
	si.SetBoolValue("UI", "DoubleClickTogglesFullscreen", true);
	si.SetBoolValue("UI", "HideMouseCursor", false);
	si.SetBoolValue("UI", "RenderToSeparateWindow", false);
	si.SetBoolValue("UI", "HideMainWindowWhenRunning", false);
	si.SetBoolValue("UI", "DisableWindowResize", false);
	si.SetBoolValue("UI", "PreferEnglishGameList", false);
	si.SetStringValue("UI", "Theme", QtHost::GetDefaultThemeName());
}

void QtHost::SaveSettings()
{
	pxAssertRel(!g_emu_thread->isOnEmuThread(), "Saving should happen on the UI thread.");

	{
		Error error;
		auto lock = Host::GetSettingsLock();
		if (!s_base_settings_interface->Save(&error))
			Console.ErrorFmt("Failed to save settings: {}", error.GetDescription());
	}

	if (s_settings_save_timer)
	{
		s_settings_save_timer->deleteLater();
		s_settings_save_timer = nullptr;
	}
}

void Host::CommitBaseSettingChanges()
{
	if (!QtHost::IsOnUIThread())
	{
		QtHost::RunOnUIThread(&Host::CommitBaseSettingChanges);
		return;
	}

	auto lock = Host::GetSettingsLock();
	if (s_settings_save_timer)
		return;

	s_settings_save_timer = new QTimer;
	s_settings_save_timer->connect(s_settings_save_timer, &QTimer::timeout, &QtHost::SaveSettings);
	s_settings_save_timer->setSingleShot(true);
	s_settings_save_timer->start(SETTINGS_SAVE_DELAY);

	static bool connected = false;
	if (!connected)
	{
		QObject::connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, []() {
			delete s_settings_save_timer;
			s_settings_save_timer = nullptr;
		});

		connected = true;
	}
}

bool Host::InBatchMode()
{
	return s_batch_mode;
}

bool Host::InNoGUIMode()
{
	return s_nogui_mode;
}

bool QtHost::IsOnUIThread()
{
	QThread* ui_thread = qApp->thread();
	return (QThread::currentThread() == ui_thread);
}

bool QtHost::ShouldShowAdvancedSettings()
{
	return Host::GetBaseBoolSettingValue("UI", "ShowAdvancedSettings", false);
}

void QtHost::RunOnUIThread(const std::function<void()>& func, bool block )
{
	QMetaObject::invokeMethod(g_main_window, "runOnUIThread", block ? Qt::BlockingQueuedConnection : Qt::QueuedConnection,
		Q_ARG(const std::function<void()>&, func));
}

bool Host::RequestResetSettings(bool folders, bool core, bool controllers, bool hotkeys, bool ui)
{
	{
		auto lock = Host::GetSettingsLock();
		VMManager::SetDefaultSettings(*s_base_settings_interface.get(), folders, core, controllers, hotkeys, ui);
	}
	Host::CommitBaseSettingChanges();

	g_emu_thread->applySettings();
	if (folders)
		g_emu_thread->updateEmuFolders();

	return true;
}

QString QtHost::GetAppNameAndVersion()
{
	return QString("PenguinScreen2 %1").arg(BuildVersion::GitRev);
}

QString QtHost::GetAppConfigSuffix()
{
#if defined(PCSX2_DEBUG)
	return QStringLiteral(" [Debug]");
#elif defined(PCSX2_DEVBUILD)
	return QStringLiteral(" [Devel]");
#else
	return QString();
#endif
}

QIcon QtHost::GetAppIcon()
{
	return QIcon(QStringLiteral(":/icons/AppIcon64.png"));
}

QString QtHost::GetResourcesBasePath()
{
	return QString::fromStdString(EmuFolders::Resources);
}

std::string QtHost::GetRuntimeDownloadedResourceURL(std::string_view name)
{
	return fmt::format("{}/{}", RUNTIME_RESOURCES_URL, Path::URLEncode(name));
}

bool QtHost::SaveGameSettings(SettingsInterface* sif, bool delete_if_empty)
{
	INISettingsInterface* ini = static_cast<INISettingsInterface*>(sif);
	Error error;

	if (delete_if_empty && ini->IsEmpty())
	{
		INFO_LOG("Removing empty gamesettings ini {}", Path::GetFileName(ini->GetFileName()));
		if (FileSystem::FileExists(ini->GetFileName().c_str()) &&
			!FileSystem::DeleteFilePath(ini->GetFileName().c_str(), &error))
		{
			Host::ReportErrorAsync(
				TRANSLATE_SV("QtHost", "Error"),
				fmt::format(TRANSLATE_FS("QtHost", "An error occurred while deleting empty game settings:\n{}"),
					error.GetDescription()));
			return false;
		}

		return true;
	}

	sif->RemoveEmptySections();

	if (!sif->Save(&error))
	{
		Host::ReportErrorAsync(
			TRANSLATE_SV("QtHost", "Error"),
			fmt::format(TRANSLATE_FS("QtHost", "An error occurred while saving game settings:\n{}"), error.GetDescription()));
		return false;
	}

	return true;
}

std::optional<bool> QtHost::DownloadFile(QWidget* parent, const QString& title, std::string url, std::vector<u8>* data)
{
	static constexpr u32 HTTP_POLL_INTERVAL = 10;

	std::unique_ptr<HTTPDownloader> http = HTTPDownloader::Create(Host::GetHTTPUserAgent());
	if (!http)
	{
		QMessageBox::critical(parent, qApp->translate("EmuThread", "Error"), qApp->translate("EmuThread", "Failed to create HTTPDownloader."));
		return false;
	}

	std::optional<bool> download_result;
	const std::string::size_type url_file_part_pos = url.rfind('/');
	QtModalProgressCallback progress(parent);
	progress.GetDialog().setLabelText(
		qApp->translate("EmuThread", "Downloading %1...").arg(QtUtils::StringViewToQString(std::string_view(url).substr((url_file_part_pos != std::string::npos) ? (url_file_part_pos + 1) : 0))));
	progress.GetDialog().setWindowTitle(title);
	progress.GetDialog().setWindowIcon(GetAppIcon());
	progress.SetCancellable(true);

	http->CreateRequest(
		std::move(url), [parent, data, &download_result](s32 status_code, const std::string&, std::vector<u8> hdata) {
			if (status_code == HTTPDownloader::HTTP_STATUS_CANCELLED)
				return;

			if (status_code != HTTPDownloader::HTTP_STATUS_OK)
			{
				QMessageBox::critical(parent, qApp->translate("EmuThread", "Error"),
					qApp->translate("EmuThread", "Download failed with HTTP status code %1.").arg(status_code));
				download_result = false;
				return;
			}

			if (hdata.empty())
			{
				QMessageBox::critical(parent, qApp->translate("EmuThread", "Error"),
					qApp->translate("EmuThread", "Download failed: Data is empty.").arg(status_code));

				download_result = false;
				return;
			}

			*data = std::move(hdata);
			download_result = true;
		},
		&progress);

	while (http->HasAnyRequests())
	{
		QApplication::processEvents(QEventLoop::AllEvents, HTTP_POLL_INTERVAL);
		http->PollRequests();
	}

	return download_result;
}

bool QtHost::DownloadFile(QWidget* parent, const QString& title, std::string url, const std::string& path)
{
	std::vector<u8> data;
	if (!DownloadFile(parent, title, std::move(url), &data).value_or(false) || data.empty())
		return false;

	const std::string directory(Path::GetDirectory(path));
	if ((!directory.empty() && !FileSystem::DirectoryExists(directory.c_str()) &&
			!FileSystem::CreateDirectoryPath(directory.c_str(), true)) ||
		!FileSystem::WriteBinaryFile(path.c_str(), data.data(), data.size()))
	{
		QMessageBox::critical(parent, qApp->translate("EmuThread", "Error"),
			qApp->translate("EmuThread", "Failed to write downloaded data to file '%1'.").arg(QString::fromStdString(path)));
		return false;
	}

	return true;
}

void Host::ReportInfoAsync(const std::string_view title, const std::string_view message)
{
	if (!title.empty() && !message.empty())
		INFO_LOG("ReportInfoAsync: {}: {}", title, message);
	else if (!message.empty())
		INFO_LOG("ReportInfoAsync: {}", message);

	QMetaObject::invokeMethod(g_main_window, "reportInfo", Qt::QueuedConnection,
		Q_ARG(const QString&, title.empty() ? QString() : QString::fromUtf8(title.data(), title.size())),
		Q_ARG(const QString&, message.empty() ? QString() : QString::fromUtf8(message.data(), message.size())));
}

void Host::ReportErrorAsync(const std::string_view title, const std::string_view message)
{
	if (!title.empty() && !message.empty())
		ERROR_LOG("ReportErrorAsync: {}: {}", title, message);
	else if (!message.empty())
		ERROR_LOG("ReportErrorAsync: {}", message);

	QMetaObject::invokeMethod(g_main_window, "reportError", Qt::QueuedConnection,
		Q_ARG(const QString&, title.empty() ? QString() : QString::fromUtf8(title.data(), title.size())),
		Q_ARG(const QString&, message.empty() ? QString() : QString::fromUtf8(message.data(), message.size())));
}

void Host::OpenURL(const std::string_view url)
{
	QtHost::RunOnUIThread([url = QtUtils::StringViewToQString(url)]() { QtUtils::OpenURL(g_main_window, QUrl(url)); });
}

bool Host::CopyTextToClipboard(const std::string_view text)
{
	QtHost::RunOnUIThread([text = QtUtils::StringViewToQString(text)]() {
		QClipboard* clipboard = QGuiApplication::clipboard();
		if (clipboard)
			clipboard->setText(text);
	});
	return true;
}

std::string Host::GetTextFromClipboard()
{
	std::lock_guard<std::mutex> lock(s_clipboard_cache_mutex);
	return s_clipboard_cache;
}

void Host::BeginTextInput()
{
	QInputMethod* method = qApp->inputMethod();
	if (method)
		QMetaObject::invokeMethod(method, "show", Qt::QueuedConnection);
}

void Host::EndTextInput()
{
	QInputMethod* method = qApp->inputMethod();
	if (method)
		QMetaObject::invokeMethod(method, "hide", Qt::QueuedConnection);
}

std::optional<WindowInfo> Host::GetTopLevelWindowInfo()
{
	std::optional<WindowInfo> ret;
	QMetaObject::invokeMethod(g_main_window, &MainWindow::getWindowInfo, Qt::BlockingQueuedConnection, &ret);
	return ret;
}

void Host::OnInputDeviceConnected(const std::string_view identifier, const std::string_view device_name)
{
	emit g_emu_thread->onInputDeviceConnected(identifier.empty() ? QString() : QString::fromUtf8(identifier.data(), identifier.size()),
		device_name.empty() ? QString() : QString::fromUtf8(device_name.data(), device_name.size()));


	if (VMManager::HasValidVM() || g_emu_thread->isRunningFullscreenUI())
	{
		Host::AddIconOSDMessage(fmt::format("controller_connected_{}", identifier), ICON_FA_GAMEPAD,
			fmt::format(TRANSLATE_FS("QtHost", "Controller {} connected."), identifier),
			Host::OSD_INFO_DURATION);
	}
}

void Host::OnInputDeviceDisconnected(const InputBindingKey key, const std::string_view identifier)
{
	emit g_emu_thread->onInputDeviceDisconnected(identifier.empty() ? QString() : QString::fromUtf8(identifier.data(), identifier.size()));

	if (VMManager::GetState() == VMState::Running && Host::GetBoolSettingValue("UI", "PauseOnControllerDisconnection", false) &&
		InputManager::HasAnyBindingsForSource(key))
	{
		std::string message =
			fmt::format(TRANSLATE_FS("QtHost", "System paused because controller {} was disconnected."), identifier);
		Host::RunOnCPUThread([message = QString::fromStdString(message)]() {
			VMManager::SetPaused(true);

			emit g_emu_thread->statusMessage(message);
		});
		Host::AddIconOSDMessage(fmt::format("controller_connected_{}", identifier), ICON_FA_GAMEPAD, std::move(message),
			Host::OSD_WARNING_DURATION);
	}
	else if (VMManager::HasValidVM() || g_emu_thread->isRunningFullscreenUI())
	{
		Host::AddIconOSDMessage(fmt::format("controller_connected_{}", identifier), ICON_FA_GAMEPAD,
			fmt::format(TRANSLATE_FS("QtHost", "Controller {} disconnected."), identifier),
			Host::OSD_INFO_DURATION);
	}
}

void Host::SetMouseMode(bool relative_mode, bool hide_cursor)
{
	emit g_emu_thread->onMouseModeRequested(relative_mode, hide_cursor);
}

void Host::SetMouseLock(bool state)
{
	emit g_emu_thread->onMouseLockRequested(state);
}

void QtHost::LockVMWithDialog()
{
	s_vm_locked_with_dialog++;
}

void QtHost::UnlockVMWithDialog()
{
	s_vm_locked_with_dialog--;
}

namespace
{
	class QtHostProgressCallback final : public BaseProgressCallback
	{
	public:
		QtHostProgressCallback();
		~QtHostProgressCallback() override;

		__fi const std::string& GetName() const { return m_name; }

		void PushState() override;
		void PopState() override;

		bool IsCancelled() const override;

		void SetCancellable(bool cancellable) override;
		void SetTitle(const char* title) override;
		void SetStatusText(const char* text) override;
		void SetProgressRange(u32 range) override;
		void SetProgressValue(u32 value) override;

		void DisplayError(const char* message) override;
		void DisplayWarning(const char* message) override;
		void DisplayInformation(const char* message) override;
		void DisplayDebugMessage(const char* message) override;

		void ModalError(const char* message) override;
		bool ModalConfirmation(const char* message) override;
		void ModalInformation(const char* message) override;

		void SetCancelled();

	private:
		struct SharedData
		{
			QProgressDialog* dialog = nullptr;
			QString init_title;
			QString init_status_text;
			std::atomic_bool cancelled{false};
			bool cancellable = true;
			bool was_fullscreen = false;
		};

		void EnsureHasData();
		static void EnsureDialogVisible(const std::shared_ptr<SharedData>& data);
		void Redraw(bool force);

		std::string m_name;
		std::shared_ptr<SharedData> m_data;
		int m_last_progress_percent = -1;
	};
}

QtHostProgressCallback::QtHostProgressCallback()
	: BaseProgressCallback()
{
}

QtHostProgressCallback::~QtHostProgressCallback()
{
	if (m_data)
	{
		QtHost::RunOnUIThread([data = m_data]() {
			if (!data->dialog)
				return;

			data->dialog->close();
			delete data->dialog;
			if (data->was_fullscreen)
				g_emu_thread->setFullscreen(true, false);
		});
	}
}

void QtHostProgressCallback::PushState()
{
	BaseProgressCallback::PushState();
}

void QtHostProgressCallback::PopState()
{
	BaseProgressCallback::PopState();
	Redraw(true);
}

void QtHostProgressCallback::SetCancellable(bool cancellable)
{
	BaseProgressCallback::SetCancellable(cancellable);
	EnsureHasData();
	m_data->cancellable = cancellable;
}

void QtHostProgressCallback::SetTitle(const char* title)
{
	EnsureHasData();
	QtHost::RunOnUIThread([data = m_data, title = QString::fromUtf8(title)]() {
		if (data->dialog)
			data->dialog->setWindowTitle(title);
		else
			data->init_title = title;
	});
}

void QtHostProgressCallback::SetStatusText(const char* text)
{
	BaseProgressCallback::SetStatusText(text);

	EnsureHasData();
	QtHost::RunOnUIThread([data = m_data, text = QString::fromUtf8(text)]() {
		if (data->dialog)
			data->dialog->setLabelText(text);
		else
			data->init_status_text = text;
	});
}

void QtHostProgressCallback::SetProgressRange(u32 range)
{
	u32 last_range = m_progress_range;

	BaseProgressCallback::SetProgressRange(range);

	if (m_progress_range != last_range)
		Redraw(false);
}

void QtHostProgressCallback::SetProgressValue(u32 value)
{
	u32 lastValue = m_progress_value;

	BaseProgressCallback::SetProgressValue(value);

	if (m_progress_value != lastValue)
		Redraw(false);
}

void QtHostProgressCallback::Redraw(bool force)
{
	const int percent = static_cast<int>((static_cast<float>(m_progress_value) / static_cast<float>(m_progress_range)) * 100.0f);
	if (percent == m_last_progress_percent && !force)
		return;

	if (g_emu_thread->isOnEmuThread())
		Host::PumpMessagesOnCPUThread();

	m_last_progress_percent = percent;
	EnsureHasData();
	QtHost::RunOnUIThread([data = m_data, percent]() {
		EnsureDialogVisible(data);
		data->dialog->setValue(percent);
	});
}

void QtHostProgressCallback::DisplayError(const char* message)
{
	Console.Error(message);
	Host::ReportErrorAsync("Error", message);
}

void QtHostProgressCallback::DisplayWarning(const char* message)
{
	Console.Warning(message);
}

void QtHostProgressCallback::DisplayInformation(const char* message)
{
	Console.WriteLn(message);
}

void QtHostProgressCallback::DisplayDebugMessage(const char* message)
{
	DevCon.WriteLn(message);
}

void QtHostProgressCallback::ModalError(const char* message)
{
	Console.Error(message);
	Host::ReportErrorAsync("Error", message);
}

bool QtHostProgressCallback::ModalConfirmation(const char* message)
{
	return false;
}

void QtHostProgressCallback::ModalInformation(const char* message)
{
	Console.WriteLn(message);
}

void QtHostProgressCallback::SetCancelled()
{
}

bool QtHostProgressCallback::IsCancelled() const
{
	return m_data && m_data->cancelled.load(std::memory_order_acquire);
}

void QtHostProgressCallback::EnsureHasData()
{
	if (!m_data)
		m_data = std::make_shared<SharedData>();
}

void QtHostProgressCallback::EnsureDialogVisible(const std::shared_ptr<SharedData>& data)
{
	pxAssert(data);
	if (data->dialog)
		return;

	data->was_fullscreen = g_emu_thread->isFullscreen();
	if (data->was_fullscreen)
		g_emu_thread->setFullscreen(false, true);

	data->dialog = new QProgressDialog(data->init_status_text,
		data->cancellable ? qApp->translate("QtHost", "Cancel") : QString(),
		0, 100, g_main_window);
	if (data->cancellable)
	{
		data->dialog->connect(data->dialog, &QProgressDialog::canceled,
			[data]() { data->cancelled.store(true, std::memory_order_release); });
	}
	data->dialog->setWindowIcon(QtHost::GetAppIcon());
	data->dialog->setMinimumWidth(400);
	data->dialog->show();
	data->dialog->raise();
	data->dialog->activateWindow();
	if (!data->init_title.isEmpty())
	{
		data->dialog->setWindowTitle(data->init_title);
		data->init_title = QString();
	}
}

std::unique_ptr<ProgressCallback> Host::CreateHostProgressCallback()
{
	return std::make_unique<QtHostProgressCallback>();
}

BEGIN_HOTKEY_LIST(g_host_hotkeys)
END_HOTKEY_LIST()


static void SignalHandler(int signal)
{
	static bool graceful_shutdown_attempted = false;
	if (!graceful_shutdown_attempted && g_main_window)
	{
		std::fprintf(stderr, "Received CTRL+C, attempting graceful shutdown. Press CTRL+C again to force.\n");
		graceful_shutdown_attempted = true;

		QMetaObject::invokeMethod(g_main_window, "requestExit", Qt::QueuedConnection, Q_ARG(bool, false));
		return;
	}

	std::signal(signal, SIG_DFL);

#ifndef __APPLE__
	std::quick_exit(1);
#else
	_Exit(1);
#endif
}

#ifdef _WIN32

static BOOL WINAPI ConsoleCtrlHandler(DWORD dwCtrlType)
{
	if (dwCtrlType != CTRL_C_EVENT)
		return FALSE;

	SignalHandler(SIGTERM);
	return TRUE;
}

#endif

void QtHost::HookSignals()
{
	std::signal(SIGINT, SignalHandler);
	std::signal(SIGTERM, SignalHandler);

#if defined(_WIN32)
	SetConsoleCtrlHandler(ConsoleCtrlHandler, FALSE);
#elif defined(__linux__)
	struct sigaction sa_chld = {};
	sigemptyset(&sa_chld.sa_mask);
	sa_chld.sa_flags = SA_SIGINFO | SA_RESTART | SA_NOCLDSTOP | SA_NOCLDWAIT;
	sigaction(SIGCHLD, &sa_chld, nullptr);
#endif
}

void QtHost::InitializeEarlyConsole()
{
	Log::SetConsoleOutputLevel(LOGLEVEL_DEBUG);
}

void QtHost::PrintCommandLineVersion()
{
	InitializeEarlyConsole();
	std::fprintf(stderr, "%s\n", (GetAppNameAndVersion() + GetAppConfigSuffix()).toUtf8().constData());
	std::fprintf(stderr, "https://pcsx2.net/\n");
	std::fprintf(stderr, "\n");
}

void QtHost::PrintCommandLineHelp(const std::string_view progname)
{
	PrintCommandLineVersion();
	fmt::print(stderr, "Usage: {} [parameters] [--] [boot filename]\n", progname);
	std::fprintf(stderr, "\n");
	std::fprintf(stderr, "  -help: Displays this information and exits.\n");
	std::fprintf(stderr, "  -version: Displays version information and exits.\n");
	std::fprintf(stderr, "  -batch: Enables batch mode (exits after shutting down).\n");
	std::fprintf(stderr, "  -nogui: Hides main window while running (implies batch mode).\n");
	std::fprintf(stderr, "  -portable: Force enable portable mode to store data in local PenguinScreen2 path instead of the default configuration path. Overrides '-datapath'.\n");
	std::fprintf(stderr, "  -datapath <path>: Specify the directory to be used for all application data.\n");
	std::fprintf(stderr, "  -elf <file>: Overrides the boot ELF with the specified filename.\n");
	std::fprintf(stderr, "  -gameargs <string>: passes the specified quoted space-delimited string of launch arguments.\n");
	std::fprintf(stderr, "  -disc <path>: Uses the specified host DVD drive as a source.\n");
	std::fprintf(stderr, "  -logfile <path>: Writes the application log to path instead of emulog.txt.\n");
	std::fprintf(stderr, "  -bios: Starts the BIOS (System Menu/OSDSYS).\n");
	std::fprintf(stderr, "  -fastboot: Force fast boot for provided filename.\n");
	std::fprintf(stderr, "  -slowboot: Force slow boot for provided filename.\n");
	std::fprintf(stderr, "  -state <index>: Loads specified save state by index.\n");
	std::fprintf(stderr, "  -statefile <filename>: Loads state from the specified filename.\n");
	std::fprintf(stderr, "  -fullscreen: Enters fullscreen mode immediately after starting.\n");
	std::fprintf(stderr, "  -nofullscreen: Prevents fullscreen mode from triggering if enabled.\n");
	std::fprintf(stderr, "  -bigpicture: Forces PenguinScreen2 to use the Big Picture mode (useful for controller-only and couch play).\n");
	std::fprintf(stderr, "  -earlyconsolelog: Forces logging of early console messages to console.\n");
	std::fprintf(stderr, "  -testconfig: Initializes configuration and checks version, then exits.\n");
	std::fprintf(stderr, "  -setupwizard: Forces initial setup wizard to run.\n");
	std::fprintf(stderr, "  -debugger: Open debugger and break on entry point.\n");
	std::fprintf(stderr, "  -turbo: Enters turbo (fast forward) mode after starting.\n");
	std::fprintf(stderr, "  -unlimited: Enters unlimited (fast forward) mode after starting.\n");
#ifdef ENABLE_RAINTEGRATION
	std::fprintf(stderr, "  -raintegration: Use RAIntegration instead of built-in achievement support.\n");
#endif
#ifdef ENABLE_VR
	std::fprintf(stderr, "  --vr: Arm VR for THIS launch (explicit per-invocation signal; no environment\n"
						 "    variable ever arms VR). Without it the binary runs flat and never creates\n"
						 "    an OpenXR instance. The shipped launch-vr-session.sh passes it.\n");
	std::fprintf(stderr, "  -vr-info: Prints OpenXR runtime/headset information and exits.\n");
#endif
	std::fprintf(stderr, "  --: Signals that no more arguments will follow and the remaining\n"
						 "    parameters make up the filename. Use when the filename contains\n"
						 "    spaces or starts with a dash.\n");
	std::fprintf(stderr, "\n");
}

std::shared_ptr<VMBootParameters>& QtHost::AutoBoot(std::shared_ptr<VMBootParameters>& autoboot)
{
	if (!autoboot)
		autoboot = std::make_shared<VMBootParameters>();

	return autoboot;
}

bool QtHost::ParseCommandLineOptions(const QStringList& args, std::shared_ptr<VMBootParameters>& autoboot)
{
	bool no_more_args = false;

	if (args.empty())
	{
		return true;
	}

	for (auto it = std::next(args.begin()); it != args.end(); ++it)
	{
		if (!no_more_args)
		{
#define CHECK_ARG(str) (*it == str)
#define CHECK_ARG_PARAM(str) (*it == str && std::next(it) != args.end())

			if (CHECK_ARG(QStringLiteral("-help")))
			{
				PrintCommandLineHelp(args.front().toStdString());
				return false;
			}
			else if (CHECK_ARG(QStringLiteral("-version")))
			{
				PrintCommandLineVersion();
				return false;
			}
#ifdef ENABLE_VR
			else if (CHECK_ARG(QStringLiteral("--vr")) || CHECK_ARG(QStringLiteral("-vr")))
			{
				VR::SetLaunchRequestedVR(true);
				continue;
			}
			else if (CHECK_ARG(QStringLiteral("-vr-info")))
			{
				PrintCommandLineVersion();
				std::fprintf(stderr, "%s", VR::GetRuntimeInfoReport().c_str());
				return false;
			}
#endif
			else if (CHECK_ARG(QStringLiteral("-batch")))
			{
				s_batch_mode = true;
				continue;
			}
			else if (CHECK_ARG(QStringLiteral("-nogui")))
			{
				s_batch_mode = true;
				s_nogui_mode = true;
				continue;
			}
			else if (CHECK_ARG(QStringLiteral("-portable")))
			{
				EmuConfig.IsPortableMode = true;
				continue;
			}
			else if (CHECK_ARG_PARAM(QStringLiteral("-datapath")))
			{
				std::string path = (++it)->toStdString();
				EmuConfig.CustomDataPath = path;
				continue;
			}
			else if (CHECK_ARG(QStringLiteral("-fastboot")))
			{
				AutoBoot(autoboot)->fast_boot = true;
				continue;
			}
			else if (CHECK_ARG(QStringLiteral("-slowboot")))
			{
				AutoBoot(autoboot)->fast_boot = false;
				continue;
			}
			else if (CHECK_ARG_PARAM(QStringLiteral("-state")))
			{
				AutoBoot(autoboot)->state_index = (++it)->toInt();
				continue;
			}
			else if (CHECK_ARG_PARAM(QStringLiteral("-statefile")))
			{
				AutoBoot(autoboot)->save_state = (++it)->toStdString();
				continue;
			}
			else if (CHECK_ARG_PARAM(QStringLiteral("-elf")))
			{
				AutoBoot(autoboot)->elf_override = (++it)->toStdString();
				continue;
			}
			else if (CHECK_ARG_PARAM(QStringLiteral("-gameargs")))
			{
				EmuConfig.CurrentGameArgs = (++it)->toStdString();
				continue;
			}
			else if (CHECK_ARG_PARAM(QStringLiteral("-disc")))
			{
				AutoBoot(autoboot)->source_type = CDVD_SourceType::Disc;
				AutoBoot(autoboot)->filename = (++it)->toStdString();
				continue;
			}
			else if (CHECK_ARG_PARAM(QStringLiteral("-logfile")))
			{
				VMManager::Internal::SetFileLogPath((++it)->toStdString());
				continue;
			}
			else if (CHECK_ARG(QStringLiteral("-bios")))
			{
				AutoBoot(autoboot)->source_type = CDVD_SourceType::NoDisc;
				continue;
			}
			else if (CHECK_ARG(QStringLiteral("-fullscreen")))
			{
				AutoBoot(autoboot)->fullscreen = true;
				s_start_fullscreen = true;
				continue;
			}
			else if (CHECK_ARG(QStringLiteral("-nofullscreen")))
			{
				AutoBoot(autoboot)->fullscreen = false;
				continue;
			}
			else if (CHECK_ARG(QStringLiteral("-earlyconsolelog")))
			{
				InitializeEarlyConsole();
				continue;
			}
			else if (CHECK_ARG(QStringLiteral("-bigpicture")))
			{
				s_start_big_picture_mode = true;
				continue;
			}
			else if (CHECK_ARG(QStringLiteral("-testconfig")))
			{
				s_test_config_and_exit = true;
				continue;
			}
			else if (CHECK_ARG(QStringLiteral("-setupwizard")))
			{
				s_run_setup_wizard = true;
				continue;
			}
			else if (CHECK_ARG(QStringLiteral("-debugger")))
			{
				s_boot_and_debug = true;
				continue;
			}
			else if (CHECK_ARG(QStringLiteral("-updatecleanup")))
			{
				s_cleanup_after_update = AutoUpdaterDialog::isSupported();
				continue;
			}
			else if (CHECK_ARG(QStringLiteral("-turbo")))
			{
				AutoBoot(autoboot)->start_turbo = true;
				continue;
			}
			else if (CHECK_ARG(QStringLiteral("-unlimited")))
			{
				AutoBoot(autoboot)->start_unlimited = true;
				continue;
			}
#ifdef ENABLE_RAINTEGRATION
			else if (CHECK_ARG(QStringLiteral("-raintegration")))
			{
				Achievements::SwitchToRAIntegration();
				continue;
			}
#endif
			else if (CHECK_ARG(QStringLiteral("--")))
			{
				no_more_args = true;
				continue;
			}
			else if ((*it)[0] == '-')
			{
				QMessageBox::critical(nullptr, QStringLiteral("Error"), QStringLiteral("Unknown parameter: '%1'").arg(*it));
				return false;
			}

#undef CHECK_ARG
#undef CHECK_ARG_PARAM
		}

		if (!AutoBoot(autoboot)->filename.empty())
			AutoBoot(autoboot)->filename += ' ';

		AutoBoot(autoboot)->filename += it->toStdString();
	}

	if (autoboot && !autoboot->source_type.has_value() && autoboot->filename.empty() && autoboot->elf_override.empty())
	{
		Console.Warning("Skipping autoboot due to no boot parameters.");
		autoboot.reset();
	}

	if(autoboot && autoboot->start_turbo.value_or(false) && autoboot->start_unlimited.value_or(false))
	{
		Console.Warning("Both turbo and unlimited frame limit modes requested. Using unlimited.");
		autoboot->start_turbo.reset();
	}

	if (s_batch_mode && !s_start_big_picture_mode && !autoboot)
	{
		QMessageBox::critical(nullptr, QStringLiteral("Error"),
			s_nogui_mode ? QStringLiteral("Cannot use no-gui mode, because no boot filename was specified.") :
						   QStringLiteral("Cannot use batch mode, because no boot filename was specified."));
		return false;
	}

	return true;
}

#ifndef _WIN32

static bool PerformEarlyHardwareChecks()
{
	const char* error;
	if (VMManager::PerformEarlyHardwareChecks(&error))
		return true;

	QMessageBox::critical(nullptr, QStringLiteral("Hardware Check Failed"), QString::fromUtf8(error));
	return false;
}

#endif

void QtHost::RegisterTypes()
{
	qRegisterMetaType<std::optional<bool>>();
	qRegisterMetaType<std::optional<WindowInfo>>("std::optional<WindowInfo>()");
	qRegisterMetaType<std::function<void()>>("std::function<void()>");
	qRegisterMetaType<std::shared_ptr<VMBootParameters>>();
	qRegisterMetaType<GSRendererType>();
	qRegisterMetaType<InputBindingKey>();
	qRegisterMetaType<CDVD_SourceType>();
	qRegisterMetaType<const GameList::Entry*>();
	qRegisterMetaType<Achievements::LoginRequestReason>();
}

void QtHost::InitializeClipboard()
{
	QClipboard* clipboard = QGuiApplication::clipboard();
	if (clipboard)
	{
		{
			std::lock_guard<std::mutex> lock(s_clipboard_cache_mutex);
			s_clipboard_cache = clipboard->text().toStdString();
		}
		QObject::connect(clipboard, &QClipboard::dataChanged, []() {
			QClipboard* cb = QGuiApplication::clipboard();
			if (cb)
			{
				std::lock_guard<std::mutex> lock(s_clipboard_cache_mutex);
				s_clipboard_cache = cb->text().toStdString();
			}
		});
	}
}

bool QtHost::RunSetupWizard()
{
	SetupWizardDialog dialog;
	if (dialog.exec() == QDialog::Rejected)
		return false;

	Host::SetBaseBoolSettingValue("UI", "SetupWizardIncomplete", false);
	Host::CommitBaseSettingChanges();
	return true;
}

class PCSX2MainApplication : public QApplication
{
public:
	using QApplication::QApplication;

	bool event(QEvent* event) override
	{
		if (event->type() == QEvent::FileOpen)
		{
			QFileOpenEvent* open = static_cast<QFileOpenEvent*>(event);
			const QUrl url = open->url();
			if (url.isLocalFile())
				return g_main_window->startFile(url.toLocalFile());
			else
				return false;
		}
		return QApplication::event(event);
	}
};

int main(int argc, char* argv[])
{
	CrashHandler::Install();

#ifdef _WIN32
	std::locale::global(std::locale(""));
#endif

	QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

	QGuiApplication::setDesktopFileName(QStringLiteral("org.penguinvr.penguinscreen2"));
	QtHost::RegisterTypes();

	PCSX2MainApplication app(argc, argv);

	QtHost::InitializeClipboard();

#ifndef _WIN32
	if (!PerformEarlyHardwareChecks())
		return EXIT_FAILURE;
#endif

	std::shared_ptr<VMBootParameters> autoboot;
	if (!QtHost::ParseCommandLineOptions(app.arguments(), autoboot))
		return EXIT_FAILURE;

	if (!QtHost::InitializeConfig())
		return EXIT_FAILURE;

	if (s_test_config_and_exit)
		return EXIT_SUCCESS;

#ifdef ENABLE_VR
	{
		const auto& issues = VR::ProfileDB::ValidateAtLaunch();
		if (!issues.empty())
		{
			QString msg = QStringLiteral("%1 invalid VR profile file(s) found at launch:\n\n").arg(issues.size());
			for (const auto& issue : issues)
			{
				msg += QStringLiteral("%1\n    %2\n\n")
						   .arg(QString::fromStdString(issue.file), QString::fromStdString(issue.message));
			}
			msg += QStringLiteral("These files are SKIPPED for this session. Where a shipped profile "
								  "exists for the same game, it is used instead. Fix or remove the "
								  "listed files — this check runs at every launch.");
			QMessageBox::critical(nullptr, QStringLiteral("Invalid VR profiles"), msg);
		}
	}
#endif

	if (s_cleanup_after_update)
		AutoUpdaterDialog::cleanupAfterUpdate();

	QtHost::UpdateApplicationTheme();

	LogWindow::updateSettings();

	QtHost::HookSignals();
	EmuThread::start();

	int result;
	if (s_run_setup_wizard && !QtHost::RunSetupWizard())
	{
		result = EXIT_FAILURE;
		goto shutdown_and_exit;
	}

	g_main_window = new MainWindow();
	g_main_window->initialize();

	if (!s_batch_mode)
		g_main_window->refreshGameList(false, false);
	else
		GameList::Refresh(false, true);

	if (!s_nogui_mode)
	{
		g_main_window->show();
		g_main_window->raise();
		g_main_window->activateWindow();
	}

	if (s_start_big_picture_mode || Host::GetBaseBoolSettingValue("UI", "StartBigPictureMode", false))
		g_emu_thread->startFullscreenUI(s_start_fullscreen || Host::GetBaseBoolSettingValue("UI", "StartFullscreen", false));

	if (s_boot_and_debug || DebuggerWindow::shouldShowOnStartup())
	{
		DebugInterface::setPauseOnEntry(s_boot_and_debug);
		g_main_window->openDebugger();
	}

	if (autoboot)
		g_emu_thread->startVM(std::move(autoboot));
	else if (!s_nogui_mode)
		g_main_window->startupUpdateCheck();

	result = app.exec();

shutdown_and_exit:
	EmuThread::stop();
	if (g_main_window)
	{
		g_main_window->close();
		delete g_main_window;
	}

	if (s_base_settings_interface->IsDirty())
		s_base_settings_interface->Save();

	return result;
}

#include "moc_QtHost.cpp"
