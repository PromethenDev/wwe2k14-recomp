/**
 * @file    core/time_util.h
 * @brief   Cross-platform broken-down local time (localtime_s / localtime_r).
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once

#include <ctime>

namespace bd {

// Win32 has no localtime_r, POSIX has no localtime_s.
inline std::tm LocalTime(std::time_t t) {
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  return tm;
}

} // namespace bd
