// Host test of the far-crowd thinning decisions (d3d/crowd_thin.h). Build: g++ -std=c++20 -Wall -Wextra -I src src/native/tests/crowd-thin-test.cpp
#include <cstdio>
#include <vector>

#include "native/d3d/crowd_thin.h"

using namespace wwe2k14::d3d::crowd_thin;

static int failures = 0;
static void check(bool ok, const char* what) {
  if (!ok) {
    std::printf("FAIL %s\n", what);
    ++failures;
  }
}
static const float kView[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 1, 10};
static const float kOtherView[16] = {0, 1, 0, 3, 1, 0, 0, 9, 0, 0, 1, 2, 1, 0, 0, 20};

int main() {
  constexpr uint32_t kFar = 225, kMiddle = 770, kNear = 1430;
  const auto seat = [](int i) {
    return std::vector<float>{float(i) * 120.0f, 5.0f, 100.0f};  // neighbours on the select grid
  };
  {  // Near and middle are never left out; far about half, the same ones every frame.
    Thinner t;
    std::vector<bool> first;
    for (int i = 0; i < 100; ++i) {
      check(!t.Offer(1, kNear, seat(i + 1000).data(), kView), "near stays");
      check(!t.Offer(1, kMiddle, seat(i + 2000).data(), kView), "middle stays");
      first.push_back(t.Offer(1, kFar, seat(i).data(), kView));
    }
    int out = 0;
    for (bool b : first) out += b;
    check(out == 50 || out == 51 || out == 49, "half of far");
    for (uint32_t f = 2; f < 40; ++f)
      for (int i = 0; i < 100; ++i) {
        auto p = seat(i);
        p[0] += (f % 2 ? 0.1f : -0.1f);  // small float noise, across cell borders too
        p[1] += (f % 3 ? 0.15f : -0.15f);
        check(t.Offer(f, kFar, p.data(), kView) == first[size_t(i)], "stable in the hold, animated");
      }
    check(t.seat_count() == 200, "no new seats from animation");
  }
  {  // A second pass in the same frame gets the same answer.
    Thinner t;
    const auto p = seat(1), q = seat(2);
    const bool a = t.Offer(1, kFar, p.data(), kView), b = t.Offer(1, kFar, q.data(), kView);
    check(a != b, "alternate");
    check(t.Offer(1, kFar, p.data(), kView) == a && t.Offer(1, kFar, q.data(), kView) == b, "same frame, same answer");
  }
  {  // Bigger mesh: back at once, and it does not vanish when the game shrinks it again in the same shot.
    Thinner t;
    t.Offer(1, kFar, seat(10).data(), kView);
    auto s = seat(11);
    t.Offer(1, kFar, s.data(), kView);
    auto out_seat = t.Offer(2, kFar, s.data(), kView) ? s : seat(10);
    check(t.Offer(3, kFar, out_seat.data(), kView), "still out");
    check(!t.Offer(4, kMiddle, out_seat.data(), kView), "bigger mesh: drawn");
    for (uint32_t f = 5; f < 30; ++f) check(!t.Offer(f, kFar, out_seat.data(), kView), "no vanishing in the same shot");
    check(t.stats().returned == 1, "counted back");
    check(t.Offer(30, kFar, out_seat.data(), kOtherView), "new shot: thinned again");
    check(t.stats().cuts == 1, "cut counted");
  }
  {  // Near mesh during the hold leaves the seat decision alone; leaving the view allows a new one.
    Thinner t;
    auto p = seat(20), q = seat(21);
    t.Offer(1, kFar, p.data(), kView);
    t.Offer(1, kFar, q.data(), kView);
    auto out_seat = t.Offer(2, kFar, p.data(), kView) ? p : q;
    check(!t.Offer(3, kMiddle, out_seat.data(), kView), "drawn");
    check(!t.Offer(4, kFar, out_seat.data(), kView), "stays drawn");
    check(t.Offer(100, kFar, out_seat.data(), kView), "after leaving the view: out from first sight");
  }
  {  // A small camera move is not a cut. Reset forces a fresh decision.
    Thinner t;
    float moved[16];
    for (int i = 0; i < 16; ++i) moved[i] = kView[i] * 1.01f;
    t.Offer(1, kFar, seat(1).data(), kView);
    t.Offer(2, kFar, seat(1).data(), moved);
    check(t.stats().cuts == 0, "pan is no cut");
    check(t.stats().frames == 2, "frames");
    t.Reset();
  }
  {  // The 64-index side geometry is not a far seat.
    Thinner t;
    check(!t.Offer(1, 64, seat(1).data(), kView) && !t.Offer(1, 64, seat(2).data(), kView) && t.stats().other == 2, "small mesh stays");
    check(t.seat_count() == 0, "no seat");
  }
  {  // Table overflow forgets everything, without a crash.
    Thinner t;
    for (int i = 0; i < 20000; ++i) t.Offer(1, kFar, seat(i).data(), kView);
    check(t.stats().resets == 1, "reset once");
  }
  std::printf(failures ? "FAILED %d\n" : "crowd-thin-test PASS\n", failures);
  return failures != 0;
}
