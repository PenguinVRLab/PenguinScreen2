// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <QtCore/QAbstractTableModel>
#include <QtWidgets/QHeaderView>

#include "DebugTools/DebugInterface.h"
#include "DebugTools/BiosDebugData.h"

#include <map>

class ThreadModel : public QAbstractTableModel
{
	Q_OBJECT

public:
	enum ThreadColumns : int
	{
		ID = 0,
		PC,
		ENTRY,
		PRIORITY,
		STATE,
		WAIT_TYPE,
		WAIT_ID,
		COLUMN_COUNT
	};

	static constexpr QHeaderView::ResizeMode HeaderResizeModes[ThreadColumns::COLUMN_COUNT] = {
		QHeaderView::ResizeMode::ResizeToContents,
		QHeaderView::ResizeMode::ResizeToContents,
		QHeaderView::ResizeMode::ResizeToContents,
		QHeaderView::ResizeMode::ResizeToContents,
		QHeaderView::ResizeMode::Stretch,
		QHeaderView::ResizeMode::Stretch,
		QHeaderView::ResizeMode::Stretch,
	};

	explicit ThreadModel(DebugInterface& cpu, QObject* parent = nullptr);

	int rowCount(const QModelIndex& parent = QModelIndex()) const override;
	int columnCount(const QModelIndex& parent = QModelIndex()) const override;
	QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
	QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

	void refreshData();

private:
	const std::map<ThreadStatus, QString> ThreadStateStrings{
		{ThreadStatus::THS_BAD, tr("BAD")},
		{ThreadStatus::THS_RUN, tr("RUN")},
		{ThreadStatus::THS_READY, tr("READY")},
		{ThreadStatus::THS_WAIT, tr("WAIT")},
		{ThreadStatus::THS_SUSPEND, tr("SUSPEND")},
		{ThreadStatus::THS_WAIT_SUSPEND, tr("WAIT SUSPEND")},
		{ThreadStatus::THS_DORMANT, tr("DORMANT")},
	};

	const std::map<WaitState, QString> ThreadWaitStrings{
		{WaitState::NONE, tr("NONE")},
		{WaitState::SEMA, tr("SEMAPHORE")},
		{WaitState::SLEEP, tr("SLEEP")},
		{WaitState::DELAY, tr("DELAY")},
		{WaitState::EVENTFLAG, tr("EVENTFLAG")},
		{WaitState::MBOX, tr("MBOX")},
		{WaitState::VPOOL, tr("VPOOL")},
		{WaitState::FIXPOOL, tr("FIXPOOL")},
	};

	DebugInterface& m_cpu;
	std::vector<std::unique_ptr<BiosThread>> m_threads;
};
