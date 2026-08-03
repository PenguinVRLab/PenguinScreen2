// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#ifdef _MSC_VER
#	pragma warning(disable:4250)
#endif

#include "common/Pcsx2Defs.h"
#include "common/VectorIntrin.h"

#include <algorithm>
#include <cinttypes>
#include <condition_variable>
#include <climits>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <list>
#include <memory>
#include <mutex>
#include <functional>
#include <optional>
#include <stack>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <stddef.h>
#include <sys/stat.h>

#if !defined(__GNUC__) || defined(__clang__)
#include "fmt/format.h"
#endif
