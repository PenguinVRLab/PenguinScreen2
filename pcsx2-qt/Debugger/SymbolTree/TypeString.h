// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <string_view>

#include <QtCore/QString>

#include <ccc/ast.h>

std::unique_ptr<ccc::ast::Node> stringToType(std::string_view string, const ccc::SymbolDatabase& database, QString& error_out);

QString typeToString(const ccc::ast::Node* type, const ccc::SymbolDatabase& database);
