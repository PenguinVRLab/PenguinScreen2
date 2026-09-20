// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <QtWidgets/QInputDialog>
#include <QtWidgets/QMessageBox>

#include <functional>

namespace AsyncDialogs
{
	void getText(
		QWidget* parent,
		const QString& title,
		const QString& label,
		const QString& text,
		std::function<void(QString)> value_callback);

	void getText(
		QWidget* parent,
		const QString& title,
		const QString& label,
		QLineEdit::EchoMode echo,
		const QString& text,
		Qt::WindowFlags flags,
		Qt::InputMethodHints input_method_hints,
		std::function<void(std::optional<QString>)> callback);

	void getMultiLineText(
		QWidget* parent,
		const QString& title,
		const QString& label,
		const QString& text,
		std::function<void(QString)> value_callback);

	void getMultiLineText(
		QWidget* parent,
		const QString& title,
		const QString& label,
		const QString& text,
		Qt::WindowFlags flags,
		Qt::InputMethodHints input_method_hints,
		std::function<void(std::optional<QString>)> callback);

	void getItem(
		QWidget* parent,
		const QString& title,
		const QString& label,
		const QStringList& items,
		int current,
		std::function<void(QString)> value_callback);

	void getItem(
		QWidget* parent,
		const QString& title,
		const QString& label,
		const QStringList& items,
		int current,
		bool editable,
		Qt::WindowFlags flags,
		Qt::InputMethodHints input_method_hints,
		std::function<void(std::optional<QString>)> callback);

	void getInt(
		QWidget* parent,
		const QString& title,
		const QString& label,
		int value,
		std::function<void(int)> value_callback);

	void getInt(
		QWidget* parent,
		const QString& title,
		const QString& label,
		int value,
		int min_value,
		int max_value,
		int step,
		Qt::WindowFlags flags,
		std::function<void(std::optional<int>)> callback);

	void getDouble(
		QWidget* parent,
		const QString& title,
		const QString& label,
		double value,
		std::function<void(double)> value_callback);

	void getDouble(
		QWidget* parent,
		const QString& title,
		const QString& label,
		double value,
		double min_value,
		double max_value,
		int decimals,
		Qt::WindowFlags flags,
		double step,
		std::function<void(std::optional<double>)> callback);

	void information(
		QWidget* parent,
		const QString& title,
		const QString& text,
		std::function<void(QMessageBox::StandardButton)> callback = {});

	void information(
		QWidget* parent,
		const QString& title,
		const QString& text,
		QMessageBox::StandardButtons buttons,
		QMessageBox::StandardButton default_button,
		std::function<void(QMessageBox::StandardButton)> callback = {});

	void question(
		QWidget* parent,
		const QString& title,
		const QString& text,
		std::function<void()> yes_callback);

	void question(
		QWidget* parent,
		const QString& title,
		const QString& text,
		QMessageBox::StandardButtons buttons,
		QMessageBox::StandardButton default_button,
		std::function<void(QMessageBox::StandardButton)> callback);

	void warning(
		QWidget* parent,
		const QString& title,
		const QString& text,
		std::function<void(QMessageBox::StandardButton)> callback = {});

	void warning(
		QWidget* parent,
		const QString& title,
		const QString& text,
		QMessageBox::StandardButtons buttons,
		QMessageBox::StandardButton default_button,
		std::function<void(QMessageBox::StandardButton)> callback = {});

	void critical(
		QWidget* parent,
		const QString& title,
		const QString& text,
		std::function<void(QMessageBox::StandardButton)> callback = {});

	void critical(
		QWidget* parent,
		const QString& title,
		const QString& text,
		QMessageBox::StandardButtons buttons,
		QMessageBox::StandardButton default_button,
		std::function<void(QMessageBox::StandardButton)> callback = {});
}
