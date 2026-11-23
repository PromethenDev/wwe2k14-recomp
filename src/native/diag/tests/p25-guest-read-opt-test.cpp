// P25-DIAG12 optimized host test for the release-only OS-read fault (RUN13: read_stop=fault@obj on a plausible mapped
// object). Build with -O2/-O3 -DNDEBUG; uses an always-on CHECK (assert is compiled out by NDEBUG).
//   g++ -std=c++20 -O2 -DNDEBUG -I src/native/diag src/native/diag/tests/p25-guest-read-opt-test.cpp -o t && ./t
#include <cstdint>
#include <cstdio>

#if defined(__linux__)
#include <sys/mman.h>
#include <unistd.h>
#endif

#include "p25_guest_read.h"

using namespace wwe2k14::p25diag;

static int g_failures = 0;

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
      ++g_failures;                                                          \
    }                                                                        \
  } while (0)

#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline)) static void DirtyStack(uint32_t seed) {
  volatile uint8_t buf[8192];
  for (size_t i = 0; i < sizeof buf; ++i) buf[i] = uint8_t(seed + i * 13u);
  asm volatile("" : : "r"((const void*)buf) : "memory");
}
#else
static void DirtyStack(uint32_t) {}
#endif

int main() {
  uint32_t word = 0x11223344;
  uint32_t out = 0;
  CHECK(OsReadBe32(&word, &out));
  CHECK(out == __builtin_bswap32(word));
  CHECK(OsLastReadErrno() == 0);

#if defined(__linux__)
  const long page_size = ::sysconf(_SC_PAGESIZE);
  CHECK(page_size > 0);
  uint8_t* pages = static_cast<uint8_t*>(
      ::mmap(nullptr, size_t(page_size) * 3, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  CHECK(pages != MAP_FAILED);
  if (pages == MAP_FAILED) return 1;
  for (long i = 0; i < page_size; ++i) pages[page_size + i] = uint8_t((i * 37 + 11) & 0xFF);

  // 32-bit guest translated-address model: a uint32_t guest offset (0x1000..0x13FF) plus the translated base.
  for (uint32_t round = 0; round < 100000; ++round) {
    DirtyStack(round);
    const uint32_t guest = 0x1000u + ((round * 4u) & 0x3FFu);
    const uint8_t* host = pages + guest;
    const uint32_t expected = (uint32_t(host[0]) << 24) | (uint32_t(host[1]) << 16) | (uint32_t(host[2]) << 8) |
                              uint32_t(host[3]);
    out = 0;
    CHECK(OsReadBe32(host, &out));
    CHECK(out == expected);
    CHECK(OsLastReadErrno() == 0);
  }

  CHECK(::mprotect(pages + page_size * 2, size_t(page_size), PROT_NONE) == 0);
  out = 0;
  CHECK(!OsReadBe32(pages + page_size * 2, &out));
  CHECK(OsLastReadErrno() != 0);
  out = 0;
  CHECK(!OsReadBe32(pages + page_size * 2 - 2, &out));  // crosses into PROT_NONE
  CHECK(OsLastReadErrno() != 0);
  CHECK(::munmap(pages, size_t(page_size) * 3) == 0);
#endif

  if (g_failures) {
    std::fprintf(stderr, "p25-guest-read-opt-test FAILED (%d)\n", g_failures);
    return 1;
  }
  std::printf("p25-guest-read-opt-test PASS\n");
  return 0;
}
