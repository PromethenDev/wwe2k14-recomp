// U9 default-off per-draw diagnostic. Backend snapshots are logged by the frontend via REXLOG_INFO.
#pragma once
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
namespace nr::backend::draw_profile {
inline bool On() {
  static const bool on = [] { const char* e = std::getenv("WWE2K14_D3D_PD_PROF"); return e && !std::strcmp(e, "1"); }();
  return on;
}
inline uint64_t Now() {
  return On() ? uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count()) : 0;
}
enum Worker { kAcquireSlot, kHandoffWait, kSlotReset, kTakeFrom, kProducerReset, kAttachJobs, kPublish, kSubmitAcquire, kSubmitFullWait, kWorkerN };
inline constexpr const char* kWorkerNames[] = {
  "AcquireRecordSlot(inclusive)", "handoff_full_wait(subset)", "slot_Reset", "TakeFrom_or_swap", "producer_Reset", "attach_jobs", "SubmitBatch", "Submit_acquire(inclusive)", "Submit_full_wait(subset)"};
enum Record { kPreJobs, kExecute, kConsts, kPipeline, kUpload, kRecord, kRecordN };
inline constexpr const char* kRecordNames[] = {"pre_ops_jobs", "execute(inclusive)", "consts", "pipeline", "upload", "record"};
struct Snapshot {
  uint64_t worker_ns[kWorkerN]{}, worker_n[kWorkerN]{};
  uint64_t record_ns[kRecordN]{}, record_n[kRecordN]{};
  uint64_t swaps = 0, checks = 0, mismatches = 0, fallbacks = 0;
};
// Called on producer thread. Record counters use atomic exchanges; timing windows may straddle a draw.
bool Active();
Snapshot Take();
}  // namespace nr::backend::draw_profile
