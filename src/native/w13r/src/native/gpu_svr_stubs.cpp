// SvR 2008 versions of re:Blue renderer hooks into Blue Dragon-specific systems.
//
// re:Blue tunes shadows/reflections for Blue Dragon's engine (gpu/hooks/tweaks.cpp) and predicts
// and precaches pipelines from Blue Dragon's recorded pipeline list (pipeline/pso_predictor.cpp,
// pso_precache.cpp). SvR has neither yet: scales stay 1, pipelines compile when first used.

#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>

#include <windows.h>

#include <rex/cvar.h>

#include "core/logging.h"
#include "gpu/device.h"
#include "gpu/hooks/tweaks.h"
#include "gpu/pipeline/pipeline_cache.h"
#include "gpu/pipeline/pso_precache.h"
#include "gpu/pipeline/pso_predictor.h"

REXCVAR_DECLARE(bool, bd_pso_precache);

namespace bd::gpu {

f64 ShadowCoverageScale() { return 1.0; }
f32 SceneRenderScale() { return 1.0f; }

void OnModelTechniqueKnown(u32, bool) {}
void OnLoadModelCreated(u32, u32) {}
void OnLoadBegin(u32) {}
void OnLoadEnd() {}
void OnDeclRegistered(u32, u8) {}
void ReemitPredictions() {}
bool IsPairPredicted(u64, u64) { return false; }
std::string DescribePair(u64, u64) { return {}; }

// Background pipeline compiler for the pipelines remembered from earlier sessions
// (pso_recorder.cpp, Documents\wwe12\cache\pipelines.bin): one low-priority thread builds them
// through the same cache as the render thread, so the first draw finds them ready.
namespace {
std::mutex g_queue_mutex;
std::condition_variable g_queue_cv;
std::deque<PipelineState> g_queue;
std::unordered_set<u64> g_queued;

void CompileWorker() {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
  u64 built = 0;
  for (;;) {
    PipelineState state;
    {
      std::unique_lock lock(g_queue_mutex);
      if (g_queue.empty() && built) {
        BD_DEBUG("pso_precache: {} pipeline(s) prepared in the background", built);
        built = 0;
      }
      g_queue_cv.wait(lock, [] { return !g_queue.empty(); });
      state = g_queue.front();
      g_queue.pop_front();
    }
    bool created = false;
    GetOrCreatePipeline(state, &created);
    built += created;
  }
}

void Enqueue(const PipelineState &state, bool front) {
  if (!Video::HostDevice())
    return;
  static std::once_flag once;
  std::call_once(once, [] {
#if defined(__ANDROID__)
    // Phone drivers take tens of ms per pipeline: one thread fell behind the match load, and the
    // render thread compiled the rest itself (5.6 s frozen on a Snapdragon 7s Gen 2).
    constexpr int kWorkers = 3;
#else
    constexpr int kWorkers = 1;
#endif
    for (int i = 0; i < kWorkers; ++i)
      std::thread(CompileWorker).detach();
  });
  {
    std::lock_guard lock(g_queue_mutex);
    if (!g_queued.insert(HashPipelineState(state)).second)
      return;
    if (front)
      g_queue.push_front(state);
    else
      g_queue.push_back(state);
  }
  g_queue_cv.notify_one();
}
}  // namespace

bool PrecacheEnabled() { return REXCVAR_GET(bd_pso_precache); }
void EnqueuePipeline(const PipelineState &state) { Enqueue(state, false); }
void EnqueuePipelinePriority(const PipelineState &state) { Enqueue(state, true); }
void BeginLoadCapture() {}
void EndLoadCapture() {}

}  // namespace bd::gpu
