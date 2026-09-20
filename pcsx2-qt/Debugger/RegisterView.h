// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "ui_RegisterView.h"

#include "DebuggerView.h"

#include "DebugTools/DebugInterface.h"
#include "DebugTools/DisassemblyManager.h"

#include <QtWidgets/QMenu>
#include <QtWidgets/QTabBar>
#include <QtGui/QPainter>

class RegisterView final : public DebuggerView
{
	Q_OBJECT

public:
	RegisterView(const DebuggerViewParameters& parameters);
	~RegisterView();

	void toJson(JsonValueWrapper& json) override;
	bool fromJson(const JsonValueWrapper& json) override;

protected:
	void paintEvent(QPaintEvent* event) override;
	void mousePressEvent(QMouseEvent* event) override;
	void mouseDoubleClickEvent(QMouseEvent* event) override;
	void wheelEvent(QWheelEvent* event) override;

public slots:
	void customMenuRequested(QPoint pos);
	void contextCopyValue();
	void contextCopyTop();
	void contextCopyBottom();
	void contextCopySegment();
	void contextChangeValue();
	void contextChangeTop();
	void contextChangeBottom();
	void contextChangeSegment();

	std::optional<DebuggerEvents::GoToAddress> contextCreateGotoEvent();

	void tabCurrentChanged(int cur);

private:
	Ui::RegisterView ui;

	void fetchNewValue(u64 currentValue, bool segment, std::function<void(u64)> callback);

	QPoint m_renderStart;

	s32 m_rowStart = 0;
	s32 m_rowEnd;
	s32 m_rowHeight;
	s32 m_fieldStartX[4];
	s32 m_fieldWidth;

	s32 m_selectedRow = 0;
	s32 m_selected128Field = 0;

	bool m_showVU0FFloat = false;
	bool m_showFPRFloat = false;
};
