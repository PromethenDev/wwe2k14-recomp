// Small host-testable wait primitive for runtime shader jobs already in flight.
#pragma once

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <condition_variable>
#include <cstdlib>
#include <cstdint>
#include <limits>
#include <mutex>
#include <thread>

namespace nr::shared::rts_wait {

enum class Result { kReady, kTimedOut, kWouldDeadlock };

inline uint32_t ParseLimitMs(const char* text) {
  if (!text || !*text || *text == '-') return 250;
  char* end = nullptr;
  errno = 0;
  const unsigned long parsed = std::strtoul(text, &end, 10);
  if (errno || end == text || *end) return 250;
  return uint32_t(std::min<unsigned long>(parsed, std::numeric_limits<uint32_t>::max()));
}

// Caller holds lock; ready() must inspect state protected by that same lock.
template <typename Ready>
Result WaitUntil(std::condition_variable& cv, std::unique_lock<std::mutex>& lock, Ready ready,
                 std::chrono::milliseconds timeout, std::thread::id caller, std::thread::id translator) {
  if (ready()) return Result::kReady;
  if (caller == translator) return Result::kWouldDeadlock;
  return cv.wait_for(lock, timeout, ready) ? Result::kReady : Result::kTimedOut;
}

}  // namespace nr::shared::rts_wait
