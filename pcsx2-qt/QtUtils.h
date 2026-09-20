// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/WindowInfo.h"

#include <QtCore/QByteArray>
#include <QtCore/QMetaType>
#include <QtCore/QString>
#include <QtCore/QAbstractItemModel>
#include <QtGui/QGuiApplication>
#include <QtGui/QWindow>
#if !defined(_WIN32) and !defined(__APPLE__)
#include <qpa/qplatformnativeinterface.h>
#endif
#include <QtGui/QScreen>
#include <functional>
#include <initializer_list>
#include <string>
#include <string_view>
#include <type_traits>
#include <optional>
#include <vector>

#include "common/Console.h"

class ByteStream;

class QAction;
class QComboBox;
class QFileInfo;
class QFrame;
class QIcon;
class QLabel;
class QKeyEvent;
class QSlider;
class QTableView;
class QTreeView;
class QVariant;
class QWidget;
class QUrl;

namespace QtUtils
{
	static constexpr float MOUSE_WHEEL_DELTA = 120.0f;

	void MarkActionAsDefault(QAction* action);

	QFrame* CreateHorizontalLine(QWidget* parent);

	QWidget* GetRootWidget(QWidget* widget, bool stop_at_window_or_dialog = true);

	void ResizeColumnsForTableView(QTableView* view, const std::initializer_list<int>& widths);
	void ResizeColumnsForTreeView(QTreeView* view, const std::initializer_list<int>& widths);

	enum struct ScalingMode
	{
		Fit,
		Fill,
		Stretch,
		Center,
		Tile,

		MaxCount
	};

	void resizeAndScalePixmap(QPixmap* pm, const int expected_width, const int expected_height, const qreal dpr, const ScalingMode scaling_mode, const float opacity);

	u32 KeyEventToCode(const QKeyEvent* ev);

	void ShowInFileExplorer(QWidget* parent, const QFileInfo& file);

	QString GetShowInFileExplorerMessage();

	void OpenURL(QWidget* parent, const QUrl& qurl);

	void OpenURL(QWidget* parent, const char* url);

	void OpenURL(QWidget* parent, const QString& url);

	QString StringViewToQString(const std::string_view str);

	void SetWidgetFontForInheritedSetting(QWidget* widget, bool inherited);

	void BindLabelToSlider(QSlider* slider, QLabel* label, float range = 1.0f);

	void SetWindowResizeable(QWidget* widget, bool resizeable);
	void SetWindowResizeable(QWindow* window, bool resizeable);

	void ResizePotentiallyFixedSizeWindow(QWidget* widget, int width, int height);
	void ResizePotentiallyFixedSizeWindow(QWindow* window, int width, int height);

	template <class T>
		requires std::is_base_of_v<QWidget, T> || std::is_base_of_v<QWindow, T>
	std::optional<WindowInfo> GetWindowInfoForWindow(T* window)
	{
		WindowInfo wi;

#if defined(_WIN32)
		wi.type = WindowInfo::Type::Win32;
		wi.window_handle = reinterpret_cast<void*>(window->winId());
#elif defined(__APPLE__)
		wi.type = WindowInfo::Type::MacOS;
		wi.window_handle = reinterpret_cast<void*>(window->winId());
#else
		QPlatformNativeInterface* pni = QGuiApplication::platformNativeInterface();
		const QString platform_name = QGuiApplication::platformName();

		QWindow* windowHandle;
		if constexpr (std::is_base_of_v<QWidget, T>)
			windowHandle = window->windowHandle();
		else
			windowHandle = window;

		if (platform_name == QStringLiteral("xcb"))
		{
			if (!window->isVisible())
			{
				Console.WriteLn("Returning null window info for widget because it is not visible.");
				return std::nullopt;
			}

			wi.type = WindowInfo::Type::X11;
			wi.display_connection = pni->nativeResourceForWindow("display", windowHandle);
			wi.window_handle = reinterpret_cast<void*>(window->winId());
		}
		else if (platform_name == QStringLiteral("wayland"))
		{
			wi.type = WindowInfo::Type::Wayland;
			wi.display_connection = pni->nativeResourceForWindow("display", windowHandle);
			wi.window_handle = pni->nativeResourceForWindow("surface", windowHandle);
		}
		else if (platform_name == QStringLiteral("offscreen"))
		{
			wi.type = WindowInfo::Type::Surfaceless;
			wi.window_handle = nullptr;
		}
		else
		{
			Console.WriteLn("Unknown PNI platform '%s'.", platform_name.toUtf8().constData());
			return std::nullopt;
		}
#endif

		qreal dpr;
		if constexpr (std::is_base_of_v<QWidget, T>)
			dpr = window->devicePixelRatioF();
		else
			dpr = window->devicePixelRatio();

		wi.surface_width = static_cast<u32>(std::max(static_cast<int>(std::round(static_cast<qreal>(window->width()) * dpr)), 1));
		wi.surface_height = static_cast<u32>(std::max(static_cast<int>(std::round(static_cast<qreal>(window->height()) * dpr)), 1));
		wi.surface_scale = static_cast<float>(dpr);

		std::optional<float> surface_refresh_rate = WindowInfo::QueryRefreshRateForWindow(wi);
		if (!surface_refresh_rate.has_value())
		{
			const QScreen* widget_screen = window->screen();
			if (!widget_screen)
				widget_screen = QGuiApplication::primaryScreen();
			surface_refresh_rate = widget_screen ? static_cast<float>(widget_screen->refreshRate()) : 0.0f;
		}

		wi.surface_refresh_rate = surface_refresh_rate.value();
		INFO_LOG("Surface refresh rate: {} Hz", wi.surface_refresh_rate);

		return wi;
	}

	template <typename T>
	QString FilledQStringFromValue(T val, u32 base)
	{
		return QString("%1").arg(QString::number(val, base), sizeof(val) * 2, '0').toUpper();
	};

	QString AbstractItemModelToCSV(QAbstractItemModel* model, int role = Qt::DisplayRole, bool useQuotes = false);

	bool IsCompositorManagerRunning();

	void SetScalableIcon(QLabel* lbl, const QIcon& icon, const QSize& size);

	QString GetSystemLanguageCode();

	QIcon GetFlagIconForLanguage(const QString& language_code);

	bool IsRunningInFlatpak();

	bool IsRunningInAppImage();

	void CreateShortcut(QWidget* parent, const std::string& name, const std::string& game_path,
		std::vector<std::string> passed_cli_args, const std::string& custom_args,
		const std::string& icon_path, bool is_desktop, bool prompt_for_destination = true);

	bool EscapeShortcutCommandLine(std::string* arg);
}
