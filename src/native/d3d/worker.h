// WP9 (decision 0006 stage 2): worker thread consuming capture events and dispatching to WP6/7/8/10.
// Frozen interface. The worker thread is the consumer thread of an nr::RecordQueue (nr_record_queue.h) owned by
// capture.cpp; EventReader is the pull view of the event the queue is currently handing to the worker.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>

#include "shadow_types.h"

namespace wwe2k14::d3d {

class EventReader {
 public:
  // Worker thread. Next published event: header + payload (valid until Done(seq)). false = none available
  // (the event the queue is handing over is returned once).
  bool Next(EvHeader* h, const uint8_t** payload);
  // Marks every event up to and including `seq` as consumed (advances worker_done_seq, releases the CP gate; the
  // queue slot itself is released when the worker's callback returns).
  void Done(uint32_t seq);
};

// Backend access lock (S2-D): held by the worker per batch of <= 32 events, by nr around its observer callbacks.
// Card S3-45 (WWE2K14_D3D_FAIR_LOCK=1): FIFO ticket lock instead of std::mutex - in heavy entrance frames the worker
// released and immediately re-took the (unfair) mutex after each batch while the CP thread waited (lane A s375: CP
// thread 21 % of samples in the lock), delaying presents. Off: a plain std::mutex as before.
class BackendMutex {
 public:
  void lock();
  void unlock();
  bool try_lock();
 private:
  std::mutex m_;
  std::atomic<uint32_t> next_{0}, serving_{0};
 public:
  // Stutter work 2026-10-17 (WWE2K14_D3D_LOCK_HANDOFF=1): threads other than the D3D worker (CP present / interrupts /
  // guest-memory-write callbacks on the game thread) waiting in lock(); the worker steps aside between events for them.
  std::atomic<uint32_t> other_waiters_{0};
};
BackendMutex& BackendLock();

// ---- WP9 internal ----
struct WorkerStats {
  std::atomic<uint64_t> events{0}, busy_ns{0}, lock_wait_ns{0}, cpu_ns{0};
  std::atomic<uint64_t> handoffs{0};  // WWE2K14_D3D_LOCK_HANDOFF: times the worker released the lock for a waiter
  std::atomic<uint64_t> draws_translated{0}, draws_skipped{0}, draws_submitted{0}, submit_fail{0};
  // Card M2c2: mode-2 draws with a refused texture (kNone binding). The backend rejects such a draw as a whole ("bad
  // texture binding", reported only through TakeDrawFailures), so it is not submitted and counted here instead.
  std::atomic<uint64_t> tex_fail_draws{0};
  std::atomic<uint64_t> texh_checked{0}, texh_changed{0};  // card M2c9 (WWE2K14_D3D_SHADOW_TEXHASH)
  std::atomic<uint64_t> snap_checked{0}, snap_bad_records{0}, snap_bad_dwords{0}, vb_checked{0}, vb_bad{0};
  std::atomic<uint64_t> apply_fail{0}, batches{0};
  std::atomic<uint64_t> ns_apply{0}, ns_rv{0}, ns_snap{0}, ns_translate{0}, ns_other{0};  // worker split
  std::atomic<uint64_t> ns_kind[kEvKindCount]{}, n_kind[kEvKindCount]{};  // C1-T2: ns_other by event kind
  std::atomic<uint32_t> phase{0}, cur_packet{0};  // 1 apply 2 rv 3 snapcmp 4 translate 6 targets/resolve/present
  std::atomic<uint32_t> cur_seq{0}, cur_kind{0}, cur_emitter{0}, in_dispatch{0}, lock_held{0}, in_lock_wait{0};  // watchdog view
};
WorkerStats& WorkerStatsRef();
// Called by capture.cpp's queue callback on the worker thread. `more_pending` = further events are already queued
// (keeps BackendLock across the batch, S2-D).
void WorkerConsume(const EvHeader& h, const uint8_t* payload, bool more_pending);
// Releases the backend lock if the batch holds it (worker idle / shutdown).
void WorkerIdle();
// Summary text pieces (called from the report on the game thread; reads atomics only).
void WorkerSetDrain(bool drain);  // shutdown: consume events without dispatching

}  // namespace wwe2k14::d3d
