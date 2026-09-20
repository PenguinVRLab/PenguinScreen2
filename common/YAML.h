// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "Error.h"

#include "ryml.hpp"
#include "ryml_std.hpp"

#include <optional>

std::optional<ryml::Tree> ParseYAMLFromString(ryml::csubstr yaml, ryml::csubstr file_name, Error* error, bool resolve_anchors = false);
