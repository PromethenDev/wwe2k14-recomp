/**
 * @file    core/shutdown.h
 * @brief   The one ordered exit path: quiesce the guest, drain the GPU, flush,
 *          then kill the process.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once

#include <functional>

namespace bd {

enum class ShutdownReason {
  WindowClose, // window X / alt-F4
  GuestExit,   // title-menu Exit
  Fatal,       // device lost, unrecoverable file error
  InitFailure, // startup never reached the guest
};

// Installed by ReblueApp: runs 'fn' on the UI thread, inline if the caller is
// already there. The sequence must not run on a guest thread, since
// TerminateTitle self-terminates its caller, so guest thread requests post
// through this and park until the process dies. False means the function was
// dropped because the UI loop already exited, and the caller kills the process
// outright. With no dispatcher (pre-UI failures) the sequence runs inline.
void SetShutdownDispatcher(std::function<bool(std::function<void()>)> dispatch);

// Installed alongside the dispatcher, and run while waiting for the renderer to
// go idle: a guest thread parked in Present's overlay marshal can then finish
// its frame instead of holding the renderer lock until the drain gives up.
void SetShutdownUIPump(std::function<void()> pump);

// The first caller wins, later ones park or return. Never returns
// for the winner (ends in TerminateProcessNow) nor for a guest thread caller.
void RequestShutdown(ShutdownReason reason, int exit_code = 0);

// Checked by hooks that would start new work, or block on the UI thread while
// it is busy running the sequence.
bool IsShuttingDown();

// Stages 1, 2 and 4 without the final exit: quiesce the renderer, cooperatively
// stop guest threads, drain and release the swap chain. Warm reboot runs this,
// then spawns and exits itself.
void QuiesceForExit();

} // namespace bd
