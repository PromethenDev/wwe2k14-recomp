/**
 * @file    core/threading.h
 * @brief   Native threading, sleep, and frame timing hooks.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause - see LICENSE
 */
#pragma once

namespace bd {

void EnableHighResTimer();
void DisableHighResTimer();

// Best effort. Never dies hard.
void DemoteThreadToBackground();

// TerminateProcess and spin. Never returns, runs no destructors (not a clean
// exit).
[[noreturn]] void TerminateProcessNow(int exit_code = 0);

} // namespace bd
