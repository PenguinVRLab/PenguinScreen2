// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0+

#include "PadXRBindingsWidget.h"
#include "QtHost.h"
#include "SettingsWindow.h"

#include "pcsx2/Host.h"
#include "pcsx2/VR/PadXRActions.h"

#include <QtWidgets/QComboBox>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>

namespace PA = VR::PadXRActions;
namespace PB = VR::PadXRBindings;

PadXRBindingsWidget::PadXRBindingsWidget(SettingsWindow* settings_dialog, QWidget* parent)
	: SettingsWidget(settings_dialog, parent)
{
	QWidget* contents = m_contents = new QWidget(this);
	QVBoxLayout* layout = new QVBoxLayout(contents);

	QLabel* intro = new QLabel(tr("PadXR's own controls in the headset: the menu button (hold it to recentre), menu "
								  "select, back, navigate and push to talk. They never reach the game. Changes apply the "
								  "next time a VR session starts."),
		contents);
	intro->setWordWrap(true);
	layout->addWidget(intro);

	QHBoxLayout* top = new QHBoxLayout();
	top->addWidget(new QLabel(tr("Controller:"), contents));
	m_profile = new QComboBox(contents);
	for (const PB::ProfileInfo& p : PB::Profiles())
		m_profile->addItem(QString::fromUtf8(p.title), QString::fromUtf8(p.path));
	top->addWidget(m_profile, 1);
	QPushButton* reset = new QPushButton(tr("Reset to Defaults"), contents);
	top->addWidget(reset);
	layout->addLayout(top);

	m_grid = new QGridLayout();
	layout->addLayout(m_grid);

	m_status = new QLabel(contents);
	m_status->setWordWrap(true);
	layout->addWidget(m_status);
	layout->addStretch(1);

	addTab(tr("PadXR Bindings"), contents);

	loadFromSettings();
	rebuildRows();

	connect(m_profile, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) { rebuildRows(); });
	connect(reset, &QPushButton::clicked, this, &PadXRBindingsWidget::resetToDefaults);
}

PadXRBindingsWidget::~PadXRBindingsWidget() = default;

const char* PadXRBindingsWidget::currentProfile() const
{
	const int i = m_profile->currentIndex();
	const auto& profiles = PB::Profiles();
	return (i >= 0 && i < static_cast<int>(profiles.size())) ? profiles[i].path : profiles.front().path;
}

void PadXRBindingsWidget::loadFromSettings()
{
	const PB::LoadReport report = m_model.Load([](const std::string& key) -> std::optional<std::string> {
		std::string v = Host::GetBaseStringSettingValue(PB::kSettingsSection, key.c_str(), "");
		if (v.empty())
			return std::nullopt;
		return v;
	});
	if (!report.rejected.empty())
		m_status->setText(tr("%n saved binding(s) were not valid and the default is used instead.", "",
			static_cast<int>(report.rejected.size())));
}

void PadXRBindingsWidget::saveToSettings()
{
	m_model.Save([](const std::string& key, const std::optional<std::string>& value) {
		if (value)
			Host::SetBaseStringSettingValue(PB::kSettingsSection, key.c_str(), value->c_str());
		else
			Host::RemoveBaseSettingValue(PB::kSettingsSection, key.c_str());
	});
	Host::CommitBaseSettingChanges();
	g_emu_thread->applySettings();
}

void PadXRBindingsWidget::rebuildRows()
{
	m_updating = true;
	while (QLayoutItem* item = m_grid->takeAt(0))
	{
		if (QWidget* w = item->widget())
		{
			w->hide();
			w->deleteLater();
		}
		delete item;
	}
	m_rows.clear();

	QWidget* owner = m_contents;
	m_grid->addWidget(new QLabel(tr("<b>Action</b>"), owner), 0, 0);
	m_grid->addWidget(new QLabel(tr("<b>Hand</b>"), owner), 0, 1);
	m_grid->addWidget(new QLabel(tr("<b>Input</b>"), owner), 0, 2);
	m_grid->addWidget(new QLabel(tr("<b>Also</b>"), owner), 0, 3);

	const char* profile = currentProfile();
	int r = 1;
	for (int a = 0; a < PA::ActionCount; a++)
	{
		for (int h = 0; h < 2; h++)
		{
			const std::vector<std::string> choices = m_model.Choices(profile, a, h);
			const std::vector<std::string>& current = m_model.Paths(profile, a, h);
			Row row{a, h, new QComboBox(owner), new QComboBox(owner)};
			for (QComboBox* box : {row.first, row.second})
			{
				box->addItem(tr("(none)"), QString());
				for (const std::string& c : choices)
					box->addItem(QString::fromStdString(c), QString::fromStdString(c));
				box->setEnabled(!choices.empty());
			}
			if (current.size() > 0)
				row.first->setCurrentIndex(std::max(0, row.first->findData(QString::fromStdString(current[0]))));
			if (current.size() > 1)
				row.second->setCurrentIndex(std::max(0, row.second->findData(QString::fromStdString(current[1]))));

			m_grid->addWidget(new QLabel(QString::fromUtf8(PA::Actions()[a].localized), owner), r, 0);
			m_grid->addWidget(new QLabel(h == 0 ? tr("Left") : tr("Right"), owner), r, 1);
			m_grid->addWidget(row.first, r, 2);
			m_grid->addWidget(row.second, r, 3);
			m_rows.push_back(row);
			const size_t index = m_rows.size() - 1;
			for (QComboBox* box : {row.first, row.second})
			{
				connect(box, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
					[this, index](int) { onRowChanged(m_rows[index]); });
			}
			r++;
		}
	}
	m_updating = false;
}

void PadXRBindingsWidget::onRowChanged(const Row& row)
{
	if (m_updating)
		return;
	std::vector<std::string> paths;
	for (QComboBox* box : {row.first, row.second})
	{
		const std::string p = box->currentData().toString().toStdString();
		if (!p.empty() && std::find(paths.begin(), paths.end(), p) == paths.end())
			paths.push_back(p);
	}
	std::string why;
	if (!m_model.SetPaths(currentProfile(), row.action, row.hand, paths, &why))
	{
		m_status->setText(QString::fromStdString(why));
		rebuildRows();
		return;
	}
	m_status->clear();
	saveToSettings();
}

void PadXRBindingsWidget::resetToDefaults()
{
	m_model.ResetToDefaults();
	saveToSettings();
	rebuildRows();
	m_status->setText(tr("Every PadXR binding is back to its default."));
}

#include "moc_PadXRBindingsWidget.cpp"
