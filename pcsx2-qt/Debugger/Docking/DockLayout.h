// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "Debugger/Docking/DockTables.h"

#include "DebugTools/DebugInterface.h"

#include <kddockwidgets/MainWindow.h>
#include <kddockwidgets/DockWidget.h>

#include <QtCore/QPointer>

class DebuggerView;
class DebuggerWindow;

extern const char* DEBUGGER_LAYOUT_FILE_FORMAT;

extern const u32 DEBUGGER_LAYOUT_FILE_VERSION_MAJOR;

extern const u32 DEBUGGER_LAYOUT_FILE_VERSION_MINOR;

class DockLayout
{
public:
	using Index = size_t;
	static const constexpr Index INVALID_INDEX = SIZE_MAX;

	enum LoadResult
	{
		SUCCESS,
		FILE_NOT_FOUND,
		INVALID_FORMAT,
		MAJOR_VERSION_MISMATCH,
		DEFAULT_LAYOUT_HASH_MISMATCH,
		CONFLICTING_NAME
	};

	DockLayout(
		QString name,
		BreakPointCpu cpu,
		bool is_default,
		const std::string& base_name,
		DockLayout::Index index);

	DockLayout(
		QString name,
		BreakPointCpu cpu,
		bool is_default,
		DockLayout::Index index);

	DockLayout(
		QString name,
		BreakPointCpu cpu,
		bool is_default,
		const DockLayout& layout_to_clone,
		DockLayout::Index index);

	DockLayout(
		const std::string& path,
		LoadResult& result,
		DockLayout::Index& index_last_session,
		DockLayout::Index index);

	~DockLayout();

	DockLayout(const DockLayout& rhs) = delete;
	DockLayout& operator=(const DockLayout& rhs) = delete;

	DockLayout(DockLayout&& rhs) = default;
	DockLayout& operator=(DockLayout&&) = default;

	const QString& name() const;
	void setName(QString name);

	BreakPointCpu cpu() const;
	void setCpu(BreakPointCpu cpu);

	bool isDefault() const;

	void freeze();

	void thaw();

	bool canReset();
	void reset();

	KDDockWidgets::Core::DockWidget* createDockWidget(const QString& name);
	void updateDockWidgetTitles();

	const std::map<QString, QPointer<DebuggerView>>& debuggerViews();
	bool hasDebuggerView(const QString& unique_name);
	size_t countDebuggerViewsOfType(const char* type);
	void createDebuggerView(const std::string& type);
	void recreateDebuggerView(const QString& unique_name);
	void destroyDebuggerView(const QString& unique_name);
	void setPrimaryDebuggerView(DebuggerView* widget, bool is_primary);

	void deleteFile();

	bool save(DockLayout::Index layout_index);

private:
	void load(
		const std::string& path,
		DockLayout::LoadResult& result,
		DockLayout::Index& index_last_session);

	void validatePrimaryDebuggerViews();

	void setupDefaultLayout();

	std::pair<QString, u64> generateNewUniqueName(const char* type);

	QString m_name;

	BreakPointCpu m_cpu;

	bool m_is_default = false;

	u64 m_next_id = 0;

	std::string m_base_layout;

	QByteArray m_toolbars;

	std::map<QString, QPointer<DebuggerView>> m_widgets;

	QByteArray m_geometry;

	std::string m_layout_file_path;

	bool m_is_active = false;
};
