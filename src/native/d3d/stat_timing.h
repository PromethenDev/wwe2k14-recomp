// C1-T3: sample the diagnostic stopwatch timers on 1 frame in N (default 8) instead of every frame.
// The clock reads gated by this are statistics-only (periodic log lines); nothing here feeds behaviour,
// watchdogs, waits or deadlines. Pure functions, host-testable; the env vars are read once (static locals).
//
//   WWE2K14_D3D_TIMING_EVERY  1..1024, default 8 (1 = old behaviour: time every frame)
//   WWE2K14_D3D_TIMING_AB     0 = off; else block length in frames. In-run A/B: even blocks K use Every()=1,
//                             odd blocks use the configured Every(). Each block switch is logged once.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <rex/logging.h>

namespace wwe2k14::d3d::stat {

// Read once. 1 = old behaviour (every frame).
inline uint32_t Every() {
  static const uint32_t v = [] {
    const char* e = std::getenv("WWE2K14_D3D_TIMING_EVERY");
    uint32_t n = 8;
    if (e && *e) {
      const int x = std::atoi(e);
      if (x >= 1 && x <= 1024) n = uint32_t(x);
    }
    return n;
  }();
  return v;
}

// Read once. 0 = A/B off.
inline uint32_t AbBlock() {
  static const uint32_t v = [] {
    const char* e = std::getenv("WWE2K14_D3D_TIMING_AB");
    uint32_t n = 0;
    if (e && *e) {
      const int x = std::atoi(e);
      if (x > 0) n = uint32_t(x);
    }
    return n;
  }();
  return v;
}

// Effective N for the frame: in A/B mode even blocks use 1, odd blocks use Every(); otherwise Every().
inline uint32_t EffectiveEvery(uint32_t frame) {
  const uint32_t b = AbBlock();
  if (!b) return Every();
  return (frame / b) % 2 == 0 ? 1u : Every();
}

// True when this frame is a timing frame. Latch this once at the start of a measured scope: a scope may
// start on one frame and end on the next; the latched bool keeps both ends on the same decision.
inline bool On(uint32_t frame) {
  return frame % EffectiveEvery(frame) == 0;
}

// The effective N for the frame: multiply statistics accumulated only on timing frames by it when
// converting them back to per-frame values.
inline uint32_t Scale(uint32_t frame) {
  return EffectiveEvery(frame);
}

// Log each A/B block switch once: "[d3d-timing-ab] block=K frames=[a, b) every=E".
inline void NoteBlock(uint32_t frame) {
  const uint32_t b = AbBlock();
  if (!b) return;
  const uint32_t k = frame / b;
  static uint32_t last = 0;
  if (k == last) return;
  last = k;
  REXLOG_INFO("[d3d-timing-ab] block={} frames=[{}, {}) every={}", k, uint64_t(k) * b, uint64_t(k + 1) * b,
              EffectiveEvery(uint64_t(k) * b));
}

}  // namespace wwe2k14::d3d::stat
