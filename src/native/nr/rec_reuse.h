// Pure decision helper for WWE2K14_NR_REC_REUSE's per-command-list descriptor bind cache.
#pragma once

#include <cstdint>

namespace nr::backend::rec_reuse {

struct ProbeResult {
  bool hit = false;
  bool mismatch = false;
};

constexpr ProbeResult Probe(bool cache_valid, uintptr_t cached, bool check_valid, uintptr_t check_cached,
                            uintptr_t wanted) noexcept {
  const bool hit = cache_valid && cached == wanted;
  const bool check_hit = check_valid && check_cached == wanted;
  return {hit, hit != check_hit};
}

}  // namespace nr::backend::rec_reuse
