// H1e (docs/tasks/H1e-design.md): pure decision of the Move-Set hang fix. The optional cache copy (sub_82C58578) must not wait on
// its own worker: try the allocation; while it fails, evict; stop when nothing can be evicted. Ops are the game's calls.
#pragma once
#include <cstdint>

namespace wwe2k14::h1e {
template <class Ops>
uint32_t TryAllocNoWait(Ops& ops, uint32_t size, uint32_t align) {
  for (;;) {
    const uint32_t p = ops.Alloc(size, align);
    if (p) return p;
    if (ops.EvictableCount() <= 0) return 0;  // the only place the original waits on its own End event
    if (ops.EvictOne() == 0) return 0;        // the original would spin here
  }
}
constexpr uint32_t CopySize(uint32_t req_word44) { return (req_word44 << 8) & 0xFFFFFF00u; }
constexpr uint32_t kCopyAlign = 64;
constexpr uint32_t kLogMinBytes = 8u * 1024u * 1024u;
}  // namespace wwe2k14::h1e
