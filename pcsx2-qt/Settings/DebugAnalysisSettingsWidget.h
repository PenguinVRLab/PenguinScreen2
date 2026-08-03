// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once
#include "ui_DebugAnalysisSettingsWidget.h"

#include "Config.h"

#include <QtGui/QStandardItemModel>

class SettingsWindow;

class DebugAnalysisSettingsWidget : public QWidget
{
	Q_OBJECT

public:
	DebugAnalysisSettingsWidget(QWidget* parent = nullptr);

	DebugAnalysisSettingsWidget(SettingsWindow* dialog, QWidget* parent = nullptr);

	void parseSettingsFromWidgets(Pcsx2Config::DebugAnalysisOptions& output);

protected:
	void setupSymbolSourceGrid();
	void saveSymbolSources();

	void setupSymbolFileList();
	void addSymbolFile();
	void removeSymbolFile();
	void saveSymbolFiles();

	void saveFunctionScanRange();

	void updateEnabledStates();

	std::string getStringSettingValue(const char* section, const char* key, const char* default_value = "");
	bool getBoolSettingValue(const char* section, const char* key, bool default_value = false);
	int getIntSettingValue(const char* section, const char* key, int default_value = 0);

	struct SymbolSourceTemp
	{
		QCheckBox* check_box = nullptr;
		bool previous_value = false;
		bool modified_by_user = false;
	};

	enum SymbolFileColumn
	{
		PATH_COLUMN = 0,
		BASE_ADDRESS_COLUMN = 1,
		CONDITION_COLUMN = 2,
		SYMBOL_FILE_COLUMN_COUNT = 3
	};

	SettingsWindow* m_dialog = nullptr;
	std::map<std::string, SymbolSourceTemp> m_symbol_sources;

	QStandardItemModel* m_symbol_file_model;

	Ui::DebugAnalysisSettingsWidget m_ui;
};
