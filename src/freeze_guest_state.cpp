// Always-on freeze-report guest-state provider.
//
// The freeze logger lives in rexcore (below rex::system) and its report runs on
// the 1 Hz watchdog thread, which is not a bound guest ThreadState, so it
// cannot use REX_KERNEL_MEMORY(). This file captures the guest memory base and
// the 0x82BB command-pump object pointer cheaply from a strong PPC_FUNC
// override of sub_82BB9418 (the pump drain, generated/wwe2k14_recomp.76.cpp:8207)
// and dumps the CAGE-SPIN section-6 fields into the open report file.
//
// The override itself lives in src/cage_hang_probe.cpp (the only translation
// unit that defines sub_82BB9418); it calls wwe2k14_freeze_record_pump() here.
// The read helper is big-endian, matching the guest.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <rex/diagnostics/freeze_logger.h>
#include <rex/ppc/context.h>
#include <rex/ppc/memory.h>

namespace {

std::atomic<const uint8_t*> g_guest_base{nullptr};
std::atomic<uint32_t> g_pump_object{0};

// Guest .data addresses found by analysing the freeze.
constexpr uint32_t kPresentDeviceGlobal = 0x83929AB8;
constexpr uint32_t kPumpFrameGlobal = 0x83940170;   // written 76.cpp:8774
constexpr uint32_t kPumpPresentGlobal = 0x8394016C; // read 76.cpp:8765
constexpr uint32_t kPumpTimestampGlobal = 0x83930050;  // 64-bit (76.cpp:8367)
constexpr uint32_t kFrameTimeGlobal = 0x835E9CF4;      // float (76.cpp:8514)
constexpr uint32_t kTimingAccum0 = 0x8392DA50;
constexpr uint32_t kTimingAccum1 = 0x8392DA18;
constexpr uint32_t kTimingAccum2 = 0x8392DC88;

inline uint32_t Load32(const uint8_t* base, uint32_t address) {
  return __builtin_bswap32(*reinterpret_cast<const volatile uint32_t*>(base + address));
}

inline uint64_t Load64(const uint8_t* base, uint32_t address) {
  const uint32_t hi = Load32(base, address);
  const uint32_t lo = Load32(base, address + 4);
  return (uint64_t(hi) << 32) | lo;
}

void DumpGuestState(FILE* file, uint64_t now_ns) {
  (void)now_ns;
  const uint8_t* base = g_guest_base.load(std::memory_order_relaxed);
  if (!base) {
    std::fprintf(file, "guest_base unavailable (0x82BB pump never entered)\n");
    return;
  }
  const uint32_t pump = g_pump_object.load(std::memory_order_relaxed);
  std::fprintf(file, "pump_object=0x%08X\n", pump);
  if (pump != 0) {
    const uint32_t vtable = Load32(base, pump + 0x00);
    const uint32_t bank = Load32(base, pump + 0x24);
    const uint32_t cursor = Load32(base, pump + 0x2C);
    const uint32_t staging = Load32(base, pump + 0x68);
    const uint32_t active = Load32(base, pump + 0x6C);
    const uint32_t count = Load32(base, pump + 0x84);
    const uint32_t frame_counter = Load32(base, pump + 0x28);
    const uint32_t counter90 = Load32(base, pump + 0x90);
    std::fprintf(file,
                 "pump+0x00=0x%08X pump+0x24(bank)=%u pump+0x2C(cursor)=%u "
                 "pump+0x28=%u pump+0x68(staging)=0x%08X pump+0x6C(active)=0x%08X "
                 "pump+0x84(count)=%u pump+0x90=%u\n",
                 vtable, bank, cursor, frame_counter, staging, active, count, counter90);
    uint32_t bank_ptr = 0;
    if (bank < 0x1000) {
      bank_ptr = Load32(base, pump + 0x30 + 4 * bank);
      std::fprintf(file, "bank_ptr=0x%08X\n", bank_ptr);
    }
    const uint32_t entries = count > 8 ? 8 : count;
    for (uint32_t i = 0; i < entries && active != 0; ++i) {
      const uint32_t entry = active + i * 0x44;
      std::fprintf(file, "entry[%u]=0x%08X end=0x%08X start=0x%08X\n", i, entry,
                   Load32(base, entry + 0x0C), Load32(base, entry + 0x10));
    }
    if (bank_ptr != 0) {
      for (int32_t k = -2; k <= 3; ++k) {
        const uint32_t node = bank_ptr + uint32_t(int32_t(cursor) + k) * 0x10;
        std::fprintf(file, "node[%d]=0x%08X state=%u size=0x%08X\n", k, node,
                     Load32(base, node + 0x00), Load32(base, node + 0x08));
      }
    }
  }

  const uint32_t device = Load32(base, kPresentDeviceGlobal);
  std::fprintf(file, "device=0x%08X\n", device);
  if (device != 0) {
    std::fprintf(file,
                 "device+0x11C=0x%08X +0x120=0x%08X +0x130=0x%08X +0x134=0x%08X "
                 "+0x138=0x%08X +0x140=0x%08X +0x144=0x%08X\n",
                 Load32(base, device + 0x11C), Load32(base, device + 0x120),
                 Load32(base, device + 0x130), Load32(base, device + 0x134),
                 Load32(base, device + 0x138), Load32(base, device + 0x140),
                 Load32(base, device + 0x144));
  }

  const uint64_t timestamp = Load64(base, kPumpTimestampGlobal);
  const uint32_t frame_time_bits = Load32(base, kFrameTimeGlobal);
  float frame_time = 0.0f;
  std::memcpy(&frame_time, &frame_time_bits, sizeof(frame_time));
  std::fprintf(file,
               "global 0x%08X=0x%08X 0x%08X=0x%08X 0x%08X=0x%016llX (u64) 0x%08X=%.6f (float)\n",
               kPumpFrameGlobal, Load32(base, kPumpFrameGlobal), kPumpPresentGlobal,
               Load32(base, kPumpPresentGlobal), kPumpTimestampGlobal,
               (unsigned long long)timestamp, kFrameTimeGlobal, double(frame_time));
  std::fprintf(file, "timing 0x%08X=0x%08X 0x%08X=0x%08X 0x%08X=0x%08X\n", kTimingAccum0,
               Load32(base, kTimingAccum0), kTimingAccum1, Load32(base, kTimingAccum1),
               kTimingAccum2, Load32(base, kTimingAccum2));
  std::fflush(file);
}

}  // namespace

// Called by the single sub_82BB9418 override in src/cage_hang_probe.cpp on every
// pump drain call: two relaxed atomic stores, always on.
extern "C" void wwe2k14_freeze_record_pump(uint8_t* base, uint32_t object) {
  g_guest_base.store(base, std::memory_order_relaxed);
  g_pump_object.store(object, std::memory_order_relaxed);
}

namespace wwe2k14 {

void RegisterFreezeGuestState() {
  rex::diagnostics::freeze_logger::SetGuestStateDumpFn(&DumpGuestState);
}

}  // namespace wwe2k14
