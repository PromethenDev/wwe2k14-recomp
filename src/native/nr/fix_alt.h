// Pure swap-boundary phase scheduler for WWE2K14_NR_FIX_ALT.
#pragma once

#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdlib>

namespace nr::backend::fix_alt {

inline int ParseSeconds(const char* value) {
  if (!value || !*value) return 0;
  errno = 0;
  char* end = nullptr;
  const long seconds = std::strtol(value, &end, 10);
  if (errno == ERANGE || end == value || *end != '\0' || seconds <= 0 || seconds > INT_MAX) return 0;
  return int(seconds);
}

class Scheduler {
 public:
  explicit Scheduler(int period_seconds = 0) : period_seconds_(period_seconds > 0 ? period_seconds : 0) {}

  int period_seconds() const { return period_seconds_; }
  bool enabled() const { return period_seconds_ > 0; }
  bool phase_b() const { return phase_b_; }

  // Returns -1 before the first transition or when disabled; otherwise returns the new phase (0=A, 1=B).
  int Tick(uint64_t now_ns) {
    if (!enabled()) return -1;
    if (!started_) {
      started_ = true;
      start_ns_ = now_ns;
      return -1;
    }
    const uint64_t period_ns = uint64_t(period_seconds_) * 1000000000ull;
    const uint64_t elapsed_ns = now_ns >= start_ns_ ? now_ns - start_ns_ : 0;
    const bool want_b = ((elapsed_ns / period_ns) & 1u) != 0;
    if (want_b == phase_b_) return -1;
    phase_b_ = want_b;
    return phase_b_ ? 1 : 0;
  }

 private:
  int period_seconds_ = 0;
  bool started_ = false;
  bool phase_b_ = false;
  uint64_t start_ns_ = 0;
};

}  // namespace nr::backend::fix_alt
