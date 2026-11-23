// W3 investigation probe for the 2-axis predicates called by sub_82581918.
// Static evidence is in generated/wwe2k14_recomp.26.cpp:28086-28205 and
// 28543-28571. Their role as camera/crowd culling is not established.

#include "native/d3d/wide_core.h"
#include "native/d3d/capture.h"
#include "native/d3d/ultrawide_probe.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <limits>
#include <mutex>
#include <thread>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/memory.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

namespace {

bool WideProbeOn() { return wwe2k14::d3d::wide_probe::Enabled(); }

bool ClaimProbeLogSlot() {
  static std::atomic<uint64_t> last_log_ms{0};
  const uint64_t now_ms = uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::steady_clock::now().time_since_epoch())
                                       .count());
  uint64_t previous = last_log_ms.load(std::memory_order_relaxed);
  for (;;) {
    if (now_ms < previous || now_ms - previous < 250) return false;
    if (last_log_ms.compare_exchange_weak(previous, now_ms, std::memory_order_relaxed)) return true;
  }
}

void LogPredicate(uint8_t* base, const char* candidate, uint32_t return_pc, uint32_t object, uint32_t result) {
  if (!WideProbeOn() || !ClaimProbeLogSlot()) return;
  const uint8_t state = PPC_LOAD_U8(object + 70);
  const uint32_t x_bits = PPC_LOAD_U32(object + 160);
  const uint32_t y_bits = PPC_LOAD_U32(object + 168);
  REXLOG_INFO("[wide-probe] candidate={} return_pc=0x{:08X} object=0x{:08X} state70={} "
              "x_bits=0x{:08X} x={} y_bits=0x{:08X} y={} result={}",
              candidate, return_pc, object, state, x_bits, std::bit_cast<float>(x_bits), y_bits,
              std::bit_cast<float>(y_bits), result);
}

}  // namespace

namespace wwe2k14::d3d::wide_probe {
namespace {

constexpr uint32_t kFloatRegisterCount = 256;
constexpr uint32_t kFloatCount = kFloatRegisterCount * 4;
constexpr uint32_t kLoggedRegisterCount = 24;
constexpr size_t kShaderSampleCapacity = 64;
constexpr uint64_t kGuestScanDelayNs = 60'000'000'000ull;
constexpr size_t kAspectReaderCount = static_cast<size_t>(AspectReader::kCount);

struct ShaderSample {
  bool occupied = false;
  bool has_usage = false;
  uint64_t shader_key = 0;
  uint64_t draw_count = 0;
  uint32_t frame = 0;
  std::array<uint64_t, kFloatRegisterCount / 64> used{};
  std::array<uint32_t, kFloatCount> constants{};
};

std::array<ShaderSample, kShaderSampleCapacity> g_shader_samples{};
std::mutex g_shader_mutex;
std::chrono::steady_clock::time_point g_last_gpu_log{};
bool g_has_gpu_log = false;
std::array<std::atomic<uint64_t>, kAspectReaderCount> g_aspect_reader_counts{};
std::mutex g_aspect_reader_log_mutex;
std::chrono::steady_clock::time_point g_last_aspect_reader_log{};
bool g_has_aspect_reader_log = false;
std::atomic<uint64_t> g_first_stretched_ns{0};
std::atomic<bool> g_guest_scan_started{false};

struct ProjectionCandidate {
  bool found = false;
  uint32_t p00_register = 0;
  uint32_t p00_component = 0;
  uint32_t p11_register = 0;
  uint32_t p11_component = 0;
  uint32_t p00_bits = 0;
  uint32_t p11_bits = 0;
  double ratio = 0.0;
  double error = 0.0;
  uint32_t count = 0;
};

ProjectionCandidate g_best_projection{};
uint64_t g_best_projection_shader_key = 0;

bool RegisterUsed(const ShaderSample& sample, uint32_t reg) {
  return sample.has_usage && (sample.used[reg / 64] & (uint64_t(1) << (reg % 64))) != 0;
}

bool RegisterCandidateEligible(const ShaderSample& sample, uint32_t reg) {
  return !sample.has_usage || RegisterUsed(sample, reg);
}

ProjectionCandidate FindProjectionCandidate(const ShaderSample& sample) {
  ProjectionCandidate best{};
  best.error = std::numeric_limits<double>::infinity();
  constexpr double kExpectedAspect = 16.0 / 9.0;
  constexpr double kMaxRatioError = 0.02;
  auto consider = [&](uint32_t p00_reg, uint32_t p00_component, uint32_t p11_reg, uint32_t p11_component) {
    if (!RegisterCandidateEligible(sample, p00_reg) || !RegisterCandidateEligible(sample, p11_reg)) return;
    const uint32_t p00_bits = sample.constants[p00_reg * 4 + p00_component];
    const uint32_t p11_bits = sample.constants[p11_reg * 4 + p11_component];
    const float p00 = std::bit_cast<float>(p00_bits);
    const float p11 = std::bit_cast<float>(p11_bits);
    if (!std::isfinite(p00) || !std::isfinite(p11) || p00 <= 0.0f || p11 <= 0.0f) return;
    const double ratio = double(p11) / double(p00);
    const double error = std::abs(ratio - kExpectedAspect);
    if (error > kMaxRatioError) return;
    ++best.count;
    if (error < best.error) {
      best.found = true;
      best.p00_register = p00_reg;
      best.p00_component = p00_component;
      best.p11_register = p11_reg;
      best.p11_component = p11_component;
      best.p00_bits = p00_bits;
      best.p11_bits = p11_bits;
      best.ratio = ratio;
      best.error = error;
    }
  };

  // Matrix diagonal candidates in adjacent float4 registers, in both row/column transposes.
  for (uint32_t reg = 0; reg + 1 < kFloatRegisterCount; ++reg) {
    consider(reg, 0, reg + 1, 1);
    consider(reg, 1, reg + 1, 0);
  }
  return best;
}

ProjectionCandidate LogGpuSample(const ShaderSample& sample) {
  REXLOG_INFO("[wide-probe] gpu_vs shader_key=0x{:016X} scene_draws={} frame={} constant_registers=c0..c{}",
              sample.shader_key, sample.draw_count, sample.frame, kLoggedRegisterCount - 1);
  for (uint32_t reg = 0; reg < kLoggedRegisterCount; ++reg) {
    const uint32_t x = sample.constants[reg * 4 + 0];
    const uint32_t y = sample.constants[reg * 4 + 1];
    const uint32_t z = sample.constants[reg * 4 + 2];
    const uint32_t w = sample.constants[reg * 4 + 3];
    char line[420];
    std::snprintf(line, sizeof(line),
                  "[wide-probe] gpu_vs shader_key=0x%016llX c%02u used=%s bits=[%08X,%08X,%08X,%08X] "
                  "floats=[%.9g,%.9g,%.9g,%.9g]",
                  static_cast<unsigned long long>(sample.shader_key), reg,
                  !sample.has_usage ? "unknown" : (RegisterUsed(sample, reg) ? "used" : "unused"),
                  x, y, z, w, double(std::bit_cast<float>(x)), double(std::bit_cast<float>(y)),
                  double(std::bit_cast<float>(z)), double(std::bit_cast<float>(w)));
    REXLOG_INFO("{}", line);
  }

  const ProjectionCandidate candidate = FindProjectionCandidate(sample);
  if (!candidate.found) {
    REXLOG_INFO("[wide-probe] gpu_projection_pair shader_key=0x{:016X} candidates=0 searched_registers=256",
                sample.shader_key);
    return candidate;
  }
  REXLOG_INFO("[wide-probe] gpu_projection_pair shader_key=0x{:016X} candidates={} "
              "p00=c{}.{} bits=0x{:08X} value={} p11=c{}.{} bits=0x{:08X} value={} ratio={}",
              sample.shader_key, candidate.count, candidate.p00_register,
              candidate.p00_component == 0 ? "x" : "y", candidate.p00_bits,
              std::bit_cast<float>(candidate.p00_bits), candidate.p11_register,
              candidate.p11_component == 0 ? "x" : "y", candidate.p11_bits,
              std::bit_cast<float>(candidate.p11_bits), candidate.ratio);
  return candidate;
}

ShaderSample* FindSampleSlot(uint64_t shader_key) {
  ShaderSample* empty = nullptr;
  ShaderSample* least_used = &g_shader_samples[0];
  for (ShaderSample& sample : g_shader_samples) {
    if (sample.occupied && sample.shader_key == shader_key) return &sample;
    if (!sample.occupied && !empty) empty = &sample;
    if (sample.draw_count < least_used->draw_count) least_used = &sample;
  }
  if (empty) return empty;
  *least_used = {};
  return least_used;
}

uint32_t LoadGuestBe32(const uint8_t* guest_base, uint32_t address) {
  const uint8_t* p = guest_base + address;
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

struct ScanPattern {
  const char* name = nullptr;
  uint32_t bits = 0;
  uint32_t logged = 0;
  bool capped = false;
};

void LogGuestHit(const uint8_t* guest_base, const ScanPattern& pattern, uint32_t address,
                 uint64_t readable_begin, uint64_t readable_end) {
  char window[512] = {};
  size_t used = 0;
  for (int delta = -4; delta <= 4; ++delta) {
    const int64_t neighbor = int64_t(address) + int64_t(delta) * 4;
    char item[64];
    if (neighbor >= int64_t(readable_begin) && uint64_t(neighbor) + 4 <= readable_end &&
        GuestRangeOk(uint32_t(neighbor), 4)) {
      const uint32_t bits = LoadGuestBe32(guest_base, uint32_t(neighbor));
      std::snprintf(item, sizeof(item), "%s%+d:0x%08X/%.8g", delta ? "," : "", delta, bits,
                    double(std::bit_cast<float>(bits)));
    } else {
      std::snprintf(item, sizeof(item), "%s%+d:--", delta ? "," : "", delta);
    }
    const size_t remaining = sizeof(window) - used;
    const int written = std::snprintf(window + used, remaining, "%s", item);
    if (written <= 0 || size_t(written) >= remaining) break;
    used += size_t(written);
  }
  REXLOG_INFO("[wide-probe] guest_hit pattern={} bits=0x{:08X} address=0x{:08X} neighbors={}",
              pattern.name, pattern.bits, address, window);
}

void ScanGuestMemory(rex::memory::Memory* memory, const uint8_t* guest_base, uint64_t shader_key,
                     std::array<ScanPattern, 5> patterns, size_t pattern_count,
                     uint32_t p00_bits, uint32_t p11_bits, uint32_t reciprocal_bits) {
  REXLOG_INFO("[wide-probe] guest_scan_start range=virtual:0x00001000..0x9FFFFFFF readable=committed+read "
              "pattern_count={} projection_shader_key=0x{:016X} p00_bits=0x{:08X} p11_bits=0x{:08X} "
              "reciprocal_p11_bits=0x{:08X}",
              pattern_count, shader_key, p00_bits, p11_bits, reciprocal_bits);
  uint64_t readable_bytes = 0;
  uint64_t readable_regions = 0;
  uint64_t cursor = 0x1000;
  constexpr uint64_t kScanEnd = 0xA0000000ull;

  while (cursor < kScanEnd) {
    auto* heap = memory->LookupHeap(uint32_t(cursor));
    if (!heap) {
      cursor = std::min(cursor + 0x1000, kScanEnd);
      continue;
    }
    const uint64_t heap_begin = heap->heap_base();
    const uint64_t heap_end = std::min<uint64_t>(heap_begin + heap->heap_size(), kScanEnd);
    if (!heap->page_size() || cursor < heap_begin || cursor >= heap_end) {
      cursor = std::min(cursor + 0x1000, kScanEnd);
      continue;
    }

    const uint64_t page_size = heap->page_size();
    const uint64_t page_begin = heap_begin + ((cursor - heap_begin) / page_size) * page_size;
    rex::memory::HeapAllocationInfo info{};
    if (!heap->QueryRegionInfo(uint32_t(cursor), &info) || !info.region_size) {
      cursor = std::min(page_begin + page_size, heap_end);
      continue;
    }
    const uint64_t region_end = std::min<uint64_t>(page_begin + info.region_size, heap_end);
    if (region_end <= cursor) {
      cursor = std::min(page_begin + page_size, heap_end);
      continue;
    }

    const bool committed = (info.state & rex::memory::kMemoryAllocationCommit) != 0;
    const bool readable = (info.protect & rex::memory::kMemoryProtectRead) != 0;
    if (committed && readable) {
      const uint64_t readable_begin = std::max(cursor, page_begin);
      const uint64_t readable_end = region_end;
      const uint64_t aligned_begin = (readable_begin + 3) & ~uint64_t(3);
      if (readable_end > aligned_begin && GuestRangeOk(uint32_t(aligned_begin), readable_end - aligned_begin)) {
        ++readable_regions;
        readable_bytes += readable_end - aligned_begin;
        for (uint64_t address = aligned_begin; address + 4 <= readable_end; address += 4) {
          const uint32_t bits = LoadGuestBe32(guest_base, uint32_t(address));
          for (size_t i = 0; i < pattern_count; ++i) {
            ScanPattern& pattern = patterns[i];
            if (bits != pattern.bits) continue;
            if (pattern.logged < 64) {
              LogGuestHit(guest_base, pattern, uint32_t(address), aligned_begin, readable_end);
              ++pattern.logged;
            } else {
              pattern.capped = true;
            }
          }
        }
      }
    }
    cursor = region_end;
  }

  REXLOG_INFO("[wide-probe] guest_scan_done readable_regions={} readable_bytes={}", readable_regions,
              readable_bytes);
  for (size_t i = 0; i < pattern_count; ++i) {
    REXLOG_INFO("[wide-probe] guest_scan_pattern name={} bits=0x{:08X} logged_hits={} cap_reached={}",
                patterns[i].name, patterns[i].bits, patterns[i].logged, patterns[i].capped ? 1 : 0);
  }
}

void StartGuestScan(uint64_t shader_key, const ProjectionCandidate& candidate) {
  const uint32_t p00_bits = candidate.found ? candidate.p00_bits : 0;
  const uint32_t p11_bits = candidate.found ? candidate.p11_bits : 0;
  const float p11 = candidate.found ? std::bit_cast<float>(candidate.p11_bits) : 0.0f;
  const uint32_t reciprocal_bits = candidate.found && p11 != 0.0f
                                       ? std::bit_cast<uint32_t>(1.0f / p11)
                                       : 0;
  std::array<ScanPattern, 5> patterns{};
  size_t pattern_count = 0;
  patterns[pattern_count++] = {"aspect_16_9", 0x3FE38E39u};
  patterns[pattern_count++] = {"aspect_9_16", 0x3F100000u};
  if (candidate.found) {
    patterns[pattern_count++] = {"projection_P00", p00_bits};
    patterns[pattern_count++] = {"projection_P11", p11_bits};
    patterns[pattern_count++] = {"reciprocal_P11", reciprocal_bits};
  }

  auto* kernel_state = rex::system::kernel_state();
  rex::memory::Memory* memory = kernel_state ? kernel_state->memory() : nullptr;
  const uint8_t* guest_base = ShadowGuestBase();
  if (!memory || !guest_base) {
    REXLOG_INFO("[wide-probe] guest_scan_unavailable memory={} guest_base={}", memory ? 1 : 0,
                guest_base ? 1 : 0);
    return;
  }
  try {
    std::thread(ScanGuestMemory, memory, guest_base, shader_key, patterns, pattern_count, p00_bits, p11_bits,
                reciprocal_bits).detach();
  } catch (const std::exception& e) {
    REXLOG_INFO("[wide-probe] guest_scan_unavailable thread_start={}", e.what());
  }
}

}  // namespace

void NoteStretchedFrame() {
  const uint64_t now_ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       std::chrono::steady_clock::now().time_since_epoch())
                                       .count());
  uint64_t unset = 0;
  g_first_stretched_ns.compare_exchange_strong(unset, now_ns, std::memory_order_release,
                                                std::memory_order_relaxed);
}

void CountAspectReader(AspectReader reader) {
  const size_t index = static_cast<size_t>(reader);
  if (index < g_aspect_reader_counts.size()) g_aspect_reader_counts[index].fetch_add(1, std::memory_order_relaxed);
}

void LogAspectReaderCounts() {
  if (!Enabled()) return;
  std::lock_guard<std::mutex> lock(g_aspect_reader_log_mutex);
  const auto now = std::chrono::steady_clock::now();
  if (!g_has_aspect_reader_log) {
    g_last_aspect_reader_log = now;
    g_has_aspect_reader_log = true;
    return;
  }
  if (now - g_last_aspect_reader_log < std::chrono::seconds(2)) return;
  const auto window_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_last_aspect_reader_log).count();
  g_last_aspect_reader_log = now;
  std::array<uint64_t, kAspectReaderCount> counts{};
  for (size_t i = 0; i < counts.size(); ++i) {
    counts[i] = g_aspect_reader_counts[i].exchange(0, std::memory_order_relaxed);
  }
  REXLOG_INFO("[wide-probe] aspect_readers window_ms={} sub_8276B670={} sub_827E9520={} sub_827E9AD8={} "
              "sub_827E9E40={} sub_827EB9B8={} sub_82BB6F78={} sub_828469B0={}",
              window_ms, counts[0], counts[1], counts[2], counts[3], counts[4], counts[5], counts[6]);
}

void ObserveSceneDraw(uint32_t frame, uint64_t shader_key, const uint8_t* constants,
                      const uint64_t* used_registers) {
  if (!constants) return;

  const auto now = std::chrono::steady_clock::now();
  const uint64_t now_ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count());
  const uint64_t stretched_ns = g_first_stretched_ns.load(std::memory_order_acquire);
  const bool scan_due = stretched_ns && now_ns >= stretched_ns && now_ns - stretched_ns >= kGuestScanDelayNs;
  bool log_due = false;
  ShaderSample selected{};

  {
    std::lock_guard<std::mutex> lock(g_shader_mutex);
    ShaderSample* sample = FindSampleSlot(shader_key);
    if (!sample->occupied || sample->shader_key != shader_key) {
      *sample = {};
      sample->occupied = true;
      sample->shader_key = shader_key;
    }
    ++sample->draw_count;
    sample->frame = frame;
    sample->has_usage = used_registers != nullptr;
    if (used_registers) std::memcpy(sample->used.data(), used_registers, sizeof(sample->used));
    else sample->used.fill(0);
    std::memcpy(sample->constants.data(), constants, sizeof(sample->constants));

    log_due = !g_has_gpu_log || now - g_last_gpu_log >= std::chrono::seconds(2);
    if (log_due) {
      g_has_gpu_log = true;
      g_last_gpu_log = now;
    }
    if (log_due || scan_due) {
      const ShaderSample* most_common = nullptr;
      for (const ShaderSample& candidate : g_shader_samples) {
        if (candidate.occupied && (!most_common || candidate.draw_count > most_common->draw_count)) {
          most_common = &candidate;
        }
      }
      if (most_common) selected = *most_common;
    }
  }

  if (log_due && selected.occupied) {
    const ProjectionCandidate candidate = LogGpuSample(selected);
    if (candidate.found) {
      std::lock_guard<std::mutex> lock(g_shader_mutex);
      if (!g_best_projection.found || candidate.error < g_best_projection.error) {
        g_best_projection = candidate;
        g_best_projection_shader_key = selected.shader_key;
      }
    }
  }
  if (scan_due) {
    bool not_started = false;
    if (g_guest_scan_started.compare_exchange_strong(not_started, true, std::memory_order_acq_rel,
                                                      std::memory_order_relaxed)) {
      ProjectionCandidate candidate{};
      uint64_t shader_key = 0;
      {
        std::lock_guard<std::mutex> lock(g_shader_mutex);
        candidate = g_best_projection;
        shader_key = g_best_projection_shader_key;
      }
      StartGuestScan(shader_key, candidate);
    }
  }
}

}  // namespace wwe2k14::d3d::wide_probe

extern "C" PPC_FUNC(__imp__sub_82581860);
extern "C" PPC_FUNC(sub_82581860) {
  const uint32_t return_pc = static_cast<uint32_t>(ctx.lr);
  const uint32_t object = ctx.r3.u32;
  __imp__sub_82581860(ctx, base);
  LogPredicate(base, "sub_82581860", return_pc, object, ctx.r3.u32);
}

extern "C" PPC_FUNC(__imp__sub_825818C0);
extern "C" PPC_FUNC(sub_825818C0) {
  const uint32_t return_pc = static_cast<uint32_t>(ctx.lr);
  const uint32_t object = ctx.r3.u32;
  __imp__sub_825818C0(ctx, base);
  LogPredicate(base, "sub_825818C0", return_pc, object, ctx.r3.u32);
}
