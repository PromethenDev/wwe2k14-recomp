/**
 * @file    core/windows_lean.h
 * @brief   Sole sanctioned entry point for <Windows.h>.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once

// This is the only file in the tree permitted to include <Windows.h> directly.
// Unguarded, it pollutes every translation unit that pulls it in: min/max
// macros shadow std::min/std::max, GetObject collides with unrelated
// GetObjectA/W-named APIs, and near/far revive long-dead memory model
// keywords. Route all Windows.h uses through this header instead.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef NOGDI
#define NOGDI
#endif

#include <Windows.h>
