// CPU test for the shared constants32 policy (G56 B1). Builds with the host compiler only:
//   mkdir -p out/lanes/g56/constants32-test-bin
//   g++ -std=c++17 -O0 -Wall -Wextra -o out/lanes/g56/constants32-test-bin/nr_constants32_test
//       out/lanes/g56/src/native/tests/nr_constants32_test.cpp
//   out/lanes/g56/constants32-test-bin/nr_constants32_test
// Temporary folders are created only under out/ (out/lanes/g56/constants32-test-tmp).
#include "../nr/nr_constants32.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace nr_constants32;

static int g_failures = 0;

#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) {                                                        \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                       \
    }                                                                     \
  } while (0)

static void WriteMarker(const fs::path& folder) {
  fs::create_directories(folder);
  std::ofstream(folder / kMarkerName) << "1\n";
}

static void TestSwitchParsing() {
  CHECK(!SwitchRequested(nullptr));
  CHECK(!SwitchRequested(""));
  CHECK(!SwitchRequested("0"));
  CHECK(!SwitchRequested("true"));
  CHECK(!SwitchRequested("01"));
  CHECK(!SwitchRequested(" 1"));
  CHECK(!SwitchRequested("1 "));
  CHECK(SwitchRequested("1"));
}

static void TestFolders() {
  CHECK(PrimaryFolder(nullptr) == "out/nr/shaders");
  CHECK(PrimaryFolder("") == "");  // runtime.cpp loads an empty WWE2K14_NR_SHADERS as-is
  CHECK(PrimaryFolder("out/nr/shaders32") == "out/nr/shaders32");
  CHECK(SecondaryFolder(nullptr).empty());
  CHECK(SecondaryFolder("") == "");
  CHECK(SecondaryFolder("out/nr/shaders_b") == "out/nr/shaders_b");
}

static void TestSelectionRules() {
  MarkerStatus absent;
  MarkerStatus present;
  present.present = true;
  MarkerStatus bad;
  bad.error = true;
  bad.error_text = "bad marker";

  Selection s = Select(false, false, absent, absent);
  CHECK(s.ok && !s.enabled && s.error.empty());

  s = Select(true, false, present, absent);
  CHECK(s.ok && s.enabled && s.error.empty());

  s = Select(true, false, absent, absent);
  CHECK(!s.ok && !s.enabled && !s.error.empty());

  s = Select(false, false, present, absent);
  CHECK(!s.ok && !s.error.empty());

  s = Select(false, true, absent, present);
  CHECK(!s.ok && !s.error.empty());

  s = Select(true, true, present, absent);
  CHECK(!s.ok && !s.error.empty());

  s = Select(true, true, present, present);
  CHECK(s.ok && s.enabled);

  s = Select(false, true, absent, absent);
  CHECK(s.ok && !s.enabled);

  s = Select(false, false, bad, absent);
  CHECK(!s.ok && !s.error.empty());
  s = Select(true, true, present, bad);
  CHECK(!s.ok && !s.error.empty());
}

static void TestMarkerFixtures() {
  const fs::path root = fs::path("out") / "lanes" / "g56" / "constants32-test-tmp";
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root / "empty", ec);
  fs::create_directories(root / "with_marker", ec);
  fs::create_directories(root / "dir_marker", ec);
  fs::create_directories(root / "dir_marker" / kMarkerName, ec);
  CHECK(!ec);
  WriteMarker(root / "with_marker");

  MarkerStatus empty = InspectMarker((root / "empty").string());
  CHECK(!empty.present && !empty.error);

  MarkerStatus missing_folder = InspectMarker((root / "does_not_exist").string());
  CHECK(!missing_folder.present && !missing_folder.error);  // no folder means no marker, not a failed run

  MarkerStatus marker = InspectMarker((root / "with_marker").string());
  CHECK(marker.present && !marker.error);

  MarkerStatus dir_marker = InspectMarker((root / "dir_marker").string());
  CHECK(!dir_marker.present && dir_marker.error);  // a directory is not a regular marker

  Selection s = Select(true, false, marker, MarkerStatus{});
  CHECK(s.ok && s.enabled);
  s = Select(false, false, marker, MarkerStatus{});
  CHECK(!s.ok);
  s = Select(true, false, empty, MarkerStatus{});
  CHECK(!s.ok);
  s = Select(true, true, marker, empty);
  CHECK(!s.ok);
  s = Select(false, true, empty, marker);
  CHECK(!s.ok);
  s = Select(true, true, marker, marker);
  CHECK(s.ok && s.enabled);

  // ReadSelection: primary is the explicit Init(shader_dir) argument, never the ambient
  // WWE2K14_NR_SHADERS; an empty secondary means "not configured", not a folder named "".
  const char* saved = std::getenv(kPrimaryFolderSwitch);
  const std::string saved_value = saved ? saved : std::string();
  CHECK(setenv(kPrimaryFolderSwitch, (root / "with_marker").string().c_str(), 1) == 0);
  Selection r = ReadSelection((root / "empty").string(), "", "0");
  CHECK(r.ok && !r.enabled);  // ambient folder has the marker, the explicit primary does not
  r = ReadSelection((root / "with_marker").string(), "", "1");
  CHECK(r.ok && r.enabled);  // empty secondary is not inspected/configured
  r = ReadSelection((root / "empty").string(), "", "1");
  CHECK(!r.ok && !r.enabled);
  r = ReadSelection((root / "with_marker").string(), (root / "empty").string(), "1");
  CHECK(!r.ok);  // a non-empty secondary is configured and must agree
  r = ReadSelection((root / "with_marker").string(), (root / "with_marker").string(), "1");
  CHECK(r.ok && r.enabled);
  if (saved) setenv(kPrimaryFolderSwitch, saved_value.c_str(), 1);
  else unsetenv(kPrimaryFolderSwitch);

  fs::remove_all(root, ec);
}

static void TestAbiPlans() {
  const AbiPlan legacy = Plan(false);
  CHECK(!legacy.constants32);
  CHECK(legacy.descriptor_set_count == 4);
  CHECK(legacy.constants_ring_addressable);
  CHECK(!legacy.constants_ring_storage);
  CHECK(!legacy.bind_constants_set_every_draw);
  CHECK(legacy.frame_slots_eligible);
  CHECK(legacy.rt1_eligible);
  CHECK(legacy.requires_buffer_device_address);
  CHECK(legacy.requires_descriptor_indexing);
  CHECK(legacy.constants_use_int64);

  const AbiPlan c32 = Plan(true);
  CHECK(c32.constants32);
  CHECK(c32.descriptor_set_count == 5);
  CHECK(!c32.constants_ring_addressable);
  CHECK(c32.constants_ring_storage);
  CHECK(c32.bind_constants_set_every_draw);
  CHECK(!c32.frame_slots_eligible);
  CHECK(!c32.rt1_eligible);
  CHECK(c32.requires_buffer_device_address);
  CHECK(c32.requires_descriptor_indexing);
  CHECK(!c32.constants_use_int64);

  const uint64_t addr = 0x7F12'3456'7890ull;  // synthetic device address, not a real allocation
  CHECK(ConstantAddressBase(legacy, true, addr) == addr);
  CHECK(ConstantAddressBase(legacy, false, addr) == 0);
  CHECK(ConstantAddressBase(c32, true, addr) == 0);
  CHECK(ConstantAddressBase(c32, false, addr) == 0);
}

static void TestPushAndRingBounds() {
  CHECK(PushLow(0x0000'0001'2345'6789ull) == 0x2345'6789u);
  CHECK(PushHigh(0x0000'0001'2345'6789ull) == 1u);
  CHECK(Fits32(0));
  CHECK(Fits32(0xFFFF'FFFFull));
  CHECK(!Fits32(0x1'0000'0000ull));

  const uint64_t ring = kSlotBytes * kSlots;  // 17408 * 12288 = 213,909,504 < 2^32
  CHECK(Fits32(ring));
  CHECK(RingInBounds(0, ring, ring));
  CHECK(RingInBounds(kGuardBytes, 4096, ring));
  CHECK(RingInBounds(ring - 1, 1, ring));
  CHECK(!RingInBounds(ring, 1, ring));
  CHECK(!RingInBounds(0, ring + 1, ring));
  CHECK(!RingInBounds(ring + 1, 1, ring));
  CHECK(!RingInBounds(UINT64_MAX, 2, ring));
  const uint64_t slot = 7 * kSlotBytes + kGuardBytes;
  CHECK(Fits32(slot));
  CHECK(RingInBounds(slot, 4096 + 4096 + 1024, ring));
}

static void TestPushBlock24() {
  // 24-byte push block (uint64_t push[3]) as nr_backend.cpp builds it: the three block offsets sit
  // in the low words at bytes 0/8/16. Values are synthetic, not from a real allocation.
  const uint64_t device_address = 0x7F12'3456'7890ull;  // synthetic legacy buffer device address
  const uint64_t slot = (kSlots - 1) * kSlotBytes + kGuardBytes;  // last slot's block base
  const uint64_t ring = kSlotBytes * kSlots;                       // 17408 * 12288 = 213,909,504
  const AbiPlan legacy = Plan(false);
  const AbiPlan c32 = Plan(true);

  // Legacy: base is the device address, so each push is address + slot, +4096, +8192.
  const uint64_t legacy_base = ConstantAddressBase(legacy, true, device_address);
  uint64_t legacy_push[3];
  legacy_push[0] = legacy_base + slot;
  legacy_push[1] = legacy_base + slot + 4096;
  legacy_push[2] = legacy_base + slot + 8192;
  const uint64_t legacy_expected[3] = {device_address + slot, device_address + slot + 4096,
                                       device_address + slot + 8192};
  CHECK(std::memcmp(legacy_push, legacy_expected, sizeof(legacy_push)) == 0);

  // constants32: base 0, so each entry's low word is the byte offset and its high word is zero.
  const uint64_t c32_base = ConstantAddressBase(c32, true, device_address);
  CHECK(c32_base == 0);
  uint64_t push[3];
  push[0] = c32_base + slot;
  push[1] = c32_base + slot + 4096;
  push[2] = c32_base + slot + 8192;
  const uint8_t* push_bytes = reinterpret_cast<const uint8_t*>(push);
  for (int i = 0; i < 3; ++i) {
    uint32_t low = 0;
    uint32_t high = 0;
    std::memcpy(&low, push_bytes + i * 8, 4);       // low words at bytes 0/8/16
    std::memcpy(&high, push_bytes + i * 8 + 4, 4);  // high words at bytes 4/12/20
    CHECK(low == static_cast<uint32_t>(slot) + static_cast<uint32_t>(i) * 4096u);
    CHECK(high == 0);
    CHECK(PushHigh(push[i]) == 0);
    CHECK(Fits32(push[i]));
  }

  // Last slot: shared starts at +8192; end of shared plus the trailing 4096-byte guard is the ring
  // end, so the highest constants32 push plus the shared block stays inside the 32-bit ring.
  CHECK(push[2] == slot + 8192);
  CHECK(push[2] + 1024 + 4096 == ring);
  CHECK(RingInBounds(push[2], 1024 + 4096, ring));
}

int main() {
  TestSwitchParsing();
  TestFolders();
  TestSelectionRules();
  TestMarkerFixtures();
  TestAbiPlans();
  TestPushAndRingBounds();
  TestPushBlock24();
  if (g_failures != 0) {
    std::fprintf(stderr, "nr_constants32_test: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("nr_constants32_test: PASS\n");
  return 0;
}
