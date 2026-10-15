// Opt-in guest-memory probe for the R7 Steel-Cage entrance freeze
//
//
// The hang watchdog (src/thread_profiler.cpp, WWE13_HANG_DUMP) proves the guest
// vdswap/main thread spins at ~100 % CPU inside sub_82BB9418, the "job/command
// list" pump (0x82BB9418 -> sub_82BB8C00 state machine -> repeat). This file
// adds a strong PPC_FUNC override of that pump which, when the watchdog reports
// a freeze, dumps the queue/command state from the guest thread (where the PPC
// `base` pointer and the guest addresses are valid, so no unmapped read).
//
// Off by default: enabled only with WWE2K14_CAGE_PROBE=1; the dump is appended
// to "$WWE13_HANG_DUMP.probe". Same strong-PPC_FUNC mechanism as
// src/cloth_handshake.cpp and src/present_interval.cpp.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <rex/ppc/context.h>
#include <rex/ppc/memory.h>

namespace {

bool ProbeEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("WWE2K14_CAGE_PROBE");
    return value != nullptr && std::strcmp(value, "1") == 0;
  }();
  return enabled;
}

std::string ProbePath() {
  static const std::string path = [] {
    const char* value = std::getenv("WWE13_HANG_DUMP");
    return std::string(value && *value ? value : "hang-probe.dump") + ".probe";
  }();
  return path;
}

std::atomic<uint32_t> g_freeze_generation{0};
uint32_t g_dumped_generation = 0;

// Dumps manager fields / entry list / current command node of the pump. Runs on
// the guest thread inside the overridden pump, so `base` and all guest reads are
// valid. `obj` is the manager pointer the pump was called with (ctx.r3).
void DumpPumpState(uint8_t* base, uint32_t obj) {
  FILE* file = std::fopen(ProbePath().c_str(), "ab");
  if (!file) {
    return;
  }
  std::fprintf(file, "\n==== cage-probe pump state ====\nobj=0x%08X\n", obj);
  if (obj == 0) {
    std::fclose(file);
    return;
  }
  for (uint32_t off = 0; off <= 0x8C; off += 4) {
    const uint32_t value = PPC_LOAD_U32(obj + off);
    std::fprintf(file, "  obj[%02X]=0x%08X (%d)\n", off, value, int32_t(value));
  }
  const uint32_t bank = PPC_LOAD_U32(obj + 0x24);
  const uint32_t current = PPC_LOAD_U32(obj + 0x2C);
  const uint32_t list = PPC_LOAD_U32(obj + 0x6C);
  const uint32_t count = PPC_LOAD_U32(obj + 0x84);
  std::fprintf(file, "bank=%u current=%u list=0x%08X count=%u\n", bank, current, list, count);
  const uint32_t entries = count > 64 ? 64 : count;
  for (uint32_t i = 0; i < entries && list != 0; ++i) {
    const uint32_t entry = list + i * 0x44;
    std::fprintf(file, "  entry[%u]=0x%08X end=%u cur=%u\n", i, entry, PPC_LOAD_U32(entry + 0xC),
                 PPC_LOAD_U32(entry + 0x10));
  }
  if (bank < 0x1000) {
    const uint32_t bank_ptr = PPC_LOAD_U32(obj + 0x30 + 4 * bank);
    std::fprintf(file, "bank_ptr=0x%08X\n", bank_ptr);
    if (bank_ptr != 0) {
      const uint32_t node = bank_ptr + current * 16;
      std::fprintf(file, "node=0x%08X\n", node);
      for (uint32_t j = 0; j < 16; ++j) {
        const uint32_t value = PPC_LOAD_U32(node + j * 4);
        std::fprintf(file, "  node[%02X]=0x%08X (%d)\n", j * 4, value, int32_t(value));
      }
      for (int32_t k = -2; k <= 2; ++k) {
        const uint32_t q = bank_ptr + uint32_t(int32_t(current) + k) * 16;
        std::fprintf(file, "  near[%d]=0x%08X state=%d size=%d\n", k, q, int32_t(PPC_LOAD_U32(q)),
                     int32_t(PPC_LOAD_U32(q + 8)));
      }
    }
  }
  std::fflush(file);
  std::fclose(file);
}

}  // namespace

// Called by the hang watchdog every 5 s while frozen (weakly linked); request a
// dump on the next pump call. Ignores the FILE* (the probe writes its own file).
extern "C" void wwe2k14_hang_extra_dump(FILE*) {
  if (ProbeEnabled()) {
    g_freeze_generation.fetch_add(1, std::memory_order_relaxed);
  }
}

extern "C" PPC_FUNC(__imp__sub_82BB9418);

// Always-on pump capture for the freeze logger (src/freeze_guest_state.cpp).
// This is the single sub_82BB9418 override; the store is two relaxed atomics.
extern "C" void wwe2k14_freeze_record_pump(uint8_t* base, uint32_t object);

extern "C" PPC_FUNC(sub_82BB9418) {
  wwe2k14_freeze_record_pump(base, ctx.r3.u32);
  if (ProbeEnabled()) {
    const uint32_t generation = g_freeze_generation.load(std::memory_order_relaxed);
    if (generation != 0 && generation != g_dumped_generation) {
      g_dumped_generation = generation;
      DumpPumpState(base, ctx.r3.u32);
    }
  }
  __imp__sub_82BB9418(ctx, base);
}
