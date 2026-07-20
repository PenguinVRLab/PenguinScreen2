// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "VRSettingsWidget.h"
#include "SettingWidgetBinder.h"
#include "SettingsWindow.h"
#include "QtHost.h"

#include "pcsx2/Host.h"
#include "pcsx2/VMManager.h"
#include "pcsx2/VR/VRProfileDB.h"

VRSettingsWidget::VRSettingsWidget(SettingsWindow* settings_dialog, QWidget* parent)
	: SettingsWidget(settings_dialog, parent)
{
	SettingsInterface* sif = dialog()->getSettingsInterface();

	setupTab(m_ui, tr("Virtual Reality"));

	//////////////////////////////////////////////////////////////////////////
	// VR Settings
	//////////////////////////////////////////////////////////////////////////
	SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.enable, "VR", "Enable", false);
	SettingWidgetBinder::BindWidgetToFloatSetting(sif, m_ui.screenDistance, "VR", "ScreenDistance", 2.0f);
	SettingWidgetBinder::BindWidgetToFloatSetting(sif, m_ui.screenHeight, "VR", "ScreenHeight", 1.4f);
	SettingWidgetBinder::BindWidgetToFloatSetting(sif, m_ui.screenArc, "VR", "ScreenArcDeg", 0.0f);
	SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.stereoMode, "VR", "StereoMode", false);
	SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.stereoUseProfile, "VR", "StereoUseProfile", true);
	SettingWidgetBinder::BindWidgetToFloatSetting(sif, m_ui.stereoSeparation, "VR", "StereoSeparation", 0.02f);
	SettingWidgetBinder::BindWidgetToFloatSetting(sif, m_ui.stereoConvergence, "VR", "StereoConvergence", 20.0f);
	SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.headCamera, "VR", "HeadCamera", false);

	dialog()->registerWidgetHelp(m_ui.enable, tr("Enable VR (OpenXR)"), tr("Unchecked"),
		tr("Renders the emulated display on a virtual screen inside a connected OpenXR headset. Requires the Vulkan "
		   "renderer and a running OpenXR runtime. Takes effect when the emulator restarts."));

	dialog()->registerWidgetHelp(m_ui.screenDistance, tr("Screen Distance"), tr("2.0 m"),
		tr("Sets how far in front of you the virtual screen is placed, in metres."));

	dialog()->registerWidgetHelp(m_ui.screenHeight, tr("Screen Height"), tr("1.4 m"),
		tr("Sets the physical height of the virtual screen, in metres. The width follows from the game's aspect ratio."));

	dialog()->registerWidgetHelp(m_ui.screenArc, tr("Screen Curve"), tr("0°"),
		tr("Curves the virtual screen into a cylinder section wrapping around you, in degrees of arc. 0 keeps the screen "
		   "flat. The arc governs the screen's width (height follows the aspect ratio); Screen Distance becomes the "
		   "cylinder radius. Requires runtime support (falls back to the flat screen otherwise). Takes effect immediately."));

	dialog()->registerWidgetHelp(m_ui.stereoMode, tr("Enable Stereoscopic 3D"), tr("Unchecked"),
		tr("Renders the game with per-eye depth on the virtual screen, like a 3D movie. The right values for Separation "
		   "and Convergence vary per game; wrong values cause eye strain or double vision. Takes effect immediately."));

	dialog()->registerWidgetHelp(m_ui.stereoUseProfile, tr("Use Per-Game Profile When Available"), tr("Checked"),
		tr("Applies the separation/convergence from the bundled per-game VR profile when one exists for the running game, "
		   "ignoring the values below. Uncheck to tune the values below live."));

	dialog()->registerWidgetHelp(m_ui.stereoSeparation, tr("Separation"), tr("0.02"),
		tr("How far apart the two eyes' views are. Larger values deepen the 3D effect but increase eye strain."));

	dialog()->registerWidgetHelp(m_ui.stereoConvergence, tr("Convergence"), tr("20.00"),
		tr("The depth at which objects appear AT the screen. Objects nearer pop out; objects farther sink in. The useful "
		   "range varies enormously between games (from below 1 to over 100)."));

	dialog()->registerWidgetHelp(m_ui.headCamera, tr("Head-Tracked Camera"), tr("Unchecked"),
		tr("Drives the game's own camera with your head movement, by writing the head pose into the game's memory each "
		   "frame. Requires a per-game immersive profile with verified camera addresses; does nothing for games without "
		   "one. Takes effect immediately."));

	dialog()->registerWidgetHelp(m_ui.profileBrowser, tr("Per-Game Profiles"), tr("N/A"),
		tr("Browses the profiles bundled in vr-profiles.yaml. Profiles apply automatically to the matching game — this "
		   "list is informational, showing each game's tuned values and capabilities."));

	populateProfileBrowser();
	connect(m_ui.profileBrowser, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
		&VRSettingsWidget::updateProfileDetails);

	// Separation/convergence only take effect while the per-game profile is NOT
	// in charge — gray them out whenever the profile checkbox is on, and show
	// the values ACTUALLY in effect inside the grayed boxes (the running game's
	// profile values when one matched, the config values otherwise).
	updateTuningFields();
	connect(m_ui.stereoUseProfile, &QCheckBox::toggled, this, [this](bool) { updateTuningFields(); });
}

void VRSettingsWidget::updateTuningFields()
{
	const bool use_profile = m_ui.stereoUseProfile->isChecked();
	const bool profile_in_charge = use_profile && m_running_profile_stereo.has_value();

	m_ui.stereoSeparation->setEnabled(!use_profile);
	m_ui.stereoConvergence->setEnabled(!use_profile);
	m_ui.stereoSeparationLabel->setEnabled(!use_profile);
	m_ui.stereoConvergenceLabel->setEnabled(!use_profile);

	// The spinboxes are two-way bound to the config: block their signals while
	// repainting them so a display-only update never writes back to the INI.
	const QSignalBlocker sep_blocker(m_ui.stereoSeparation);
	const QSignalBlocker conv_blocker(m_ui.stereoConvergence);
	if (profile_in_charge)
	{
		m_ui.stereoSeparation->setValue(m_running_profile_stereo->first);
		m_ui.stereoConvergence->setValue(m_running_profile_stereo->second);
	}
	else
	{
		m_ui.stereoSeparation->setValue(
			dialog()->getEffectiveFloatValue("VR", "StereoSeparation", 0.02f));
		m_ui.stereoConvergence->setValue(
			dialog()->getEffectiveFloatValue("VR", "StereoConvergence", 20.0f));
	}
}

void VRSettingsWidget::populateProfileBrowser()
{
	// The DB is load-once and immutable afterwards; safe to browse from the UI
	// thread (see VRProfileDB.h threading notes).
	m_profiles = VR::ProfileDB::ListProfiles();

	QSignalBlocker blocker(m_ui.profileBrowser);
	m_ui.profileBrowser->clear();

	const std::string running_serial = VMManager::GetDiscSerial();
	int running_index = -1;
	for (const VR::ProfileDB::Summary& p : m_profiles)
	{
		QStringList caps;
		if (p.has_stereo)
			caps << tr("stereo");
		if (p.has_camera)
			caps << tr("head camera");
		const QString label = QStringLiteral("%1 — %2 [%3]")
								  .arg(QString::fromStdString(p.serial),
									  p.name.empty() ? tr("(unnamed)") : QString::fromStdString(p.name),
									  caps.isEmpty() ? tr("none") : caps.join(QStringLiteral(", ")));
		m_ui.profileBrowser->addItem(label);
		if (!running_serial.empty() &&
			QString::fromStdString(p.serial).compare(QString::fromStdString(running_serial), Qt::CaseInsensitive) == 0)
		{
			running_index = m_ui.profileBrowser->count() - 1;
		}
	}

	if (m_ui.profileBrowser->count() == 0)
	{
		m_ui.profileBrowser->addItem(tr("(no profiles found)"));
		m_ui.profileBrowser->setEnabled(false);
	}

	if (running_serial.empty())
	{
		m_ui.profileStatus->setText(tr("No game is running."));
	}
	else if (running_index >= 0)
	{
		m_ui.profileBrowser->setCurrentIndex(running_index);
		const VR::ProfileDB::Summary& match = m_profiles[static_cast<size_t>(running_index)];
		if (match.has_stereo)
			m_running_profile_stereo = std::make_pair(match.separation, match.convergence);
		m_ui.profileStatus->setText(
			tr("Running game %1: profile FOUND — it applies automatically while \"Use Per-Game Profile\" is checked.")
				.arg(QString::fromStdString(running_serial)));
	}
	else
	{
		m_ui.profileStatus->setText(
			tr("Running game %1: no bundled profile — the Separation/Convergence values above apply.")
				.arg(QString::fromStdString(running_serial)));
	}

	updateProfileDetails(m_ui.profileBrowser->currentIndex());
	updateTuningFields();
}

void VRSettingsWidget::updateProfileDetails(int index)
{
	if (index < 0 || static_cast<size_t>(index) >= m_profiles.size())
	{
		m_ui.profileDetails->setText(QStringLiteral("-"));
		return;
	}

	const VR::ProfileDB::Summary& p = m_profiles[static_cast<size_t>(index)];
	QString text;
	if (p.has_stereo)
		text = tr("Stereo: separation %1, convergence %2.").arg(p.separation, 0, 'f', 3).arg(p.convergence, 0, 'f', 2);
	else
		text = tr("No stereo tuning in this profile.");
	if (p.has_camera)
		text += tr(" Head-tracked camera: available.");
	m_ui.profileDetails->setText(text);
}

VRSettingsWidget::~VRSettingsWidget() = default;

#include "moc_VRSettingsWidget.cpp"
