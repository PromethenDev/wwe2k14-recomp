#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

namespace wwe2k14::d3d::seed_lazy {

enum class Observer : uint8_t { kNone, kUncoveredBind, kWriteBack, kPropagation };
enum class Action : uint8_t { kNoPending, kSkip, kRefuseDirtyBind, kMaterialize };

// Dirty binds retain textures.cpp's current write-watch refusal. Write-back emits only resolve-covered cells, so it never
// observes a seeded texel. A pending seed always reads the guest's current bytes, the only writer of those texels. So it can
// differ from an eager seed by being newer: on dirty propagation (counted), and after in-place reuse or a watch rebaseline
// took a fresh baseline (not counted). In CHECK mode that second case can log a mismatch that is not a fault: read the frame.
constexpr Action Decide(bool pending, bool watch_clean, Observer observer) {
  if (!pending) return Action::kNoPending;
  if (observer == Observer::kNone || observer == Observer::kWriteBack) return Action::kSkip;
  if (observer == Observer::kUncoveredBind && !watch_clean) return Action::kRefuseDirtyBind;
  return Action::kMaterialize;
}

struct Rect {
  uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};

// One bit per destination pixel. The existing resolve coverage map is deliberately coarser (8x8 cells); this map
// protects partial edge texels from being overwritten when the seed is eventually copied into the live canvas.
class PixelCoverage {
 public:
  PixelCoverage() = default;
  PixelCoverage(uint32_t width, uint32_t height) { Reset(width, height); }

  void Reset(uint32_t width, uint32_t height) {
    width_ = width;
    height_ = height;
    const uint64_t pixels = uint64_t(width) * height;
    bits_.assign(size_t((pixels + 63) / 64), 0);
  }

  uint32_t width() const { return width_; }
  uint32_t height() const { return height_; }
  uint64_t storage_bytes() const { return uint64_t(bits_.size()) * sizeof(uint64_t); }

  bool Covered(uint32_t x, uint32_t y) const {
    if (x >= width_ || y >= height_) return false;
    const uint64_t bit = uint64_t(y) * width_ + x;
    return (bits_[size_t(bit >> 6)] & (uint64_t(1) << (bit & 63))) != 0;
  }

  bool Covers(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1) const {
    if (x1 > width_ || y1 > height_ || x0 > x1 || y0 > y1) return false;
    for (uint32_t y = y0; y < y1; ++y)
      for (uint32_t x = x0; x < x1; ++x)
        if (!Covered(x, y)) return false;
    return true;
  }

  void MarkRect(int32_t rx0, int32_t ry0, int32_t rx1, int32_t ry1) {
    const uint32_t x0 = uint32_t(std::clamp<int32_t>(rx0, 0, int32_t(width_)));
    const uint32_t y0 = uint32_t(std::clamp<int32_t>(ry0, 0, int32_t(height_)));
    const uint32_t x1 = uint32_t(std::clamp<int32_t>(rx1, 0, int32_t(width_)));
    const uint32_t y1 = uint32_t(std::clamp<int32_t>(ry1, 0, int32_t(height_)));
    if (x0 >= x1 || y0 >= y1) return;
    for (uint32_t y = y0; y < y1; ++y) SetRange(uint64_t(y) * width_ + x0, uint64_t(y) * width_ + x1);
  }

  void CopyClippedFrom(const PixelCoverage& src) {
    const uint32_t w = std::min(width_, src.width_);
    const uint32_t h = std::min(height_, src.height_);
    for (uint32_t y = 0; y < h; ++y) {
      uint32_t x = 0;
      while (x < w) {
        while (x < w && !src.Covered(x, y)) ++x;
        const uint32_t x0 = x;
        while (x < w && src.Covered(x, y)) ++x;
        if (x0 < x) SetRange(uint64_t(y) * width_ + x0, uint64_t(y) * width_ + x);
      }
    }
  }

  // Return a small rect list for copying the seed around resolved pixels. Runs with identical x extents on adjacent
  // rows merge vertically. false means the caller should use its correctness fallback instead of an unbounded list.
  bool UncoveredRects(std::vector<Rect>* out, size_t max_rects = 65536) const {
    if (!out) return false;
    out->clear();
    using Span = std::pair<uint32_t, uint32_t>;
    std::map<Span, Rect> active;
    for (uint32_t y = 0; y < height_; ++y) {
      std::vector<Span> spans;
      for (uint32_t x = 0; x < width_;) {
        while (x < width_ && Covered(x, y)) ++x;
        const uint32_t x0 = x;
        while (x < width_ && !Covered(x, y)) ++x;
        if (x0 < x) spans.emplace_back(x0, x);
      }
      std::map<Span, Rect> next;
      for (const Span& span : spans) {
        auto it = active.find(span);
        Rect r = it == active.end() ? Rect{span.first, y, span.second, y + 1} : it->second;
        if (it != active.end()) r.y1 = y + 1;
        next.emplace(span, r);
      }
      for (const auto& kv : active) {
        if (next.find(kv.first) == next.end()) {
          out->push_back(kv.second);
          if (out->size() > max_rects) return false;
        }
      }
      active = std::move(next);
      if (out->size() + active.size() > max_rects) return false;
    }
    for (const auto& kv : active) {
      out->push_back(kv.second);
      if (out->size() > max_rects) return false;
    }
    return true;
  }

 private:
  void SetRange(uint64_t first, uint64_t last) {
    while (first < last) {
      const size_t word = size_t(first >> 6);
      const uint32_t offset = uint32_t(first & 63);
      const uint32_t count = uint32_t(std::min<uint64_t>(last - first, 64 - offset));
      const uint64_t mask = count == 64 ? ~uint64_t(0) : ((uint64_t(1) << count) - 1) << offset;
      bits_[word] |= mask;
      first += count;
    }
  }

  uint32_t width_ = 0, height_ = 0;
  std::vector<uint64_t> bits_;
};

}  // namespace wwe2k14::d3d::seed_lazy
