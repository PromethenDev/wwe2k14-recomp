// D3D-fed renderer lifecycle extracted from the packet frontend (B6-4a).
#include "runtime.h"
#include "hle_marker.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/diagnostics/freeze_logger.h>
#include <rex/graphics/flags.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>

#include "gate.h"
#include "capture.h"
#include "late_upload.h"
#include "rr_play.h"
#include "present.h"
#include "wide_aspect.h"
#include "ultrawide_probe.h"
#include "textures.h"
#include "g18c_diag.h"  // TEST-ONLY card G18 diagnostic
#include "worker.h"
#include "../nr/nr_backend.h"
#include "../nr/nr_pc_sampler.h"
#include "../nr/nr_shared.h"

namespace wwe2k14::d3d::runtime {
namespace {

namespace nb = nr::backend;

bool EnvIsOne(const char* name) {
  const char* value = std::getenv(name);
  return value && std::strcmp(value, "1") == 0;
}

bool EnvIsZero(const char* name) {
  const char* value = std::getenv(name);
  return value && std::strcmp(value, "0") == 0;
}

void UpdateWidePresenter(uint64_t ord) {
  static const float wide_k = wide_core::ParseK(std::getenv("WWE2K14_D3D_WIDE"));
  if (!wide_core::IsEnabled(wide_k)) return;
  static const bool aspect_requested = wide_aspect::Enabled();
  const bool game_aspect_enabled = aspect_requested && wide_aspect::WideProjectionActive();

  static bool initialized = false;
  static wide_core::FrameModeHysteresis mode;
  if (!initialized) {
    const std::string flag = rex::cvar::GetFlagByName("present_letterbox");
    const bool initial_letterboxed = aspect_requested || (flag != "false" && flag != "0");
    mode = wide_core::FrameModeHysteresis(initial_letterboxed);
    if (aspect_requested) {
      wide_aspect::SetWideProjectionActive(false);
      rex::cvar::SetFlagByName("present_letterbox", "true");
    }
    initialized = true;
    if (aspect_requested) {
      REXLOG_INFO("[d3d-wide] present_letterbox start=true (aspect mode starts with native 16:9 projection)");
    } else {
      REXLOG_INFO("[d3d-wide] present_letterbox start={} (read current SDK flag)",
                  initial_letterboxed ? "true" : "false");
    }
  }

  const wide_core::FrameDrawStats stats = TakeWideSwapStats(ord);
  const uint32_t scene_draws = wide_core::SceneDrawCountForFrameMode(stats, game_aspect_enabled);
  const uint32_t scene_draw_threshold = aspect_requested ? wide_core::FrameModeHysteresis::kAspectSceneDrawThreshold
                                                         : wide_core::FrameModeHysteresis::kSceneDrawThreshold;
  const bool mode_changed = aspect_requested ? mode.UpdateAspect(scene_draws)
                                             : mode.Update(scene_draws, scene_draw_threshold);
  if (mode_changed) {
    const char* value = mode.letterboxed ? "true" : "false";
    rex::cvar::SetFlagByName("present_letterbox", value);
    if (aspect_requested) {
      const bool use_wide_projection = !mode.letterboxed;
      const wide_aspect::AspectWriteResult write = wide_aspect::SetWideProjectionActive(use_wide_projection);
      if (write.ready) {
        REXLOG_INFO("[d3d-wide] present_letterbox={} game_aspect={} address=0x8202B768 old={} (0x{:08X}) new={} "
                    "(0x{:08X})",
                    value, use_wide_projection ? "wide" : "16:9", std::bit_cast<float>(write.old_bits),
                    write.old_bits, std::bit_cast<float>(write.new_bits), write.new_bits);
      } else {
        REXLOG_INFO("[d3d-wide] present_letterbox={} game_aspect={} address=0x8202B768 patch=pending",
                    value, use_wide_projection ? "wide" : "16:9");
      }
    } else {
      REXLOG_INFO("[d3d-wide] present_letterbox={}", value);
    }
  }
  static const bool wide_probe_enabled = wide_probe::Enabled();
  if (wide_probe_enabled) wide_probe::LogAspectReaderCounts();
  static bool probe_stretched_frame_noted = false;
  if (wide_probe_enabled && !probe_stretched_frame_noted && !mode.letterboxed &&
      scene_draws >= scene_draw_threshold) {
    wide_probe::NoteStretchedFrame();
    probe_stretched_frame_noted = true;
  }

  static uint32_t sample_frames = 0;
  static uint64_t squeezed = 0, hud_squeezed = 0, skipped_screen_space = 0, skipped_fullscreen = 0,
                  aspect_scene_draws = 0, skipped_no_target = 0, skipped_no_depth = 0,
                  letterboxed_frames = 0, stretched_frames = 0;
  ++sample_frames;
  squeezed += stats.squeezed;
  aspect_scene_draws += stats.aspect_scene_draws;
  hud_squeezed += stats.hud_squeezed;
  skipped_screen_space += stats.skipped_screen_space;
  skipped_fullscreen += stats.skipped_fullscreen;
  skipped_no_target += stats.skipped_no_target;
  skipped_no_depth += stats.skipped_no_depth;
  if (mode.letterboxed) ++letterboxed_frames;
  else ++stretched_frames;
  if (sample_frames >= 600) {
    if (game_aspect_enabled) {
      REXLOG_INFO("[d3d-wide] frames={} squeezed={} aspect_scene_draws={} hud_squeezed={} skipped_screen_space={} "
                  "skipped_fullscreen={} skipped_no_target={} skipped_no_depth_test={} letterboxed_frames={} "
                  "stretched_frames={}",
                  sample_frames, squeezed, aspect_scene_draws, hud_squeezed, skipped_screen_space, skipped_fullscreen,
                  skipped_no_target, skipped_no_depth, letterboxed_frames, stretched_frames);
    } else {
      REXLOG_INFO("[d3d-wide] frames={} squeezed={} hud_squeezed={} skipped_screen_space={} skipped_fullscreen={} "
                  "skipped_no_target={} skipped_no_depth_test={} letterboxed_frames={} stretched_frames={}",
                  sample_frames, squeezed, hud_squeezed, skipped_screen_space, skipped_fullscreen, skipped_no_target,
                  skipped_no_depth, letterboxed_frames, stretched_frames);
    }
    sample_frames = 0;
    squeezed = hud_squeezed = skipped_screen_space = skipped_fullscreen = aspect_scene_draws = skipped_no_target =
        skipped_no_depth = 0;
    letterboxed_frames = stretched_frames = 0;
  }
}

uint64_t NowNs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

constexpr size_t kFixAltDtBuckets = 201;  // 1 ms buckets; bucket 200 contains 200 ms and above.

struct GuestFlipSnapshot {
  uint64_t frames = 0, interval1_frames = 0, on_time = 0, le17_5 = 0, gt33 = 0, gt50 = 0, interval2_frames = 0;
  std::array<uint64_t, kFixAltDtBuckets> dt_hist{};
};

struct GuestFlipAccumulator {
  void Record(uint64_t delta_us, uint32_t interval) {
    if (skip_next.exchange(false, std::memory_order_relaxed)) return;
    if (delta_us == 0) return;
    frames.fetch_add(1, std::memory_order_relaxed);
    if (interval == 1) {
      interval1_frames.fetch_add(1, std::memory_order_relaxed);
      if (delta_us >= 14000 && delta_us <= 19500) on_time.fetch_add(1, std::memory_order_relaxed);
    }
    if (delta_us <= 17500) le17_5.fetch_add(1, std::memory_order_relaxed);
    if (delta_us > 33000) gt33.fetch_add(1, std::memory_order_relaxed);
    if (delta_us > 50000) gt50.fetch_add(1, std::memory_order_relaxed);
    if (interval == 2) interval2_frames.fetch_add(1, std::memory_order_relaxed);
    const size_t bucket = std::min<size_t>(size_t(delta_us / 1000), kFixAltDtBuckets - 1);
    dt_hist[bucket].fetch_add(1, std::memory_order_relaxed);
  }

  GuestFlipSnapshot Take() {
    GuestFlipSnapshot snapshot;
    snapshot.frames = frames.exchange(0, std::memory_order_relaxed);
    snapshot.interval1_frames = interval1_frames.exchange(0, std::memory_order_relaxed);
    snapshot.on_time = on_time.exchange(0, std::memory_order_relaxed);
    snapshot.le17_5 = le17_5.exchange(0, std::memory_order_relaxed);
    snapshot.gt33 = gt33.exchange(0, std::memory_order_relaxed);
    snapshot.gt50 = gt50.exchange(0, std::memory_order_relaxed);
    snapshot.interval2_frames = interval2_frames.exchange(0, std::memory_order_relaxed);
    for (size_t i = 0; i < kFixAltDtBuckets; ++i) {
      snapshot.dt_hist[i] = dt_hist[i].exchange(0, std::memory_order_relaxed);
    }
    return snapshot;
  }

  void SkipNext() { skip_next.store(true, std::memory_order_relaxed); }

  std::atomic<bool> skip_next{false};
  std::atomic<uint64_t> frames{0}, interval1_frames{0}, on_time{0}, le17_5{0}, gt33{0}, gt50{0}, interval2_frames{0};
  std::array<std::atomic<uint64_t>, kFixAltDtBuckets> dt_hist{};
};

struct FixAltWindow {
  bool active = false;
  bool phase_b = false;
  uint64_t start_ns = 0, draws_start = 0;
  uint64_t frames = 0, interval1_frames = 0, on_time = 0, le17_5 = 0, gt33 = 0, gt50 = 0, interval2_frames = 0;
  std::array<uint64_t, kFixAltDtBuckets> dt_hist{};
};

GuestFlipAccumulator g_fix_alt_flip_accumulator;
FixAltWindow g_fix_alt_window;

void AddGuestFlipSnapshot(const GuestFlipSnapshot& snapshot) {
  FixAltWindow& window = g_fix_alt_window;
  window.frames += snapshot.frames;
  window.interval1_frames += snapshot.interval1_frames;
  window.on_time += snapshot.on_time;
  window.le17_5 += snapshot.le17_5;
  window.gt33 += snapshot.gt33;
  window.gt50 += snapshot.gt50;
  window.interval2_frames += snapshot.interval2_frames;
  for (size_t i = 0; i < kFixAltDtBuckets; ++i) window.dt_hist[i] += snapshot.dt_hist[i];
}

double FixAltPercentile(const std::array<uint64_t, kFixAltDtBuckets>& histogram, uint64_t frames, uint32_t percentile) {
  if (!frames) return 0.0;
  const uint64_t target = (frames * percentile + 99) / 100;
  uint64_t cumulative = 0;
  for (size_t i = 0; i < kFixAltDtBuckets; ++i) {
    cumulative += histogram[i];
    if (cumulative >= target) return double(i);
  }
  return double(kFixAltDtBuckets - 1);
}

void StartFixAltWindow(uint64_t now_ns, bool phase_b, uint64_t draws) {
  g_fix_alt_window = FixAltWindow{};
  g_fix_alt_window.active = true;
  g_fix_alt_window.phase_b = phase_b;
  g_fix_alt_window.start_ns = now_ns;
  g_fix_alt_window.draws_start = draws;
}

void CloseFixAltWindow(uint64_t now_ns, uint64_t draws, int period_seconds) {
  FixAltWindow& window = g_fix_alt_window;
  if (!window.active) return;
  const double seconds = now_ns >= window.start_ns ? double(now_ns - window.start_ns) * 1e-9 : 0.0;
  const double frame_div = window.frames ? double(window.frames) : 1.0;
  const double on_time_pct = window.interval1_frames ? 100.0 * double(window.on_time) / window.interval1_frames : 0.0;
  const double le17_5_pct = window.frames ? 100.0 * double(window.le17_5) / window.frames : 0.0;
  const double draws_per_frame = window.frames && draws >= window.draws_start
                                     ? double(draws - window.draws_start) / frame_div
                                     : 0.0;
  REXLOG_INFO("[nr-fix-alt] phase={} period_s={} secs={:.2f} frames={} fps={:.2f} on_time={:.1f}% le17_5={:.1f}% "
              "gt33={} gt50={} dt_p50={:.0f}ms dt_p95={:.0f}ms interval2_frames={} draws_per_frame={:.1f}",
              window.phase_b ? "B" : "A", period_seconds, seconds, window.frames,
              seconds > 0.0 ? double(window.frames) / seconds : 0.0, on_time_pct, le17_5_pct, window.gt33,
              window.gt50, FixAltPercentile(window.dt_hist, window.frames, 50),
              FixAltPercentile(window.dt_hist, window.frames, 95), window.interval2_frames, draws_per_frame);
  window.active = false;
}

void FixAltSwapBoundary(uint64_t now_ns, uint64_t draws) {
  if (!g_fix_alt_window.active) StartFixAltWindow(now_ns, nb::FixAltPhaseB(), draws);
  AddGuestFlipSnapshot(g_fix_alt_flip_accumulator.Take());
  const int new_phase = nb::FixAltFrame(now_ns);
  if (new_phase >= 0) {
    CloseFixAltWindow(now_ns, draws, nb::FixAltPeriodSeconds());
    g_fix_alt_flip_accumulator.SkipNext();
    StartFixAltWindow(now_ns, new_phase == 1, draws);
  }
}

// This lock is shared with the D3D worker. CP callbacks must take it only after any gate wait, because the worker
// holds it while consuming precisely the event being waited for.
class BackendGuard {
 public:
  BackendGuard() {
    static const bool on = ShadowMode() == 2;
    if (on) {
      lock_ = &wwe2k14::d3d::BackendLock();
      lock_->lock();
    }
  }
  ~BackendGuard() {
    if (lock_) lock_->unlock();
  }

 private:
  BackendMutex* lock_ = nullptr;
};

}  // namespace

void NoteFixAltGuestFlip(uint64_t delta_us, uint32_t interval) {
  g_fix_alt_flip_accumulator.Record(delta_us, interval);
}

Runtime::Runtime() = default;

Runtime& Get() {
  static Runtime runtime;
  return runtime;
}
void BootBackendFromWorker() { Get().BootFromWorker(); }

void Runtime::EnsureBackend(uint32_t ordinal) {
  if (backend_init_failed_) return;

  const std::filesystem::path shader_dir = std::getenv("WWE2K14_NR_SHADERS")
                                               ? std::getenv("WWE2K14_NR_SHADERS")
                                               : "out/nr/shaders";
  const std::filesystem::path shader_dir_b = std::getenv("WWE2K14_NR_SHADERS_B")
                                                 ? std::getenv("WWE2K14_NR_SHADERS_B")
                                                 : "";
  const bool f24_conservative = EnvIsOne("WWE2K14_NR_F24_CONSERVATIVE");
  nr::shared::ShaderSet& shaders = nr::shared::ShaderSet::Get();
  shaders.Load(shader_dir, shader_dir_b, f24_conservative);
  if (nb::BackendWantsDxil() && !shaders.FormatMatchesBackend()) {
    REXLOG_ERROR("[nr] init {}: WWE2K14_NR_BACKEND=d3d12 requires a DXIL shader manifest in {}", ordinal,
                 shader_dir.string());
    backend_init_failed_ = true;
    return;
  }
  nb::SetProfiling(EnvIsOne("WWE2K14_NR_PROFILE"));

  if (!nb::Ready()) {
    std::string error;
    rex::graphics::NrD3D12Device d3d12_dev;
    if (EnvIsOne("WWE2K14_NR_DIRECT_PRESENT") && nb::BackendWantsDxil()) {
      // Card C3: the SDK in D3D12 mode exposes its device and direct queue (nr_d3d12_shared_device).
      if (rex::graphics::GetNrD3D12Device(&d3d12_dev)) {
        nb::SharedD3D12Device shared;
        shared.device = d3d12_dev.device;
        shared.direct_queue = d3d12_dev.direct_queue;
        nb::SetSharedD3D12Device(shared);
        REXLOG_INFO("[nr] init {}: shared D3D12 device", ordinal);
      } else {
        REXLOG_INFO("[nr] init {}: shared D3D12 device unavailable (SDK not in D3D12 mode or nr_d3d12_shared_device "
                    "off); backend uses its own device, no direct present",
                    ordinal);
      }
    } else if (EnvIsOne("WWE2K14_NR_DIRECT_PRESENT")) {
      rex::graphics::NrVulkanDevice dev;
      if (rex::graphics::GetNrVulkanDevice(&dev)) {
        nb::SharedVulkanDevice shared;
        shared.instance = dev.instance;
        shared.get_instance_proc_addr = dev.get_instance_proc_addr;
        shared.api_version = dev.api_version;
        shared.physical_device = dev.physical_device;
        shared.device = dev.device;
        shared.queue_family = dev.queue_family;
        shared.queue = dev.queue;
        shared.queue_mutex = dev.queue_mutex;
        shared.descriptor_indexing = dev.descriptor_indexing;
        shared.scalar_block_layout = dev.scalar_block_layout;
        shared.buffer_device_address = dev.buffer_device_address;
        shared.null_descriptor = dev.null_descriptor;
        shared.sampler_mirror_clamp_to_edge = dev.sampler_mirror_clamp_to_edge;
        shared.geometry_shader = dev.geometry_shader;
        nb::SetSharedVulkanDevice(shared);
        REXLOG_INFO("[nr] init {}: shared Vulkan device", ordinal);
      } else {
        REXLOG_INFO("[nr] init {}: shared Vulkan device unavailable; backend uses its own device", ordinal);
      }
    }
    if (!nb::Init(shader_dir.string(), &error)) {
      REXLOG_ERROR("[nr] draw-off backend init failed: {}", error);
      backend_init_failed_ = true;
      return;
    }
    REXLOG_INFO("[nr] draw-off backend initialized: device {}", nb::DeviceName());
    // Director 2026-10-19 (B6-4 review BLOCKER 1): resolve.hlsl lookup tables (fixed16 / 4-bit copy-out conversion),
    // computed with exactly the CPU conversion expressions; moved verbatim from the old nr_frontend EnsureInit.
    // D3D resolves of k_16_16 / k_16_16_16_16 use them via nb::SnapshotFixed16. WWE2K14_NR_CPU_COPY=1 still skips them.
    if (!EnvIsOne("WWE2K14_NR_CPU_COPY") && nb::ResolveAvailable()) {
      std::vector<uint32_t> lut(65536 + 256);
      for (uint32_t u = 0; u < 65536; ++u) {
        const int16_t q = int16_t(uint16_t(u));
        const float value = std::min(std::max(float(q) / 32767.0f * 32.0f, 0.0f), 1.0f);
        lut[u] = uint16_t(value * 65535.0f + 0.5f);
      }
      for (uint32_t b = 0; b < 256; ++b) {
        const float v = std::min(std::max(float(b) * 0.00392156886f, 0.0f), 1.0f);
        lut[65536 + b] = uint32_t(v * 15.0f + 0.5f);
      }
      std::string lut_err;
      if (nb::SetResolveLut(lut, &lut_err)) REXLOG_INFO("[nr] init {}: GPU copy-out conversion", ordinal);
      else REXLOG_ERROR("[nr] init {}: resolve LUT upload failed: {}", ordinal, lut_err);
    }

  }

  if (nb::Ready()) {
    nb::SetRetireByFrame(true);  // draw-off frame boundary is the resource-retirement point
    if (EnvIsOne("WWE2K14_D3D_LAZY_SNAP")) nb::SetLazySnapshots(true);
  }

  if (EnvIsOne("WWE2K14_NR_PC_SAMPLER") && !sampler_started_) {
    sampler_started_ = true;
    nr::sampler::Start(nb::RecordThreadNativeHandle(), nb::RecordThreadNativeHandle() != 0);
    const std::filesystem::path out = std::getenv("WWE2K14_NR_OUT") ? std::getenv("WWE2K14_NR_OUT") : "nr-out";
    nr::sampler::Dump((out / "pcsamples.txt").string());
  }
}

// Card DE-2M: the SDK NOP hook (CP thread, after the bin predicate). Only our indexed-draw markers (hle_marker.h) act: the
// first CP work may be a marker, so bootstrap the backend exactly as the first OnDraw does, then gate on the marker's seq.
// No ObservedDraw, shader analysis or host draw: the native worker draws it from the captured event.
void Runtime::OnNop(const rex::graphics::NrNopEvent& event) {
  if (event.payload_dwords < 1 || event.payload_dwords > 4 || event.payload_dwords == 3 || !event.predicated ||
      !event.memory)
    return;
  const uint8_t* const phys = event.memory->physical_membase();
  if (event.packet_guest_address > 0x1FFFFFFFu - 20u) return;
  uint32_t seq = 0;
  const uint8_t* const payload = phys + event.packet_guest_address + 4;
  if (event.payload_dwords == 1) {  // clear-helper marker: tag + low 24 bits of the seq
    if (!marker::DecodeOne(payload, ShadowLatestSeq(), &seq)) return;
  } else if (event.payload_dwords == 2) {  // UP-draw marker (written before EndVertices commits the cursor: never torn)
    if (!marker::DecodeUp(payload, &seq)) return;
  } else if (!marker::Decode(payload, &seq)) {
    // Review de23 BLOCKER 1: another thread's ring kick can expose the slot while the producer is still writing the marker
    // (header first). A torn marker gates on the newest allocated event, which is at or after its own (fails closed).
    if (marker::LoadBe(payload) != marker::kMagicA) return;
    seq = ShadowLatestSeq();
    ++torn_markers_;
    if (torn_markers_ <= 8) REXLOG_WARN("[d3d-marker] torn marker at 0x{:08X}: gating on latest seq {}", event.packet_guest_address, seq);
  }
  memory_ = event.memory;
  if (NrDrawOff() && !draw_off_init_.exchange(true)) {
    BackendGuard guard;
    EnsureBackend(0);
  }
  SetCpWhere(11);
  GateCpSeq(seq);
  SetCpWhere(0);
}

void Runtime::OnDraw(const rex::graphics::ObservedDraw& draw) {
  memory_ = draw.memory;
  if (NrDrawOff() && !draw_off_init_.exchange(true)) {
    // Initialize before the CP gate, matching the old observer: the D3D worker waits for Ready outside BackendLock.
    BackendGuard guard;
    EnsureBackend(draw.ordinal);
  }

  SetCpWhere(10);
  GateCpDraw(draw.packet_guest_address);
  SetCpWhere(0);

  if (NrDrawOff()) {
    if (!skip_set_) {
      REXCVAR_SET(nr_skip_host_draws, true);
      skip_set_ = true;
    }
    ++s3_off_draws_;
  }
}

void Runtime::OnSwap(uint32_t frame) {
  if (PresentFromD3d()) ++swap_ordinal_;
  else GateCpSwap(swap_ordinal_++);
  SetCpWhere(30);
  BackendGuard guard;
  SetCpWhere(31);

  nr::shared::ShaderSet& shaders = nr::shared::ShaderSet::Get();
  nb::MaybeStartPipelinePrewarm([&shaders] {
    std::vector<const std::vector<uint8_t>*> all;
    shaders.ForEachSpirv([&](const std::vector<uint8_t>& spirv) { all.push_back(&spirv); });
    return all;
  });
  // Card R14: before play, known pipelines are built first (no-op after / in background mode). The 780M needs ~11 s on a
  // cold driver cache with nothing presented, which tripped the 10 s freeze report (owner laptop 2026-10-29). While the
  // build keeps making progress, tell the freeze logger the game is alive; a build stuck for 5 s still gets reported.
  {
    const nb::PrewarmStatus st0 = nb::GetPrewarmStatus();
    if (st0.running && st0.before_play) {
      size_t last_done = st0.done;
      auto last_progress = std::chrono::steady_clock::now();
      for (;;) {
        const nb::PrewarmStatus st = nb::GetPrewarmStatus();
        if (!st.running) break;
        const auto now = std::chrono::steady_clock::now();
        if (st.done != last_done) {
          last_done = st.done;
          last_progress = now;
        }
        if (now - last_progress < std::chrono::seconds(5)) rex::diagnostics::freeze_logger::RecordPresent();
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
      }
    }
  }
  nb::WaitPipelinePrewarm();
  {
    static bool logged = false;
    if (!logged) {
      const nb::PrewarmStatus st = nb::GetPrewarmStatus();
      if (st.total && !st.running) {
        logged = true;
        REXLOG_INFO("[nr-prewarm] {} of {} pipelines in {:.1f} s ({})", st.done, st.total, st.seconds,
                    st.before_play ? "before play" : "background");
      }
    }
  }
  nb::MaybeSavePipelineCache();
  if (nb::RecordThreadActive()) {
    for (const nb::DrawFailure& failure : nb::TakeDrawFailures()) {
      REXLOG_ERROR("[nr] draw {} backend (async): {}", failure.ordinal, failure.error);
    }
  }
  nb::RecordQueueFrame(frame);
  uint64_t record_cpu_ns = 0;
  if (nb::TakeRecordThreadCpuDeltaNs(&record_cpu_ns))
    REXLOG_INFO("[nr-record-cpu] frame={} cpu_ms/f={:.3f}", frame, record_cpu_ns * 1e-6);
  static const bool phys_snap_on = !EnvIsZero("WWE2K14_NR_PHYS_SNAP");
  if (EnvIsOne("WWE2K14_NR_PROFILE")) {
    EmitProfile(frame);
    if (nb::GpuDrawsActive()) nb::GpuDrawsFrame(profile_frame_, false);
  } else if (phys_snap_on && nb::Ready()) {
    static uint64_t window_start = NowNs();
    const uint64_t now = NowNs();
    if (now - window_start >= 10000000000ull) {
      nb::Stats& stats = nb::GetStats();
      const double seconds = double(now - window_start) / 1.0e9;
      REXLOG_INFO("[nr-phys-snap] window_s={:.1f} color={} depth={} logical_materialized={} checks={} "
                  "mismatch_texels={} over_tol={} max_color={} max_depth_e6={} check_failures={} barriers={} "
                  "barrier_items={} barrier_legacy={} passes={}",
                  seconds, stats.phys_snap_color, stats.phys_snap_depth, stats.phys_snap_logical_materialized,
                  stats.phys_snap_checks, stats.phys_snap_mismatch_texels, stats.phys_snap_over_tol,
                  stats.phys_snap_max_color, stats.phys_snap_max_depth_e6, stats.phys_snap_check_failures,
                  stats.vk_barrier_calls, stats.vk_barrier_items, stats.vk_barrier_legacy, stats.elide_passes);
      stats.vk_barrier_calls = stats.vk_barrier_items = stats.vk_barrier_legacy = stats.elide_passes = 0;
      stats.phys_snap_color = stats.phys_snap_depth = stats.phys_snap_logical_materialized = 0;
      stats.phys_snap_checks = stats.phys_snap_mismatch_texels = stats.phys_snap_check_failures = 0;
      stats.phys_snap_over_tol = stats.phys_snap_max_color = stats.phys_snap_max_depth_e6 = 0;
      window_start = now;
    }
  }
  nb::CrowdFrame(frame);
  for (const std::string& line : nb::CrowdTakeLog()) REXLOG_INFO("{}", line);
  for (const std::string& line : nb::SurfaceTakeWarnings()) REXLOG_WARN("{}", line);
  SetCpWhere(0);
}

void Runtime::EmitProfile(uint32_t swap) {
  nb::RecordBatchFrameEnd(profile_frame_ + 1);
  nb::Stats& stats = nb::GetStats();
  const uint64_t now = NowNs();
  const uint64_t wall_ns = profile_frame_start_ns_ ? now - profile_frame_start_ns_ : 0;
  const double ms = 1e-6;
  ++profile_frame_;
  REXLOG_INFO("[nr-profile] frame={} swap={} wall_ms={:.3f} nr_ms={:.3f} draws=0 copies=0 d3d_draw_off=1 "
              "submits={} submit_ms={:.3f} kicks={} pipelines={} pipeline_ms={:.3f} adopted={} buffers={} upload_mb={:.2f} "
              "readback_mb={:.2f}",
              profile_frame_, swap, wall_ns * ms, stats.total_ns * ms, stats.submits, stats.submit_ns * ms, stats.kicks,
              stats.pipelines, stats.pipeline_ns * ms, stats.pipelines_adopted, stats.buffers, stats.upload_bytes / 1048576.0,
              stats.readback_bytes / 1048576.0);
  if (!EnvIsZero("WWE2K14_NR_PHYS_SNAP")) {
    static uint64_t window_start = now;
    static uint64_t color = 0, depth = 0, logical_materialized = 0, checks = 0, mismatch_texels = 0, check_failures = 0;
    static uint64_t over_tol = 0, max_color = 0, max_depth_e6 = 0;
    static uint64_t barriers = 0, barrier_items = 0, barrier_legacy = 0, passes = 0;
    color += stats.phys_snap_color;
    depth += stats.phys_snap_depth;
    logical_materialized += stats.phys_snap_logical_materialized;
    checks += stats.phys_snap_checks;
    mismatch_texels += stats.phys_snap_mismatch_texels;
    check_failures += stats.phys_snap_check_failures;
    over_tol += stats.phys_snap_over_tol;
    max_color = std::max(max_color, stats.phys_snap_max_color);
    max_depth_e6 = std::max(max_depth_e6, stats.phys_snap_max_depth_e6);
    barriers += stats.vk_barrier_calls;
    barrier_items += stats.vk_barrier_items;
    barrier_legacy += stats.vk_barrier_legacy;
    passes += stats.elide_passes;
    if (now - window_start >= 10000000000ull) {
      const double seconds = double(now - window_start) / 1.0e9;
      REXLOG_INFO("[nr-phys-snap] window_s={:.1f} color={} depth={} logical_materialized={} checks={} "
                  "mismatch_texels={} over_tol={} max_color={} max_depth_e6={} check_failures={} barriers={} "
                  "barrier_items={} barrier_legacy={} passes={}",
                  seconds, color, depth, logical_materialized, checks, mismatch_texels, over_tol, max_color,
                  max_depth_e6, check_failures, barriers, barrier_items, barrier_legacy, passes);
      window_start = now;
      color = depth = logical_materialized = checks = mismatch_texels = check_failures = 0;
      over_tol = max_color = max_depth_e6 = 0;
      barriers = barrier_items = barrier_legacy = passes = 0;
    }
  }
  const std::string batch_drain = nb::BatchDrainStatsLine();
  if (!batch_drain.empty()) REXLOG_INFO("[nr-batch-drain] frame={} {}", profile_frame_, batch_drain);
  stats = nb::Stats{};
  profile_frame_start_ns_ = now;
  if (profile_frame_ % 600 == 0 || profile_frame_ == 1) {
    const std::filesystem::path out = std::getenv("WWE2K14_NR_OUT") ? std::getenv("WWE2K14_NR_OUT") : "nr-out";
    nr::sampler::Dump((out / "pcsamples.txt").string());
  }
}

void Runtime::OnBeforeSwap() {
  SetCpWhere(20);
  if (PresentFromD3d()) {
    d3d_frame_ready_ = GateCpSwapStatus(swap_ordinal_) == 0;
    // The worker has finalized this swap's draw counts; update the cvar before the SDK presenter reads it.
    if (d3d_frame_ready_) UpdateWidePresenter(d3d_present_ordinal_);
  }
  SetCpWhere(21);
  BackendGuard guard;
  SetCpWhere(22);
  OnGpuInterruptFrameEnd();
  const uint64_t d3d_swap_ordinal = d3d_present_ordinal_++;
  if (nb::FixAltEnabled()) FixAltSwapBoundary(NowNs(), s3_off_draws_);
  if (const int mode = nb::CrowdFrameStart(); mode >= 0)
    REXLOG_INFO("[nr-crowd-mode] swap={} grouping_mode={} FIX_ALT_phase={}", d3d_swap_ordinal, mode,
                nb::FixAltPhaseB() ? "B" : "A");
  if (NrDrawOff()) PresentD3dOnly();
  (void)d3d_swap_ordinal;
  SetCpWhere(0);
}

void Runtime::OnGuestMemoryWritten(uint32_t physical_base, uint32_t length) {
  SetCpWhere(60);
  BackendGuard guard;
  SetCpWhere(61);
  // Director 2026-10-19 (B6-4 review SHOULD-FIX 3): main's draw-off path never fed these trace-playback writes into
  // the D3D watch; keep that behaviour (a change here needs its own card).
  (void)physical_base;
  (void)length;
  SetCpWhere(0);
}

void Runtime::BootFromWorker() {
  if (!NrDrawOff() || draw_off_init_.exchange(true)) return;
  REXLOG_INFO("[d3d-native-boot] backend started by the worker");
  BackendGuard guard;
  EnsureBackend(0);
}

void Runtime::OnCpuVisibility(uint32_t physical_base, uint32_t length) {
  SetCpWhere(40);
  if (NrDrawOff()) cpu_vis_wait_status_ = GateWaitPublished(1000, &cpu_vis_wait_ns_);
  SetCpWhere(41);
  BackendGuard guard;
  SetCpWhere(42);
  if (NrDrawOff()) {
    ++s3_cpu_vis_;
    const textures::CpuVisResult result = textures::WriteBackResolves(
        physical_base, length,
        [this](uint32_t start, const std::vector<uint8_t>& bytes) {
          GuestWrite(start, bytes);
          G18cReadback(start, bytes.data(), bytes.size());  // card G18 attire trace (lite by default, WWE2K14_D3D_G18C=0 off)
        });
    s3_cpu_vis_served_ += result.served;
    s3_cpu_vis_refused_ += result.refused.size();
    if (s3_cpu_vis_ <= 16) {
      REXLOG_INFO("[d3d-cpuvis] #{} {:08X}+{:X}: worker {} wait {:.2f} ms; entries {} served {} texels {} bytes {} "
                  "refused {}",
                  s3_cpu_vis_, physical_base, length, cpu_vis_wait_status_, cpu_vis_wait_ns_ / 1e6, result.entries,
                  result.served, result.texels, result.bytes, result.refused.size());
      for (size_t i = 0; i < result.refused.size() && i < 8; ++i) {
        REXLOG_WARN("[d3d-cpuvis]   refused {}", result.refused[i]);
      }
    }
  }
  SetCpWhere(0);
}

void Runtime::OnBeforeInterrupt(uint32_t) {
  SetCpWhere(50);
  OnBeforeGpuInterrupt();
  SetCpWhere(0);
}

void Runtime::OnCompareFrame(bool, uint32_t) {}

void Runtime::GuestWrite(uint32_t start, const std::vector<uint8_t>& bytes) {
  if (bytes.empty() || !memory_) return;
  std::memcpy(memory_->TranslatePhysical<uint8_t*>(start), bytes.data(), bytes.size());
  // Preserve the SDK guest-memory notification used by the shipped package; packet-only cache bookkeeping is retired.
  if (EnvIsOne("WWE2K14_NR_DIRECT_INVALIDATE")) {
    rex::graphics::NrInvalidateGuestRange(start, uint32_t(bytes.size()));
  }
}

void Runtime::MaybeShot(uint32_t snapshot, uint32_t width, uint32_t height, const uint8_t swizzle[4]) {
  static const int every = [] {
    const char* value = std::getenv("WWE2K14_NR_SHOT_EVERY");
    return value ? std::atoi(value) : 0;
  }();
  // WWE2K14_NR_SHOT_MAX (test harness, default 20): more shots for long hand-driven menu routes (card S5-2).
  static const int max_shots = [] {
    const char* value = std::getenv("WWE2K14_NR_SHOT_MAX");
    const int n = value ? std::atoi(value) : 0;
    return n > 0 ? n : 20;
  }();
  if (every <= 0 || shots_written_ >= max_shots) return;
  static const auto start = std::chrono::steady_clock::now();
  const auto now = std::chrono::steady_clock::now();
  const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start).count();
  if (elapsed < shot_next_s_) return;
  shot_next_s_ = elapsed + every;
  std::vector<uint8_t> pixels;
  std::string error;
  if (!nb::ReadSnapshot(snapshot, width, height, pixels, &error)) {
    REXLOG_INFO("[nr] shot at {} s failed: {}", elapsed, error);
    return;
  }
  std::vector<uint8_t> rgb(size_t(width) * height * 3);
  for (size_t i = 0; i < size_t(width) * height; ++i) {
    for (int c = 0; c < 3; ++c) {
      const uint8_t channel = swizzle[c];
      rgb[i * 3 + c] = channel <= 3 ? pixels[i * 4 + channel] : (channel == 5 ? 255 : 0);
    }
  }
  const std::filesystem::path out = std::getenv("WWE2K14_NR_OUT") ? std::getenv("WWE2K14_NR_OUT") : "nr-out";
  std::error_code ec;
  std::filesystem::create_directories(out, ec);
  char name[64];
  std::snprintf(name, sizeof(name), "shot-%05lld.ppm", static_cast<long long>(elapsed));
  if (FILE* file = std::fopen((out / name).string().c_str(), "wb")) {
    std::fprintf(file, "P6\n%u %u\n255\n", width, height);
    std::fwrite(rgb.data(), 1, rgb.size(), file);
    std::fclose(file);
    ++shots_written_;
    REXLOG_INFO("[nr] shot {} written ({}x{}, swap {})", name, width, height, s3_d3d_);
  }
}

void Runtime::PresentD3dOnly() {
  late_upload::Release(late_upload::kPresent);
  rex::graphics::SetNrExternalSwap(rex::graphics::NrExternalSwap{});
  rex::graphics::SetNrD3D12ExternalSwap(rex::graphics::NrD3D12ExternalSwap{});  // card C3
  uint32_t width = 0, height = 0;
  bool swap_rb = false;
  int32_t d3d_swizzle = -1;
  std::string flash_info;
  uint32_t game_frame = 0;
  const uint32_t snapshot = d3d_frame_ready_
                                ? TakePresentFrame(d3d_present_ordinal_ - 1, &width, &height, &swap_rb, &d3d_swizzle,
                                                   &flash_info, &game_frame)
                                : nb::kNone;
  bool ok = false;
  if (snapshot != nb::kNone && d3d_swizzle < 0) {
    ++s3_swz_missing_;
    nb::ReleaseSnapshot(snapshot);
  } else if (snapshot != nb::kNone) {
    uint8_t swizzle[4];
    for (int c = 0; c < 4; ++c) {
      uint8_t channel = uint8_t((uint32_t(d3d_swizzle) >> (3 * c)) & 7);
      if (swap_rb && (channel == 0 || channel == 2)) channel = uint8_t(2 - channel);
      swizzle[c] = channel;
    }
    MaybeShot(snapshot, width, height, swizzle);
    rr::MaybeLiveShot(snapshot, width, height, swizzle, game_frame);  // RR2: WWE2K14_RR_LIVE_OUT only
    std::string error;
    uint32_t physical_width = width, physical_height = height;
    const uint64_t view = nb::PresentViewOfSnapshot(snapshot, swizzle, &error, &physical_width, &physical_height);
    nb::ReleaseSnapshot(snapshot);
    if (!flash_info.empty()) {
      flash_info_[flash_presents_ % kFlashInfo] = flash_info;
      ++flash_presents_;
      uint64_t event;
      while (nb::FlashTakeEvent(&event)) {
        for (uint64_t i = event >= 3 ? event - 3 : 0; i < flash_presents_ && i <= event + 2; ++i) {
          if (i + kFlashInfo <= flash_presents_) continue;
          std::fprintf(stderr, "[nr-flash]   %s present=%llu %s\n", i == event ? ">>" : "  ",
                       static_cast<unsigned long long>(i), flash_info_[i % kFlashInfo].c_str());
        }
      }
    }
    nb::D3D12PresentInfo d3d12_present;
    if (view && nb::IsD3D12() && nb::TakeD3D12PresentInfo(&d3d12_present)) {
      // Card C3: the D3D12 presenter samples the resource with the swizzle as its SRV component mapping (no red/blue
      // flag) after a GPU wait for the plume submit that finished it.
      rex::graphics::NrD3D12ExternalSwap swap;
      swap.resource = reinterpret_cast<void*>(uintptr_t(view));
      swap.width = physical_width;
      swap.height = physical_height;
      swap.width_unscaled = width;
      swap.height_unscaled = height;
      swap.srv_format = d3d12_present.srv_format;
      swap.component_mapping = d3d12_present.component_mapping;
      swap.wait_fence = d3d12_present.fence;
      swap.wait_value = d3d12_present.fence_value;
      rex::graphics::SetNrD3D12ExternalSwap(swap);
      ok = true;
    } else if (view && !nb::IsD3D12()) {
      rex::graphics::NrExternalSwap swap;
      swap.image_view = view;
      swap.width = physical_width;
      swap.height = physical_height;
      swap.width_unscaled = width;
      swap.height_unscaled = height;
      const uint32_t swizzle_word = uint32_t(d3d_swizzle);
      auto fetch_swizzle = [&](int c) { return (swizzle_word >> (3 * c)) & 7; };
      swap.needs_rb_swap = fetch_swizzle(0) == 2 && fetch_swizzle(1) == 1 && fetch_swizzle(2) == 0;
      rex::graphics::SetNrExternalSwap(swap);
      ok = true;
    }
  }
  static const bool frame_time = EnvIsOne("WWE2K14_FRAME_TIME");
  if (frame_time) {
    static uint64_t last_us = 0, n = 0;
    const uint64_t now_us = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                                         std::chrono::steady_clock::now().time_since_epoch())
                                         .count());
    REXLOG_INFO("[nr-present] seq={} t_us={} pdt_us={} ok={}", n++, now_us, last_us ? now_us - last_us : 0,
                ok ? 1 : 0);
    last_us = now_us;
  }
  if (ok) ++s3_d3d_;
  else if (!d3d_frame_ready_) ++s3_fb_gate_;
  else ++s3_fb_missing_;
  if ((s3_d3d_ + s3_fb_gate_ + s3_fb_missing_) % 600 == 1) {
    REXLOG_INFO("[nr] draw-off present: d3d={} no_frame: gate={} missing_or_view={} | nr draws skipped={} cpu_reads={} "
                "(entries served {}, refused {}) swizzle missing={}",
                s3_d3d_, s3_fb_gate_, s3_fb_missing_, s3_off_draws_, s3_cpu_vis_, s3_cpu_vis_served_,
                s3_cpu_vis_refused_, s3_swz_missing_);
  }
}

}  // namespace wwe2k14::d3d::runtime
