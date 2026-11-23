#include "v17c_ab.h"

#include <atomic>
#include <cstdlib>
#include <cstring>

#include <rex/logging.h>

namespace wwe2k14::d3d::v17c_ab {
namespace {
struct RuntimeConfig {
  uint32_t block_frames = 0;
  bool enabled = false;
};

const RuntimeConfig& Config() {
  static const RuntimeConfig config = [] {
    RuntimeConfig out;
    const char* schedule_text = std::getenv("WWE2K14_D3D_V17C_AB");
    bool invalid = false;
    out.block_frames = schedule::ParseBlockFrames(schedule_text, &invalid);
    if (invalid) {
      REXLOG_WARN("[d3d-v17c-ab] invalid WWE2K14_D3D_V17C_AB='{}' (expected 1..1000000); schedule disabled",
                  schedule_text ? schedule_text : "");
      out.block_frames = 0;
      return out;
    }
    if (!out.block_frames) return out;

    const char* dtr = std::getenv("WWE2K14_D3D_DIRTY_TRANSLATE");
    const char* vb = std::getenv("WWE2K14_NR_VB_DIRECT");
    const char* rec = std::getenv("WWE2K14_NR_REC_REUSE");
    if (!dtr || std::strcmp(dtr, "1") != 0 || !vb || std::strcmp(vb, "1") != 0 || !rec || std::strcmp(rec, "1") != 0) {
      REXLOG_WARN("[d3d-v17c-ab] WWE2K14_D3D_V17C_AB requires WWE2K14_D3D_DIRTY_TRANSLATE=1, "
                  "WWE2K14_NR_VB_DIRECT=1, WWE2K14_NR_REC_REUSE=1; schedule disabled");
      out.block_frames = 0;
      return out;
    }
    out.enabled = true;
    return out;
  }();
  return config;
}
}  // namespace

bool Enabled() { return Config().enabled; }

int ModeForFrame(uint32_t frame) {
  const RuntimeConfig& config = Config();
  if (!config.enabled) return -1;
  const uint32_t block = frame / config.block_frames;
  static std::atomic<uint32_t> last_block{UINT32_MAX};
  uint32_t previous = last_block.load(std::memory_order_relaxed);
  while (previous != block && !last_block.compare_exchange_weak(previous, block, std::memory_order_relaxed)) {}
  if (previous != block) {
    const uint64_t start = uint64_t(block) * config.block_frames;
    const uint64_t end = start + config.block_frames;
    const int mode = schedule::ModeForFrame(config.block_frames, frame);
    REXLOG_INFO("[d3d-v17c-ab] block={} frames=[{}, {}) mode={} ({})", block, start, end, mode,
                mode ? "DTR+VB_DIRECT+REC_REUSE" : "legacy");
  }
  return schedule::ModeForFrame(config.block_frames, frame);
}

}  // namespace wwe2k14::d3d::v17c_ab
