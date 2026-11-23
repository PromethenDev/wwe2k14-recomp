// GAP-cpu1 (lane cpu2): the timer queue with an optional sleeping dispatch thread.
//
// Game-side replacement of the SDK's src/core/timer_queue.cpp: this file defines the same three public functions
// (QueueTimerOnce, QueueTimerRecurring, TimerQueueWaitItem::Disarm), so the linker never pulls the SDK's object from
// librexcore and no SDK file changes. With the switch unset the dispatch loop is the SDK's loop, unchanged.
//
// Why: the queue's only steady job in a match is the 1 ms kernel time tick (KeTimeStampBundle, xboxkrnl_module.cpp).
// Between ticks the SDK's loop waits in disruptorplus::spin_wait: 4092 pause instructions, then yields, a 1 ms sleep only
// every 20th round, and the spinner restarts after each tick. Measured (docs/tasks/GAP-cpu1-report.md): 16.7 % of a core
// on Linux, 12.1 % on the 780M laptop, with 0.6 % of the samples in the callback.
//
//   WWE2K14_TIMER_SLEEP=1      sleep until the next due time (never longer than 1 ms, so a newly queued timer waits at
//                              most about 1 ms); Windows: high-resolution waitable timer (ring_wait_pacing.h)
//   WWE2K14_TIMER_SLEEP=2      keep spinning, but log the same statistics (control arm for lateness)
//   WWE2K14_TIMER_SLEEP_AB=S   in-run A/B: even S-second blocks spin, odd blocks sleep (implies the statistics)
// With any of these set, one [timer-sleep] line per block (10 s without A/B): mode, this thread's CPU, callbacks per
// second, callback lateness p50/p99/max. Lateness is now - due at the callback, the only thing this thread can change.
//
// Copyright 2022 Ben Vanik (Xenia), released under the BSD license; adapted for ReXGlue by Tom Clay, 2026 (the queue
// structure, wait item states and Disarm below are theirs); sleeping dispatch and statistics are this project's.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <forward_list>
#include <memory>
#include <thread>

#include <disruptorplus/multi_threaded_claim_strategy.hpp>
#include <disruptorplus/ring_buffer.hpp>
#include <disruptorplus/sequence_barrier.hpp>
#include <disruptorplus/spin_wait_strategy.hpp>

#include <rex/assert.h>
#include <rex/logging.h>
#include <rex/thread.h>
#include <rex/thread/timer_queue.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#endif

#include "native/ring_wait_pacing.h"

namespace dp = disruptorplus;

namespace rex::thread {

using WaitItem = TimerQueueWaitItem;

namespace {

uint64_t ThisThreadCpuNs() {
#if defined(_WIN32)
  FILETIME c, e, k, u;
  if (!GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u)) return 0;
  const auto to100 = [](FILETIME f) { return (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
  return (to100(k) + to100(u)) * 100;
#else
  timespec ts{};
  if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0) return 0;
  return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
#endif
}

// Written once by the first QueueTimer call (after TEST-ENV.txt was applied), before that timer is published.
std::atomic<int> g_mode{0};         // 0 unset: the SDK's behaviour, no statistics. 1 sleep. 2 spin with statistics.
std::atomic<int64_t> g_ab_s{0};     // > 0: in-run A/B block length in seconds

void ReadSwitchOnce() {
  static const bool done = [] {
    if (const char* e = std::getenv("WWE2K14_TIMER_SLEEP"); e && *e)
      g_mode.store(std::strcmp(e, "1") == 0 ? 1 : (std::strcmp(e, "2") == 0 ? 2 : 0), std::memory_order_relaxed);
    if (const char* e = std::getenv("WWE2K14_TIMER_SLEEP_AB"); e && *e) {
      const int64_t s = std::strtol(e, nullptr, 10);
      if (s > 0) {
        g_ab_s.store(s, std::memory_order_relaxed);
        if (g_mode.load(std::memory_order_relaxed) == 0) g_mode.store(2, std::memory_order_relaxed);
      }
    }
    return true;
  }();
  (void)done;
}

class LateStats {
 public:
  void Note(int64_t late_ns) {
    const uint64_t us = late_ns > 0 ? uint64_t(late_ns) / 1000 : 0;
    ++n_;
    max_us_ = std::max(max_us_, us);
    ++buckets_[std::min<uint64_t>(us / kBucketUs, kBuckets - 1)];
  }
  // Logs and restarts the window.
  void Report(int mode, double wall_s, uint64_t cpu_ns) {
    uint32_t p50 = 0, p99 = 0;
    uint64_t seen = 0;
    for (uint32_t i = 0; i < kBuckets; ++i) {
      if (seen < n_ / 2 && seen + buckets_[i] >= n_ / 2) p50 = i * kBucketUs;
      if (seen < n_ - n_ / 100 && seen + buckets_[i] >= n_ - n_ / 100) p99 = i * kBucketUs;
      seen += buckets_[i];
    }
    REXLOG_INFO("[timer-sleep] mode={} wall_s={:.1f} cpu_pct={:.2f} callbacks_per_s={:.0f} late_us p50={} p99={} max={}",
                mode == 1 ? "sleep" : "spin", wall_s, 100.0 * double(cpu_ns) / (wall_s * 1e9),
                double(n_) / wall_s, p50, p99, max_us_);
    *this = LateStats{};
  }

 private:
  static constexpr uint32_t kBucketUs = 50, kBuckets = 400;  // 0..20 ms
  uint64_t n_ = 0, max_us_ = 0;
  uint64_t buckets_[kBuckets] = {};
};

}  // namespace

class TimerQueue {
 public:
  using clock = WaitItem::clock;
  static_assert(clock::is_steady);

  TimerQueue()
      : buffer_(kWaitCount),
        wait_strategy_(),
        claim_strategy_(kWaitCount, wait_strategy_),
        consumed_(wait_strategy_) {
    claim_strategy_.add_claim_barrier(consumed_);
    dispatch_thread_ = std::jthread([this](std::stop_token stop_token) { TimerThreadMain(stop_token); });
  }

  ~TimerQueue() {
    dispatch_thread_.request_stop();

    // Kick dispatch thread to check stop token
    auto wait_item = std::make_shared<WaitItem>(nullptr, nullptr, this, clock::time_point::min(),
                                                clock::duration::zero());
    wait_item->Disarm();
    QueueTimer(std::move(wait_item));

    // std::jthread auto-joins on destruction
  }

  void TimerThreadMain(std::stop_token stop_token) {
    dp::sequence_t next_sequence = 0;
    const auto comp = [](const std::shared_ptr<WaitItem>& left, const std::shared_ptr<WaitItem>& right) {
      return left->due_ < right->due_;
    };

    set_current_thread_name("rex::thread::TimerQueue");

    LateStats stats;
    clock::time_point window_start{};
    uint64_t window_cpu = 0;
    int window_mode = 0;

    while (!stop_token.stop_requested()) {
      const int switch_mode = g_mode.load(std::memory_order_relaxed);
      int mode = switch_mode;
      if (switch_mode != 0) {
        // Statistics and A/B blocks: nothing below runs with the switch unset.
        const auto now = clock::now();
        if (window_mode == 0) {
          window_start = now;
          window_cpu = ThisThreadCpuNs();
        }
        const int64_t ab = g_ab_s.load(std::memory_order_relaxed);
        if (ab > 0) {
          const auto s = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
          mode = (s / ab) & 1 ? 1 : 2;
        }
        const double wall_s = std::chrono::duration<double>(now - window_start).count();
        if (window_mode != 0 && (mode != window_mode || wall_s >= 10.0)) {
          const uint64_t cpu = ThisThreadCpuNs();
          stats.Report(window_mode, wall_s, cpu - window_cpu);
          window_start = now;
          window_cpu = cpu;
        }
        window_mode = mode;
      }
      {
        // Consume new wait items and add them to sorted wait queue
        dp::sequence_t available;
        if (mode == 1) {
          available = claim_strategy_.last_published_after(next_sequence - 1);
          if (available == next_sequence - 1) {
            auto wait = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::milliseconds(1));
            if (!wait_queue_.empty()) {
              const auto to_due =
                  std::chrono::ceil<std::chrono::microseconds>(wait_queue_.front()->due_ - clock::now());
              wait = std::min(wait, to_due);
            }
            if (wait.count() > 0) {
              // A very short wait still sleeps: the tick is not worth a spin.
              wwe2k14::ring_wait_pacing::WaitForSleepStep(std::max(wait, std::chrono::microseconds(100)));
              available = claim_strategy_.last_published_after(next_sequence - 1);
            }
          }
        } else {
          available = claim_strategy_.wait_until_published(
              next_sequence, next_sequence - 1,
              wait_queue_.empty() ? clock::time_point::max() : wait_queue_.front()->due_);
        }

        // Check for timeout
        if (available != next_sequence - 1) {
          std::forward_list<std::shared_ptr<WaitItem>> wait_items;
          do {
            wait_items.push_front(std::move(buffer_[next_sequence]));
          } while (next_sequence++ != available);

          consumed_.publish(available);

          wait_items.sort(comp);
          wait_queue_.merge(wait_items, comp);
        }
      }

      {
        // Check wait queue, invoke callbacks and reschedule
        std::forward_list<std::shared_ptr<WaitItem>> wait_items;
        while (!wait_queue_.empty() && wait_queue_.front()->due_ <= clock::now()) {
          auto wait_item = std::move(wait_queue_.front());
          wait_queue_.pop_front();

          // Ensure that it isn't disarmed
          auto state = WaitItem::State::kIdle;
          if (wait_item->state_.compare_exchange_strong(state, WaitItem::State::kInCallback,
                                                        std::memory_order_acq_rel)) {
            if (switch_mode != 0)
              stats.Note(std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - wait_item->due_).count());
            // Possibility to dispatch to a thread pool here
            assert_not_null(wait_item->callback_);
            wait_item->callback_(wait_item->userdata_);

            if (wait_item->interval_ != clock::duration::zero() &&
                wait_item->state_.load(std::memory_order_acquire) !=
                    WaitItem::State::kInCallbackSelfDisarmed) {
              // Item is recurring and didn't self-disarm during callback:
              wait_item->due_ += wait_item->interval_;
              wait_item->state_.store(WaitItem::State::kIdle, std::memory_order_release);
              wait_item->state_.notify_all();
              wait_items.push_front(std::move(wait_item));
            } else {
              wait_item->state_.store(WaitItem::State::kDisarmed, std::memory_order_release);
              wait_item->state_.notify_all();
            }
          } else {
            // Specifically, kInCallback is illegal here
            assert_true(WaitItem::State::kDisarmed == state);
          }
        }
        wait_items.sort(comp);
        wait_queue_.merge(wait_items, comp);
      }
    }
  }

  std::weak_ptr<WaitItem> QueueTimer(std::shared_ptr<WaitItem> wait_item) {
    ReadSwitchOnce();
    auto wait_item_weak = std::weak_ptr<WaitItem>(wait_item);

    // Mitigate callback flooding
    wait_item->due_ = std::max(clock::now() - wait_item->interval_, wait_item->due_);

    auto sequence = claim_strategy_.claim_one();
    buffer_[sequence] = std::move(wait_item);
    claim_strategy_.publish(sequence);

    return wait_item_weak;
  }

  std::jthread::id dispatch_thread_id() const { return dispatch_thread_.get_id(); }

 private:
  // This ring buffer will be used to introduce timers queued by the public API
  static constexpr size_t kWaitCount = 512;
  dp::ring_buffer<std::shared_ptr<WaitItem>> buffer_;
  dp::spin_wait_strategy wait_strategy_;
  dp::multi_threaded_claim_strategy<dp::spin_wait_strategy> claim_strategy_;
  dp::sequence_barrier<dp::spin_wait_strategy> consumed_;

  // This is a _sorted_ (ascending due_) list of active timers managed by a
  // dedicated thread
  std::forward_list<std::shared_ptr<WaitItem>> wait_queue_;
  std::jthread dispatch_thread_;
};

rex::thread::TimerQueue timer_queue_;

void TimerQueueWaitItem::Disarm() {
  State state;

  // Special case for calling from a callback itself
  if (std::this_thread::get_id() == parent_queue_->dispatch_thread_id()) {
    state = State::kInCallback;
    if (state_.compare_exchange_strong(state, State::kInCallbackSelfDisarmed,
                                       std::memory_order_acq_rel)) {
      // If we are self disarming from the callback set this special state and
      // exit
      return;
    }
    // Normal case can handle the rest
  }

  state = State::kIdle;
  // Classes which hold WaitItems will often call Disarm() to cancel them during
  // destruction. This may lead to race conditions when the dispatch thread
  // executes a callback which accesses memory that is freed simultaneously due
  // to this. Therefore, we need to guarantee that no callbacks will be running
  // once Disarm() has returned.
  while (!state_.compare_exchange_weak(state, State::kDisarmed, std::memory_order_acq_rel)) {
    if (state == State::kDisarmed) {
      break;
    }
    if (state == State::kInCallback || state == State::kInCallbackSelfDisarmed) {
      // Wait for callback to complete - dispatch thread will notify
      state_.wait(state, std::memory_order_acquire);
    }
    state = State::kIdle;
  }
}

std::weak_ptr<WaitItem> QueueTimerOnce(std::move_only_function<void(void*)> callback,
                                       void* userdata, WaitItem::clock::time_point due) {
  return timer_queue_.QueueTimer(std::make_shared<WaitItem>(
      std::move(callback), userdata, &timer_queue_, due, WaitItem::clock::duration::zero()));
}

std::weak_ptr<WaitItem> QueueTimerRecurring(std::move_only_function<void(void*)> callback,
                                            void* userdata, WaitItem::clock::time_point due,
                                            WaitItem::clock::duration interval) {
  return timer_queue_.QueueTimer(
      std::make_shared<WaitItem>(std::move(callback), userdata, &timer_queue_, due, interval));
}

}  // namespace rex::thread
