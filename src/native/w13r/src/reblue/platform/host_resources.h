/**
 * @file    platform/host_resources.h
 * @brief   Per-process descriptor and mapping usage against the host ceilings.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once

#include <string>

namespace bd::platform {

// Usage against the per-process ceilings POSIX hits long before Windows would:
// RLIMIT_NOFILE and, on Linux only, vm.max_map_count, reported as -1 elsewhere.
// Linux reads /proc, macOS walks /dev/fd and the task's Mach VM regions. Not
// free, so call it on failure paths only.
std::string ResourceUse();

// Raises RLIMIT_NOFILE's soft limit to the hard limit.
void RaiseFDLimit();

} // namespace bd::platform
