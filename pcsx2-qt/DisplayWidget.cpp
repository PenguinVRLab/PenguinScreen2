// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "DisplayWidget.h"
#include "MainWindow.h"
#include "QtHost.h"
#include "QtUtils.h"

#include "pcsx2/ImGui/FullscreenUI.h"
#include "pcsx2/ImGui/ImGuiManager.h"

#include "common/Assertions.h"
#include "common/Console.h"

#include <QtCore/QDebug>
#include <QtCore/QTimer>
#include <QtGui/QGuiApplication>
#include <QtGui/QKeyEvent>
#include <QtGui/QResizeEvent>
#include <QtGui/QScreen>
#include <QtGui/QWindow>
#include <QtGui/QWindowStateChangeEvent>

#include <bit>
#include <cmath>

#if defined(_WIN32)
#include "common/RedtapeWindows.h"
#endif

DisplaySurface::DisplaySurface()
	: QWindow()
{
	m_resize_debounce_timer = new QTimer(this);
	m_resize_debounce_timer->setSingleShot(true);
	m_resize_debounce_timer->setTimerType(Qt::PreciseTimer);
	connect(m_resize_debounce_timer, &QTimer::timeout, this, &DisplaySurface::onResizeDebounceTimer);
}

DisplaySurface::~DisplaySurface()
{
#ifdef _WIN32
	if (m_clip_mouse_enabled)
		ClipCursor(nullptr);
#endif
}

QWidget* DisplaySurface::createWindowContainer(QWidget* parent)
{
	m_container = QWidget::createWindowContainer(this, parent);
	m_container->installEventFilter(this);
	m_container->setFocusPolicy(Qt::StrongFocus);
	return m_container;
}

std::optional<WindowInfo> DisplaySurface::getWindowInfo()
{
	std::optional<WindowInfo> ret(QtUtils::GetWindowInfoForWindow(this));
	if (ret.has_value())
	{
		m_last_window_width = ret->surface_width;
		m_last_window_height = ret->surface_height;
		m_last_window_scale = ret->surface_scale;
	}
	return ret;
}

void DisplaySurface::updateRelativeMode(bool enabled)
{
#ifdef _WIN32
	bool clip_cursor = enabled && false ;
	if (m_relative_mouse_enabled == enabled && m_clip_mouse_enabled == clip_cursor)
		return;

	DevCon.WriteLn("updateRelativeMode(): relative=%s, clip=%s", enabled ? "yes" : "no", clip_cursor ? "yes" : "no");

	if (!clip_cursor && m_clip_mouse_enabled)
	{
		m_clip_mouse_enabled = false;
		ClipCursor(nullptr);
	}
#else
	if (m_relative_mouse_enabled == enabled)
		return;

	DevCon.WriteLn("updateRelativeMode(): relative=%s", enabled ? "yes" : "no");
#endif

	if (enabled)
	{
#ifdef _WIN32
		m_relative_mouse_enabled = !clip_cursor;
		m_clip_mouse_enabled = clip_cursor;
#else
		m_relative_mouse_enabled = true;
#endif
		m_relative_mouse_start_pos = QCursor::pos();
		updateCenterPos();
		setMouseGrabEnabled(true);
	}
	else if (m_relative_mouse_enabled)
	{
		m_relative_mouse_enabled = false;
		QCursor::setPos(m_relative_mouse_start_pos);
		setMouseGrabEnabled(false);
	}
}

void DisplaySurface::updateCursor(bool hidden)
{
	if (m_cursor_hidden == hidden)
		return;

	m_cursor_hidden = hidden;
	if (hidden)
	{
		DevCon.WriteLn("updateCursor(): Cursor is now hidden");
		setCursor(Qt::BlankCursor);
	}
	else
	{
		DevCon.WriteLn("updateCursor(): Cursor is now shown");
		unsetCursor();
	}
}

void DisplaySurface::handleCloseEvent(QCloseEvent* event)
{
	if (QtHost::IsVMValid() && !isFullScreen())
	{
		QMetaObject::invokeMethod(g_main_window, "requestShutdown", Q_ARG(bool, true),
			Q_ARG(bool, true), Q_ARG(bool, false));
	}
	else
	{
		QMetaObject::invokeMethod(g_main_window, "requestExit", Q_ARG(bool, true));
	}

	event->ignore();
}

bool DisplaySurface::isFullScreen() const
{
	return (parent() ? parent()->windowState() : windowState()) & Qt::WindowFullScreen;
}

void DisplaySurface::setFocus()
{
	if (m_container)
		m_container->setFocus();
	else
		requestActivate();
}

QByteArray DisplaySurface::saveGeometry() const
{
	if (m_container)
		return m_container->saveGeometry();
	else
	{
		QWidget dummy = QWidget();
		dummy.setGeometry(geometry());
		return dummy.saveGeometry();
	}
}

void DisplaySurface::restoreGeometry(const QByteArray& geometry)
{
	if (m_container)
		m_container->restoreGeometry(geometry);
	else
	{
		QWidget dummy = QWidget();
		dummy.restoreGeometry(geometry);
		setGeometry(dummy.geometry());
	}
}

void DisplaySurface::updateCenterPos()
{
#ifdef _WIN32
	if (m_clip_mouse_enabled)
	{
		RECT rc;
		if (GetWindowRect(reinterpret_cast<HWND>(winId()), &rc))
			ClipCursor(&rc);
	}
	else if (m_relative_mouse_enabled)
	{
		RECT rc;
		if (GetWindowRect(reinterpret_cast<HWND>(winId()), &rc))
		{
			m_relative_mouse_center_pos.setX(((rc.right - rc.left) / 2) + rc.left);
			m_relative_mouse_center_pos.setY(((rc.bottom - rc.top) / 2) + rc.top);
			SetCursorPos(m_relative_mouse_center_pos.x(), m_relative_mouse_center_pos.y());
		}
	}
#else
	if (m_relative_mouse_enabled)
	{
		m_relative_mouse_center_pos = mapToGlobal(QPoint((width() + 1) / 2, (height() + 1) / 2));
		QCursor::setPos(m_relative_mouse_center_pos);
		m_relative_mouse_center_pos = QCursor::pos();
	}
#endif
}

void DisplaySurface::handleKeyInputEvent(QEvent* event)
{
	switch (event->type())
	{
		case QEvent::KeyPress:
		case QEvent::KeyRelease:
		{
			const QKeyEvent* key_event = static_cast<QKeyEvent*>(event);

			if (ImGuiManager::WantsTextInput() && key_event->type() == QEvent::KeyPress)
			{
				QString text(key_event->text());
				text.remove(QChar('\b'));
				if (!text.isEmpty())
					ImGuiManager::AddTextInput(text.toStdString());
			}

			if (key_event->isAutoRepeat())
				return;

			const u32 key = QtUtils::KeyEventToCode(key_event);
			const Qt::KeyboardModifiers modifiers = key_event->modifiers();
			const bool pressed = (key_event->type() == QEvent::KeyPress);
			const auto it = std::find(m_keys_pressed_with_modifiers.begin(), m_keys_pressed_with_modifiers.end(), key);
			if (it != m_keys_pressed_with_modifiers.end())
			{
				if (pressed)
					return;
				else
					m_keys_pressed_with_modifiers.erase(it);
			}
			else if (modifiers != Qt::NoModifier && modifiers != Qt::KeypadModifier && pressed)
			{
				m_keys_pressed_with_modifiers.push_back(key);
			}

			Host::RunOnCPUThread([key, pressed]() {
				InputManager::InvokeEvents(InputManager::MakeHostKeyboardKey(key), static_cast<float>(pressed));
			});

			return;
		}

		default:
			pxAssert(false);
			return;
	}
}

void DisplaySurface::onResizeDebounceTimer()
{
	emit windowResizedEvent(m_pending_window_width, m_pending_window_height, m_pending_window_scale);
}

bool DisplaySurface::event(QEvent* event)
{
	switch (event->type())
	{
		case QEvent::KeyPress:
		case QEvent::KeyRelease:
		{
			handleKeyInputEvent(event);
			return true;
		}

		case QEvent::MouseMove:
		{
			const QMouseEvent* mouse_event = static_cast<QMouseEvent*>(event);

			if (!m_relative_mouse_enabled)
			{
				const qreal dpr = devicePixelRatio();
				const QPoint mouse_pos = mouse_event->pos();

				const float scaled_x = static_cast<float>(static_cast<qreal>(mouse_pos.x()) * dpr);
				const float scaled_y = static_cast<float>(static_cast<qreal>(mouse_pos.y()) * dpr);
				InputManager::UpdatePointerAbsolutePosition(0, scaled_x, scaled_y);
			}
			else
			{
				float dx = 0.0f, dy = 0.0f;

#ifndef _WIN32
				const QPoint mouse_pos = QCursor::pos();
				if (mouse_pos != m_relative_mouse_center_pos)
				{
					dx = static_cast<float>(mouse_pos.x() - m_relative_mouse_center_pos.x());
					dy = static_cast<float>(mouse_pos.y() - m_relative_mouse_center_pos.y());
					QCursor::setPos(m_relative_mouse_center_pos);
				}
#else
				POINT mouse_pos;
				if (GetCursorPos(&mouse_pos))
				{
					dx = static_cast<float>(mouse_pos.x - m_relative_mouse_center_pos.x());
					dy = static_cast<float>(mouse_pos.y - m_relative_mouse_center_pos.y());
					SetCursorPos(m_relative_mouse_center_pos.x(), m_relative_mouse_center_pos.y());
				}
#endif

				if (dx != 0.0f)
					InputManager::UpdatePointerRelativeDelta(0, InputPointerAxis::X, dx);
				if (dy != 0.0f)
					InputManager::UpdatePointerRelativeDelta(0, InputPointerAxis::Y, dy);
			}

			return true;
		}

		case QEvent::MouseButtonPress:
		case QEvent::MouseButtonDblClick:
		case QEvent::MouseButtonRelease:
		{
			if (const u32 button_mask = static_cast<u32>(static_cast<const QMouseEvent*>(event)->button()))
			{
				Host::RunOnCPUThread([button_index = std::countr_zero(button_mask),
										 pressed = (event->type() != QEvent::MouseButtonRelease)]() {
					InputManager::InvokeEvents(
						InputManager::MakePointerButtonKey(0, button_index), static_cast<float>(pressed));
				});
			}

			if (event->type() == QEvent::MouseButtonDblClick &&
				static_cast<const QMouseEvent*>(event)->button() == Qt::LeftButton &&
				QtHost::IsVMValid() && !FullscreenUI::HasActiveWindow() &&
				((!QtHost::IsVMPaused() && !InputManager::HasAnyBindingsForKey(InputManager::MakePointerButtonKey(0, 0))) ||
					(QtHost::IsVMPaused() && !ImGuiManager::WantsMouseInput())) &&
				Host::GetBoolSettingValue("UI", "DoubleClickTogglesFullscreen", true))
			{
				g_emu_thread->toggleFullscreen();
			}

			return true;
		}

		case QEvent::Wheel:
		{
			const QPoint delta_angle(static_cast<QWheelEvent*>(event)->angleDelta());
			const float dx = std::clamp(static_cast<float>(delta_angle.x()) / QtUtils::MOUSE_WHEEL_DELTA, -1.0f, 1.0f);
			if (dx != 0.0f)
				InputManager::UpdatePointerRelativeDelta(0, InputPointerAxis::WheelX, dx);

			const float dy = std::clamp(static_cast<float>(delta_angle.y()) / QtUtils::MOUSE_WHEEL_DELTA, -1.0f, 1.0f);
			if (dy != 0.0f)
				InputManager::UpdatePointerRelativeDelta(0, InputPointerAxis::WheelY, dy);

			return true;
		}

		case QEvent::DevicePixelRatioChange:
		case QEvent::Resize:
		{
			QWindow::event(event);

			const float dpr = devicePixelRatio();
			const u32 scaled_width = static_cast<u32>(std::max(static_cast<int>(std::round(static_cast<qreal>(width()) * dpr)), 1));
			const u32 scaled_height = static_cast<u32>(std::max(static_cast<int>(std::round(static_cast<qreal>(height()) * dpr)), 1));

			if (m_last_window_width != scaled_width || m_last_window_height != scaled_height || m_last_window_scale != dpr)
			{
				m_pending_window_width = scaled_width;
				m_pending_window_height = scaled_height;
				m_pending_window_scale = dpr;

				m_last_window_width = scaled_width;
				m_last_window_height = scaled_height;
				m_last_window_scale = dpr;
				m_resize_debounce_timer->start(100);
			}

			updateCenterPos();
			return true;
		}

		case QEvent::DragEnter:
			QWindow::event(event);
			emit dragEnterEvent(static_cast<QDragEnterEvent*>(event));
			return event->isAccepted();

		case QEvent::Drop:
			QWindow::event(event);
			emit dropEvent(static_cast<QDropEvent*>(event));
			return event->isAccepted();

		case QEvent::Move:
			updateCenterPos();
			return true;

		case QEvent::Close:
			handleCloseEvent(static_cast<QCloseEvent*>(event));
			return true;
		case QEvent::WindowStateChange:
			if (static_cast<QWindowStateChangeEvent*>(event)->oldState() & Qt::WindowMinimized)
				emit windowRestoredEvent();
			return false;

		default:
			return QWindow::event(event);
	}
}

bool DisplaySurface::eventFilter(QObject* object, QEvent* event)
{
	switch (event->type())
	{
		case QEvent::KeyPress:
		case QEvent::KeyRelease:
#ifdef _WIN32
			requestActivate();
#endif
			handleKeyInputEvent(event);
			return true;

		case QEvent::Close:
			handleCloseEvent(static_cast<QCloseEvent*>(event));
			return true;
		case QEvent::WindowStateChange:
			if (static_cast<QWindowStateChangeEvent*>(event)->oldState() & Qt::WindowMinimized)
				emit windowRestoredEvent();
			return false;

		case QEvent::ChildWindowRemoved:
			if (static_cast<QChildWindowEvent*>(event)->child() == this)
			{
				object->removeEventFilter(this);
				m_container = nullptr;
			}
			return false;

		case QEvent::FocusIn:

			if (QApplication::activeModalWidget() != nullptr)
				return false;

			if (const auto* w = qobject_cast<QWidget*>(object))
				w->window()->activateWindow();

			return false;
		default:
			return false;
	}
}

#include "moc_DisplayWidget.cpp"
