// Host test for the stall catcher (src/hitch_catcher.cpp), Windows build run under Wine or Windows:
//   the main thread presents every 16 ms for 1.3 s (the catcher's idle refresh sees the worker), then the worker spins in BusySpin() for 600 ms while presents stop.
//   Expected: one "[hitch] n=1 ms=~600" header with samples > 0 and a thread line whose leaf is in BusySpin.
// Build: tools/diag/build-hitch-test.sh
#include "../../hitch_catcher.cpp"

#include <atomic>
#include <thread>

extern "C" __declspec(noinline) void BusySpin(std::atomic<bool>* stop) {
  volatile uint64_t x = 0;
  while (!stop->load(std::memory_order_relaxed)) x = x * 3 + 1;
}

int main() {
  using namespace std::chrono_literals;
  wwe2k14::hitch::Start();
  std::atomic<bool> stop{false}, go{false};
  std::thread busy([&] {
    while (!go) std::this_thread::sleep_for(1ms);
    BusySpin(&stop);
  });
  for (int i = 0; i < 80; ++i) { wwe2k14::hitch::NotePresent(); std::this_thread::sleep_for(16ms); }  // > 1 s: refresh
  go = true;
  std::this_thread::sleep_for(600ms);  // no presents: a stall
  stop = true;
  busy.join();
  wwe2k14::hitch::NotePresent();
  std::this_thread::sleep_for(200ms);  // let the watchdog report
  std::printf("BusySpin rva=0x%llx\n", (unsigned long long)(uintptr_t(&BusySpin) - uintptr_t(&__ImageBase)));
  return 0;
}
