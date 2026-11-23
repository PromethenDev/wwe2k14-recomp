// V19: pure real-byte RT-cap policy, host-testable without the renderer or Vulkan SDK.
#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

namespace wwe2k14::d3d::rt_real_cap {

struct Candidate {
  uint64_t token = 0;
  uint64_t last_use_ms = 0;
  uint64_t bytes = 0;
  bool bound = false;
  bool pending_read = false;
};

struct Plan {
  std::vector<uint64_t> victims;  // LRU order
  uint64_t after_bytes = 0;       // current bytes + incoming bytes - selected victims
  bool fits = false;              // incoming alone must fit; no bound/pending target is ever selected
};

// The renderer's high-resolution setting scales the shadow target cap by pixel area. Keep the
// 720p cap as a floor so lower scene scales do not force target churn.
inline uint64_t ScaleCapBytes(uint64_t base_bytes, uint32_t scale_milli) {
  if (scale_milli <= 1000) return base_bytes;
  const uint64_t scale = scale_milli;
  if (base_bytes > std::numeric_limits<uint64_t>::max() / scale) return std::numeric_limits<uint64_t>::max();
  const uint64_t once = base_bytes * scale;
  if (once > std::numeric_limits<uint64_t>::max() / scale) return std::numeric_limits<uint64_t>::max();
  return (once * scale) / 1000000ull;
}

inline bool Eligible(const Candidate& c, uint64_t now_ms, uint64_t min_idle_ms) {
  return c.bytes && !c.bound && !c.pending_read && now_ms >= c.last_use_ms &&
         now_ms - c.last_use_ms >= min_idle_ms;
}

inline Plan MakePlan(uint64_t current_bytes, uint64_t incoming_bytes, uint64_t cap_bytes, uint64_t now_ms,
                     uint64_t min_idle_ms, const std::vector<Candidate>& candidates) {
  Plan plan;
  const uint64_t max = std::numeric_limits<uint64_t>::max();
  plan.after_bytes = current_bytes > max - incoming_bytes ? max : current_bytes + incoming_bytes;
  if (incoming_bytes > cap_bytes) return plan;
  if (plan.after_bytes <= cap_bytes) {
    plan.fits = true;
    return plan;
  }

  // Match the existing cap's batch behavior: reclaim to 80%, but leave room for a single
  // target larger than that watermark. If all safe victims are exhausted, accept only <= cap.
  const uint64_t target = std::max(incoming_bytes, cap_bytes / 5 * 4);
  std::vector<const Candidate*> lru;
  lru.reserve(candidates.size());
  for (const Candidate& c : candidates)
    if (Eligible(c, now_ms, min_idle_ms)) lru.push_back(&c);
  std::sort(lru.begin(), lru.end(), [](const Candidate* a, const Candidate* b) {
    return a->last_use_ms != b->last_use_ms ? a->last_use_ms < b->last_use_ms : a->token < b->token;
  });
  for (const Candidate* c : lru) {
    if (plan.after_bytes <= target) break;
    plan.victims.push_back(c->token);
    plan.after_bytes = c->bytes >= plan.after_bytes ? 0 : plan.after_bytes - c->bytes;
  }
  plan.fits = plan.after_bytes <= cap_bytes;
  return plan;
}

// Retired allocations remain in current_bytes until their backend serial completes. A single
// batched wait can make the request fit only when those pending bytes cover the exact shortfall.
inline bool PendingRetireCanSatisfy(uint64_t current_bytes, uint64_t incoming_bytes, uint64_t cap_bytes,
                                    uint64_t pending_retire_bytes, bool wait_already_used = false) {
  if (wait_already_used) return false;
  if (incoming_bytes > cap_bytes) return false;
  const uint64_t current_limit = cap_bytes - incoming_bytes;
  if (current_bytes <= current_limit) return false;
  return pending_retire_bytes >= current_bytes - current_limit;
}

}  // namespace wwe2k14::d3d::rt_real_cap
