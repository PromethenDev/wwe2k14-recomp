// P25-DIAG12: OS fault-safe 4-byte read for the diagnostic. No guest dereference: process_vm_readv (Linux) or
// ReadProcessMemory (Windows) reads the already-translated host address and returns false instead of faulting.
#pragma once

#include <cerrno>
#include <cstdint>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__linux__)
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>
#endif

namespace wwe2k14::p25diag {

// Last OS error of this thread's most recent OsReadBe32 call (0 on success). Used only for the diagnostic log.
inline thread_local int t_os_read_errno = 0;
inline int OsLastReadErrno() { return t_os_read_errno; }

inline bool OsReadBe32(const void* host, uint32_t* out) {
  t_os_read_errno = 0;
  if (!host || !out) {
    t_os_read_errno = EINVAL;
    return false;
  }
  uint8_t buf[4];
#if defined(_WIN32)
  SIZE_T n = 0;
  if (!::ReadProcessMemory(::GetCurrentProcess(), host, buf, sizeof buf, &n) || n != sizeof buf) {
    t_os_read_errno = int(::GetLastError());
    return false;
  }
#elif defined(__linux__) && defined(SYS_process_vm_readv)
  struct iovec local{buf, sizeof buf};
  struct iovec remote{const_cast<void*>(host), sizeof buf};
  // syscall is variadic: pass every argument as a full machine word. Passing the literal ints (1, 0) leaves the
  // upper halves of the count/flag registers unspecified in an optimized build; that is the RUN13 release fault.
  const long n = ::syscall(static_cast<long>(SYS_process_vm_readv),
                           static_cast<unsigned long>(::getpid()),
                           reinterpret_cast<unsigned long>(&local),
                           static_cast<unsigned long>(1),
                           reinterpret_cast<unsigned long>(&remote),
                           static_cast<unsigned long>(1),
                           static_cast<unsigned long>(0));
  if (n != static_cast<long>(sizeof buf)) {
    t_os_read_errno = errno;
    return false;
  }
#else
  t_os_read_errno = ENOSYS;
  return false;
#endif
  *out = (uint32_t(buf[0]) << 24) | (uint32_t(buf[1]) << 16) | (uint32_t(buf[2]) << 8) | uint32_t(buf[3]);
  return true;
}

}  // namespace wwe2k14::p25diag
