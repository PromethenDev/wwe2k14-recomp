// Imported into WWE 2K14 native workspace (Track E, 2026-10-13) from
//   <wwe13-recomp-nr>/src/nr/nr_record_queue.h
//   repo HEAD 19c8bb361a9b7d6cba65afffc628bda023da2e0c (file last changed in 9f9b8a1),
//   original sha256 78b1a615cd62b0e66c8bf7f6c5cbc0e79624d4a2256dc411822a01744c6a3781.
// Local changes: WWE13_NR* renamed to WWE2K14_NR* (comments/env names only).
// WWE '13 native renderer - single-producer / single-consumer record queue (WWE2K14_NR_RECORD_THREAD=1).
//
// The renderer thread ("GPU Commands") parses PM4 into a plain description of each draw and a second thread
// records the Vulkan/D3D12 commands from it, so frame N's parse overlaps its own recording. Header-only and
// free of renderer types so tools/nr/tests/record-queue-test.cpp can stress it under ThreadSanitizer.
//
// Protocol (one producer thread P, one consumer thread W):
//   P: slot = Acquire(); fill *slot; Submit();          (Submit publishes slot with a release store of head_)
//   W: runs exec(*slot) for each published slot in order, then publishes done_ (release store)
//   P: Sync() returns once done_ == head_, i.e. every submitted slot has been executed, and everything W wrote
//      is visible to P (acquire load of done_).
// A slot belongs to P from Acquire() until Submit() and to W from Submit() until done_ passes it; Acquire()
// blocks while all N slots are in flight.
// Waiting is spin, then yield, then a condition variable (a spinning waiter would starve its partner on a
// single core, and Windows sleeps are coarse). W spins briefly when idle (draws arrive every few
// microseconds while P parses) and then sleeps on a condition variable. P publishes under the same mutex
// used by W's wait predicate and notifies after each publication.

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace nr {

inline void CpuRelax() {
#if defined(__x86_64__) || defined(__i386__)
  __builtin_ia32_pause();
#elif defined(__aarch64__)
  __asm__ __volatile__("yield");
#else
  std::this_thread::yield();
#endif
}

struct RecordQueueStats {
  uint64_t submitted = 0;
  uint64_t sync_calls = 0;       // Sync() calls that had to wait
  uint64_t sync_wait_ns = 0;     // producer time spent waiting in Sync()
  uint64_t full_waits = 0;       // Acquire() calls that found every slot in flight
  uint64_t full_wait_ns = 0;
  uint64_t worker_sleeps = 0;    // times the consumer went to sleep on the condition variable
  uint64_t worker_wakes = 0;     // producer notifications sent
};

struct RecordQueueWatchdogState {
  uint64_t published = 0;
  uint64_t committed = 0;
  uint64_t consumed = 0;
  uint64_t spurious_recoveries = 0;
  uint64_t executing = 0;
  uint32_t worker_state = 0;  // 0 idle/spinning, 1 executing callback, 2 asleep for work, 3 notifying a waiter
  bool producer_blocked = false;
  bool producer_sync_waiting = false;
};

template <class T, uint32_t N>
class RecordQueue {
  static_assert((N & (N - 1)) == 0, "N must be a power of two");

 public:
  explicit RecordQueue(std::function<void(T&)> exec) : exec_(std::move(exec)) {
    for (auto& s : slots_) s = std::make_unique<T>();
    worker_ = std::thread([this] { WorkerLoop(); });
  }
  ~RecordQueue() { Stop(); }
  RecordQueue(const RecordQueue&) = delete;
  RecordQueue& operator=(const RecordQueue&) = delete;

  // ---- producer side (one thread) ----
  T* Acquire() {
    if (write_head_ - done_.load(std::memory_order_acquire) >= Cap()) WaitForSpace();
    return slots_[write_head_ & (N - 1)].get();
  }
  // U9: exchange only an acquired, uncommitted slot with a private producer object.
  // Acquire's done_ acquire-load establishes that the consumer has stopped touching this unique_ptr.
  // Submit/SubmitBatch's existing head_ release-store publishes the replacement. No queue ordering changes.
  bool ExchangeAcquired(T* expected, std::unique_ptr<T>& owner) {
    if (!owner || !expected || write_head_ - done_.load(std::memory_order_acquire) >= Cap()) return false;
    auto& slot = slots_[write_head_ & (N - 1)];
    if (slot.get() != expected || slot.get() == owner.get()) return false;
    slot.swap(owner);
    return true;
  }
  void Submit() {
    ++write_head_;
    committed_.store(write_head_, std::memory_order_release);
    ++stats_.submitted;
    Publish();
  }
  // Batch mode: commit one preallocated entry and publish all committed entries with one release store when
  // `draw_threshold` draws have accumulated. jobs-only entries occupy the same sequence and ring slots, but do not
  // advance the draw threshold. Capacity forces a publication before the producer can overwrite an unconsumed slot.
  void SubmitBatch(bool is_draw, uint32_t draw_threshold) {
    ++write_head_;
    committed_.store(write_head_, std::memory_order_release);
    ++stats_.submitted;
    if (is_draw) ++pending_draws_;
    const uint64_t published = head_.load(std::memory_order_relaxed);
    if (pending_draws_ >= draw_threshold || write_head_ - published >= Cap()) Publish();
  }
  // Blocks until every submitted slot has been executed.
  void Sync() {
    Publish();
    WakeWorker();
    const uint64_t target = write_head_;
    if (done_.load(std::memory_order_acquire) >= target) return;
    const auto t0 = std::chrono::steady_clock::now();
    WaitDone(target);
    ++stats_.sync_calls;
    stats_.sync_wait_ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        std::chrono::steady_clock::now() - t0).count());
  }
  // Card S3-44: true while the producer waits for a free slot (the consumer must not wait on the producer then).
  bool ProducerBlocked() const { return producer_blocked_.load(std::memory_order_seq_cst); }
  bool Idle() const { return done_.load(std::memory_order_acquire) >= write_head_; }
  RecordQueueWatchdogState WatchdogState() const {
    return {head_.load(std::memory_order_acquire), committed_.load(std::memory_order_acquire),
            done_.load(std::memory_order_acquire), spurious_recoveries_.load(std::memory_order_relaxed),
            executing_.load(std::memory_order_relaxed),
            worker_state_.load(std::memory_order_relaxed), producer_blocked_.load(std::memory_order_relaxed),
            sync_waiting_.load(std::memory_order_relaxed)};
  }
  RecordQueueStats& stats() { return stats_; }  // producer-side fields; read after Sync()
  std::atomic<bool> producer_blocked_{false};
  std::atomic<uint32_t> cap_{N};
  uint64_t worker_sleeps() const { return sleeps_.load(std::memory_order_relaxed); }
  uint64_t spurious_recoveries() const { return spurious_recoveries_.load(std::memory_order_relaxed); }

  void Stop() {
    if (!worker_.joinable()) return;
    Sync();
    stop_.store(true, std::memory_order_seq_cst);
    {
      std::lock_guard<std::mutex> lk(m_);
      cv_work_.notify_all();
    }
    worker_.join();
  }
  std::thread::native_handle_type worker_native_handle() { return worker_.native_handle(); }
  // Night 11-18 (sync-why2: 60 ring_full waits/f): slots in flight may be limited below the storage N at run time
  // (WWE2K14_NR_RECORD_SLOTS, nr_backend.cpp). Indices stay modulo N, so any cap <= N never overwrites an
  // unconsumed slot; a lowered cap only makes the producer wait sooner.
  void SetCapacity(uint32_t c) { cap_.store(c < 2 ? 2u : c > N ? N : c, std::memory_order_relaxed); }
  uint32_t Cap() const { return cap_.load(std::memory_order_relaxed); }

 private:
  void WaitDone(uint64_t target) {
    for (int i = 0; i < 4000; ++i) {  // ~100 us of pause-spinning: a barrier normally waits for one draw
      if (done_.load(std::memory_order_acquire) >= target) return;
      CpuRelax();
    }
    for (int i = 0; i < 200; ++i) {
      if (done_.load(std::memory_order_acquire) >= target) return;
      std::this_thread::yield();
    }
    sync_waiting_.store(true, std::memory_order_seq_cst);
    std::unique_lock<std::mutex> lk(m_);
    cv_done_.wait(lk, [&] { return done_.load(std::memory_order_acquire) >= target; });
    sync_waiting_.store(false, std::memory_order_seq_cst);
  }
  void WaitForSpace() {
    // A lowered capacity (SetCapacity) can leave more unpublished entries than the new cap: publish them first, or
    // the consumer could never free the slots this wait needs (host test record-slots-test caught the deadlock).
    Publish();
    const auto t0 = std::chrono::steady_clock::now();
    const uint64_t h = write_head_;
    producer_blocked_.store(true, std::memory_order_seq_cst);
    WaitDone(h - Cap() + 1);
    producer_blocked_.store(false, std::memory_order_seq_cst);
    ++stats_.full_waits;
    stats_.full_wait_ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        std::chrono::steady_clock::now() - t0).count());
  }
  void WorkerLoop() {
    uint64_t next = 0;
    for (;;) {
      if (next < head_.load(std::memory_order_acquire)) {
        executing_.store(next, std::memory_order_relaxed);
        worker_state_.store(1, std::memory_order_release);
        exec_(*slots_[next & (N - 1)]);
        ++next;
        done_.store(next, std::memory_order_seq_cst);
        worker_state_.store(0, std::memory_order_release);
        if (sync_waiting_.load(std::memory_order_seq_cst)) {
          worker_state_.store(3, std::memory_order_release);
          std::lock_guard<std::mutex> lk(m_);
          cv_done_.notify_all();
          worker_state_.store(0, std::memory_order_release);
        }
        continue;
      }
      if (stop_.load(std::memory_order_acquire)) return;
      // Idle: spin ~60 us for the next draw, then sleep.
      bool got = false;
      const auto t0 = std::chrono::steady_clock::now();
      for (int i = 0;; ++i) {
        CpuRelax();
        if (next < head_.load(std::memory_order_acquire)) {
          got = true;
          break;
        }
        if ((i & 63) == 63 && std::chrono::steady_clock::now() - t0 > std::chrono::microseconds(60)) break;
       }
       if (got) continue;
       std::unique_lock<std::mutex> lk(m_);
       worker_state_.store(2, std::memory_order_release);
       sleeps_.fetch_add(1, std::memory_order_relaxed);
       const auto ready = [&] {
         return next < head_.load(std::memory_order_acquire) || stop_.load(std::memory_order_acquire);
       };
       bool timed_recovery = false;
       auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
       while (!ready()) {
         if (cv_work_.wait_until(lk, deadline) == std::cv_status::timeout) {
           if (ready()) {
             timed_recovery = next < head_.load(std::memory_order_acquire);
             break;
           }
           deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
         }
       }
       const bool stopping = stop_.load(std::memory_order_acquire);
       if (timed_recovery) {
         const uint64_t recoveries = spurious_recoveries_.fetch_add(1, std::memory_order_relaxed) + 1;
         std::fprintf(stderr,
                      "[nr-record-queue] spurious recovery count=%llu published=%llu consumed=%llu\n",
                      static_cast<unsigned long long>(recoveries),
                      static_cast<unsigned long long>(head_.load(std::memory_order_relaxed)),
                      static_cast<unsigned long long>(next));
       }
       worker_state_.store(0, std::memory_order_release);
       if (stopping) return;
    }
  }

  void Publish() {
    const uint64_t published = head_.load(std::memory_order_relaxed);
    if (published == write_head_) return;
    {
      std::lock_guard<std::mutex> lk(m_);
      head_.store(write_head_, std::memory_order_release);
      pending_draws_ = 0;
      cv_work_.notify_one();
      ++stats_.worker_wakes;
    }
  }

  // Sync must also wake the consumer when the queue head was published earlier.
  void WakeWorker() {
    std::lock_guard<std::mutex> lk(m_);
    cv_work_.notify_one();
    ++stats_.worker_wakes;
  }

  std::function<void(T&)> exec_;
  std::unique_ptr<T> slots_[N];
  uint64_t write_head_ = 0;  // single producer; may be ahead of the consumer-visible head_ until Publish()
  uint32_t pending_draws_ = 0;
  alignas(64) std::atomic<uint64_t> head_{0};  // written by the producer
  alignas(64) std::atomic<uint64_t> committed_{0};  // includes unpublished batch entries; watchdog-only view
  alignas(64) std::atomic<uint64_t> done_{0};  // written by the consumer
  alignas(64) std::atomic<bool> sync_waiting_{false}, stop_{false};
  std::atomic<uint64_t> executing_{0};
  std::atomic<uint32_t> worker_state_{0};
  std::atomic<uint64_t> sleeps_{0}, spurious_recoveries_{0};
  std::mutex m_;
  std::condition_variable cv_work_, cv_done_;
  RecordQueueStats stats_;
  std::thread worker_;
};

}  // namespace nr
