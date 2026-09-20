// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <kddockwidgets/core/indicators/ClassicDropIndicatorOverlay.h>
#include <kddockwidgets/core/views/ClassicIndicatorWindowViewInterface.h>
#include <kddockwidgets/qtwidgets/views/SegmentedDropIndicatorOverlay.h>

class DockDropIndicator;

class DockDropIndicatorProxy : public KDDockWidgets::Core::ClassicIndicatorWindowViewInterface
{
public:
	DockDropIndicatorProxy(KDDockWidgets::Core::ClassicDropIndicatorOverlay* classic_indicators);
	~DockDropIndicatorProxy();

	void setObjectName(const QString&) override;
	KDDockWidgets::DropLocation hover(QPoint globalPos) override;
	QPoint posForIndicator(KDDockWidgets::DropLocation) const override;
	void updatePositions() override;
	void raise() override;
	void setVisible(bool visible) override;
	void resize(QSize size) override;
	void setGeometry(QRect rect) override;
	bool isWindow() const override;
	void updateIndicatorVisibility() override;

private:
	KDDockWidgets::Core::ClassicIndicatorWindowViewInterface* window();
	const KDDockWidgets::Core::ClassicIndicatorWindowViewInterface* window() const;

	void recreateWindowIfNecessary();

	KDDockWidgets::Core::ClassicIndicatorWindowViewInterface* m_window = nullptr;
	KDDockWidgets::Core::ClassicIndicatorWindowViewInterface* m_fallback_window = nullptr;

	bool m_supports_compositing = true;
	KDDockWidgets::Core::ClassicDropIndicatorOverlay* m_classic_indicators = nullptr;
};

class DockDropIndicatorWindow : public QWidget, public KDDockWidgets::Core::ClassicIndicatorWindowViewInterface
{
	Q_OBJECT

public:
	DockDropIndicatorWindow(
		KDDockWidgets::Core::ClassicDropIndicatorOverlay* classic_indicators);

	void setObjectName(const QString& name) override;
	KDDockWidgets::DropLocation hover(QPoint globalPos) override;
	QPoint posForIndicator(KDDockWidgets::DropLocation loc) const override;
	void updatePositions() override;
	void raise() override;
	void setVisible(bool visible) override;
	void resize(QSize size) override;
	void setGeometry(QRect rect) override;
	bool isWindow() const override;
	void updateIndicatorVisibility() override;

protected:
	void resizeEvent(QResizeEvent* ev) override;

private:
	KDDockWidgets::Core::ClassicDropIndicatorOverlay* m_classic_indicators;
	std::vector<DockDropIndicator*> m_indicators;
};

class DockDropIndicator : public QWidget
{
	Q_OBJECT

public:
	DockDropIndicator(KDDockWidgets::DropLocation loc, QWidget* parent = nullptr);

	KDDockWidgets::DropLocation location;
	bool hovered = false;

protected:
	void paintEvent(QPaintEvent* event) override;
};

class DockSegmentedDropIndicatorOverlay : public KDDockWidgets::QtWidgets::SegmentedDropIndicatorOverlay
{
	Q_OBJECT

public:
	DockSegmentedDropIndicatorOverlay(
		KDDockWidgets::Core::SegmentedDropIndicatorOverlay* controller, QWidget* parent = nullptr);

	static std::string s_indicator_style;

protected:
	void paintEvent(QPaintEvent* event) override;

private:
	void drawSegmented();
	void drawMinimalistic();
};
