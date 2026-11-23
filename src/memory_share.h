// Shared-memory safety net (owner 2026-11-17, after #154): see memory_share.cpp.
#pragma once

#include <cstdint>

#include <rex/ppc/context.h>

namespace wwe2k14::mem {
// >= 0: the guest's "calling thread's heap" query (sub_82C31100) answers this heap on this host thread only.
// Set only by a scoped retry (memory_share.cpp, pool_fail_log.cpp) around one nested allocator call.
extern thread_local int t_force_heap;

// Test-only diagnostics that also watch the block allocators register here instead of a second override
// (one strong symbol per guest function). nullptr = the generated body.
using GuestFn = void (*)(PPCContext&, uint8_t*);
extern GuestFn g_inner_new;    // sub_82C31518
extern GuestFn g_inner_small;  // sub_82C32DE8
}  // namespace wwe2k14::mem
