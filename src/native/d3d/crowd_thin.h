// Optional far-crowd thinning (WWE2K14_D3D_CROWD_THIN=1, default off; lowers the picture, so opt-in only). Pure logic, no GPU
// calls, shared by draw.cpp and the host test src/native/tests/crowd-thin-test.cpp.
//
// Measured (thin-l1, arena 39): the perspective crowd is VS EB1F; its member position c76-c78 .w is exact and static frame to
// frame, order too, seats about 100 units apart. VS 9F55 with the same PS is an orthographic side pass with animated bones (39 draws
// every frame, also in menus): the caller never offers it, so that pass stays whole.
// Far = the game's own lowest crowd mesh: rd-o1 main pass, index counts 209-249 far, 709-825 middle, 1349-1533 near.
// Every other seat by position is selected. No seat ever disappears on screen:
// - a selected far seat is left out only from the moment it comes into view, or from a camera cut;
// - one that the game shows on a bigger mesh is drawn again and stays drawn until the next cut or until it left the view.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace wwe2k14::d3d::crowd_thin {

inline constexpr uint64_t kPerspectiveCrowdVs = 0xEB1F2E0E8FA90E1Full;
inline constexpr uint32_t kFarMinIndices = 200, kFarMaxIndices = 300, kMiddleMaxIndices = 1000;
inline constexpr float kSelectGrid = 120.f;    // about the seat spacing: every other seat, spread evenly (thin-l1 data)
inline constexpr uint32_t kViewGapFrames = 3;  // unseen longer than this: it left the view
inline constexpr float kCutRatio = 0.08f;      // change of the view-projection between frames, as a share of its size

struct Stats {
  uint64_t near_members = 0, middle = 0, far = 0, dropped = 0, new_seats = 0, returned = 0, cuts = 0, resets = 0, frames = 0, other = 0, redropped = 0;
};

class Thinner {
 public:
  explicit Thinner(float match_radius = 1.0f, float cell = 128.f) : radius_(match_radius), cell_(cell) {
    heads_.assign(kBuckets, -1);
    seats_.reserve(4096);
  }

  // One crowd draw. `position` = member root in world space, `view_proj` = 16 floats. True = leave this member out.
  bool Offer(uint32_t frame, uint32_t index_count, const float position[3], const float view_proj[16]) {
    if (frame != frame_ || !have_frame_) BeginFrame(frame, view_proj);
    if (index_count >= kMiddleMaxIndices) {
      ++stats_.near_members;
      return false;
    }
    if (index_count < kFarMinIndices) {
      ++stats_.other;
      return false;
    }
    const bool far_mesh = index_count < kFarMaxIndices;
    Seat& seat = Find(position);
    if (!far_mesh) {
      ++stats_.middle;
      if (seat.drop) ++stats_.returned;
      seat.drop = false;
      seat.seen = true;
      seat.last_seen = frame;
      seat.epoch = epoch_;
      return false;
    }
    ++stats_.far;
    const bool in_view = seat.seen && frame - seat.last_seen <= kViewGapFrames && seat.epoch == epoch_;
    if (!in_view) {  // not on screen a moment ago (first sight, back in view) or a new shot
      if (seat.seen && !seat.drop && seat.selected) ++stats_.redropped;
      seat.drop = seat.selected;
    }
    seat.seen = true;
    seat.last_seen = frame;
    seat.epoch = epoch_;
    if (seat.drop) ++stats_.dropped;
    return seat.drop;
  }

  void Reset() {  // switch turned on again: a fresh decision for everyone
    ++epoch_;
  }
  const Stats& stats() const { return stats_; }
  void ClearStats() { stats_ = Stats{}; }
  size_t seat_count() const { return seats_.size(); }

 private:
  static constexpr uint32_t kBuckets = 1u << 14, kMaxSeats = 1u << 14;
  struct Seat {
    float pos[3];
    int32_t next;
    uint32_t last_seen, epoch;
    bool selected, drop, seen;
  };

  void BeginFrame(uint32_t frame, const float view_proj[16]) {
    frame_ = frame;
    ++stats_.frames;
    if (have_frame_) {
      float diff = 0.f, size = 0.f;
      for (int i = 0; i < 16; ++i) {
        diff += std::fabs(view_proj[i] - view_[i]);
        size += std::fabs(view_[i]);
      }
      if (diff > kCutRatio * size) {
        ++epoch_;
        ++stats_.cuts;
      }
    }
    std::memcpy(view_, view_proj, sizeof(view_));
    have_frame_ = true;
  }
  static bool Selected(const float p[3]) {  // the same seats every time, whatever the order they are first seen in
    const auto cell = [](float v) { return int32_t(std::lround(v / kSelectGrid)); };
    return ((cell(p[0]) + cell(p[1]) + cell(p[2])) & 1) != 0;
  }
  static uint32_t Bucket(int32_t x, int32_t y, int32_t z) {
    const uint32_t h = (uint32_t(x) * 0x8DA6B343u) ^ (uint32_t(y) * 0xD8163841u) ^ (uint32_t(z) * 0xCB1AB31Fu);
    return (h ^ (h >> 15)) & (kBuckets - 1);
  }
  bool Close(const Seat& s, const float p[3]) const {
    return std::fabs(s.pos[0] - p[0]) <= radius_ && std::fabs(s.pos[1] - p[1]) <= radius_ && std::fabs(s.pos[2] - p[2]) <= radius_;
  }
  int32_t Search(int32_t x, int32_t y, int32_t z, const float p[3]) const {
    for (int32_t i = heads_[Bucket(x, y, z)]; i >= 0; i = seats_[size_t(i)].next)
      if (Close(seats_[size_t(i)], p)) return i;
    return -1;
  }
  Seat& Find(const float p[3]) {
    int32_t c[3], lo[3], hi[3];
    for (int k = 0; k < 3; ++k) {
      c[k] = int32_t(std::floor(p[k] / cell_));
      lo[k] = int32_t(std::floor((p[k] - radius_) / cell_));
      hi[k] = int32_t(std::floor((p[k] + radius_) / cell_));
    }
    int32_t found = Search(c[0], c[1], c[2], p);
    for (int32_t x = lo[0]; found < 0 && x <= hi[0]; ++x)
      for (int32_t y = lo[1]; found < 0 && y <= hi[1]; ++y)
        for (int32_t z = lo[2]; found < 0 && z <= hi[2]; ++z)
          if (x != c[0] || y != c[1] || z != c[2]) found = Search(x, y, z, p);
    if (found >= 0) return seats_[size_t(found)];
    if (seats_.size() >= kMaxSeats) {  // far more than one arena holds: forget everything (next arena)
      seats_.clear();
      heads_.assign(kBuckets, -1);
      ++stats_.resets;
    }
    ++stats_.new_seats;
    const uint32_t b = Bucket(c[0], c[1], c[2]);
    Seat s{};
    std::memcpy(s.pos, p, sizeof(s.pos));
    s.next = heads_[b];
    s.selected = Selected(p);
    heads_[b] = int32_t(seats_.size());
    seats_.push_back(s);
    return seats_.back();
  }

  float radius_, cell_;
  std::vector<int32_t> heads_;
  std::vector<Seat> seats_;
  float view_[16] = {};
  uint32_t frame_ = 0, epoch_ = 1;
  bool have_frame_ = false;
  Stats stats_;
};

}  // namespace wwe2k14::d3d::crowd_thin
