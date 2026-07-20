// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "ui_VRSettingsWidget.h"

#include "SettingsWidget.h"

#include "pcsx2/VR/VRProfileDB.h"

#include <optional>
#include <utility>
#include <vector>

class VRSettingsWidget : public SettingsWidget
{
	Q_OBJECT

public:
	VRSettingsWidget(SettingsWindow* settings_dialog, QWidget* parent);
	~VRSettingsWidget();

private:
	void populateProfileBrowser();
	void updateProfileDetails(int index);
	void updateTuningFields();

	Ui::VRSettingsWidget m_ui;
	std::vector<VR::ProfileDB::Summary> m_profiles;

	// The RUNNING game's matched profile stereo values (separation, convergence),
	// shown read-only in the grayed spinboxes while the profile is in charge.
	std::optional<std::pair<float, float>> m_running_profile_stereo;
};
