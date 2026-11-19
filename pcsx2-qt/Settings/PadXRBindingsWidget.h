// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "SettingsWidget.h"

#include "pcsx2/VR/PadXRBindings.h"

#include <vector>

class QComboBox;
class QGridLayout;
class QLabel;

class PadXRBindingsWidget : public SettingsWidget
{
	Q_OBJECT

public:
	PadXRBindingsWidget(SettingsWindow* settings_dialog, QWidget* parent);
	~PadXRBindingsWidget();

private:
	struct Row
	{
		int action;
		int hand;
		QComboBox* first;
		QComboBox* second;
	};

	const char* currentProfile() const;
	void loadFromSettings();
	void saveToSettings();
	void rebuildRows();
	void onRowChanged(const Row& row);
	void resetToDefaults();

	VR::PadXRBindings::Model m_model;
	QWidget* m_contents = nullptr;
	QComboBox* m_profile = nullptr;
	QGridLayout* m_grid = nullptr;
	QLabel* m_status = nullptr;
	std::vector<Row> m_rows;
	bool m_updating = false;
};
