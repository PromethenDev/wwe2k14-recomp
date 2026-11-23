/**
 * @file  gpu/deferred_destroy.h
 * @brief Post-fence guest resource destruction queue.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once

#include <mutex>
#include <rex/types.h>
#include <unordered_set>
#include <vector>

namespace bd::gpu {

enum class ResourceType : u32; // defined with the guest resource types

// Guest resources are torn down only after the frame fence signals: a resource
// still referenced by the open command list must outlive that list's GPU
// execution, or the driver AVs inside the next OMSetRenderTargets. Dedup guards
// the double-name case (Release refcount 1->0 plus an explicit Destroy).
// Self-synchronized: Queue on the guest thread, Take from Present.
class DeferredDestroyQueue {
public:
  struct Entry {
    u32 guest_va;
    ResourceType type;
  };

  void Queue(u32 guest_va, ResourceType type) {
    if (!guest_va)
      return;
    std::lock_guard lock(mutex_);
    if (!queued_.insert(guest_va).second)
      return; // already pending
    pending_.push_back({guest_va, type});
  }

  // Caller runs teardown with no lock held: teardown re-enters renderer state.
  std::vector<Entry> Take() {
    std::lock_guard lock(mutex_);
    std::vector<Entry> out;
    out.swap(pending_);
    queued_.clear();
    return out;
  }

private:
  std::mutex mutex_;
  std::vector<Entry> pending_;
  std::unordered_set<u32> queued_;
};

} // namespace bd::gpu
