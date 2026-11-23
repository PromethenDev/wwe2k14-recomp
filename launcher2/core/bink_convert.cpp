#include "bink_convert.h"
#include "internal/util.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstring>
#include <fstream>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace wwe2k14::launcher {
namespace {

struct ChildResult {
  bool started = false;
  bool canceled = false;
  int exit_code = -1;
  std::string detail;
};

std::string Trim(std::string text) {
  while (!text.empty() && (text.back() == '\r' || text.back() == '\n')) text.pop_back();
  return text;
}

bool ParseUint(std::string_view text, uint64_t* value) {
  if (text.empty() || value == nullptr) return false;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), *value);
  return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

class OutputLines {
 public:
  explicit OutputLines(const ProgressFn& progress) : progress_(progress) {}

  void Append(const char* bytes, size_t size) {
    for (size_t i = 0; i < size; ++i) {
      if (bytes[i] == '\n') {
        Handle(Trim(std::move(line_)));
        line_.clear();
      } else if (line_.size() < 8192) {
        line_.push_back(bytes[i]);
      }
    }
  }

  void Finish() {
    if (!line_.empty()) Handle(Trim(std::move(line_)));
    line_.clear();
  }

  const std::string& detail() const { return detail_; }

 private:
  void Handle(std::string line) {
    constexpr std::string_view prefix = "PROGRESS\t";
    if (line.compare(0, prefix.size(), prefix) == 0) {
      const size_t first = line.find('\t', prefix.size());
      const size_t second = first == std::string::npos ? first : line.find('\t', first + 1);
      uint64_t done = 0, total = 0;
      if (first != std::string::npos && second != std::string::npos &&
          ParseUint(std::string_view(line).substr(prefix.size(), first - prefix.size()), &done) &&
          ParseUint(std::string_view(line).substr(first + 1, second - first - 1), &total) && progress_) {
        const std::string label = "Converting titantron frame " + std::to_string(done) + " of " + std::to_string(total);
        progress_(done, total, label);
      }
      return;
    }
    if (line.compare(0, 6, "ERROR\t") == 0) line.erase(0, 6);
    if (!line.empty()) detail_ = std::move(line);
  }

  const ProgressFn& progress_;
  std::string line_;
  std::string detail_;
};

#ifdef _WIN32
struct WinHandle {
  HANDLE value = nullptr;
  WinHandle() = default;
  explicit WinHandle(HANDLE h) : value(h) {}
  ~WinHandle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
  WinHandle(const WinHandle&) = delete;
  WinHandle& operator=(const WinHandle&) = delete;
  WinHandle(WinHandle&& other) noexcept : value(other.value) { other.value = nullptr; }
  WinHandle& operator=(WinHandle&& other) noexcept {
    if (this != &other) {
      if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value);
      value = other.value;
      other.value = nullptr;
    }
    return *this;
  }
};

std::wstring QuoteWindowsArg(std::wstring_view value) {
  std::wstring quoted(1, L'"');
  size_t slashes = 0;
  for (wchar_t c : value) {
    if (c == L'\\') { ++slashes; continue; }
    if (c == L'"') {
      quoted.append(slashes * 2 + 1, L'\\');
      quoted.push_back(L'"');
    } else {
      quoted.append(slashes, L'\\');
      quoted.push_back(c);
    }
    slashes = 0;
  }
  quoted.append(slashes * 2, L'\\');
  quoted.push_back(L'"');
  return quoted;
}

ChildResult RunConverter(const fs::path& executable, const fs::path& input, const fs::path& output,
                         const ProgressFn& progress, const CancelFlag& cancel) {
  ChildResult result;
  SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
  HANDLE read_raw = nullptr, write_raw = nullptr;
  if (!CreatePipe(&read_raw, &write_raw, &security, 0)) {
    result.detail = "The converter's progress pipe could not be created.";
    return result;
  }
  WinHandle read_pipe(read_raw), write_pipe(write_raw);
  if (!SetHandleInformation(read_pipe.value, HANDLE_FLAG_INHERIT, 0)) {
    result.detail = "The converter's progress pipe could not be configured.";
    return result;
  }

  const std::wstring exe = executable.wstring();
  std::wstring command = QuoteWindowsArg(exe) + L" " + QuoteWindowsArg(input.wstring()) + L" " +
                         QuoteWindowsArg(output.wstring()) + L" " + QuoteWindowsArg(L"--progress");
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = write_pipe.value;
  startup.hStdError = write_pipe.value;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  PROCESS_INFORMATION process{};
  std::vector<wchar_t> mutable_command(command.begin(), command.end());
  mutable_command.push_back(L'\0');
  if (!CreateProcessW(exe.c_str(), mutable_command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                      nullptr, nullptr, &startup, &process)) {
    result.detail = "Windows could not start the converter (error " + std::to_string(GetLastError()) + ").";
    return result;
  }
  result.started = true;
  WinHandle process_handle(process.hProcess), thread_handle(process.hThread);
  write_pipe = WinHandle{};
  OutputLines lines(progress);
  bool terminated = false;
  DWORD exit_code = STILL_ACTIVE;
  for (;;) {
    if (cancel.load(std::memory_order_relaxed) && !terminated) {
      result.canceled = true;
      terminated = true;
      TerminateProcess(process_handle.value, 1);
    }
    DWORD available = 0;
    if (PeekNamedPipe(read_pipe.value, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
      std::array<char, 4096> buffer{};
      DWORD read = 0;
      if (ReadFile(read_pipe.value, buffer.data(), std::min<DWORD>(available, static_cast<DWORD>(buffer.size())), &read, nullptr) && read)
        lines.Append(buffer.data(), read);
    }
    const DWORD wait = WaitForSingleObject(process_handle.value, 0);
    if (wait == WAIT_OBJECT_0) {
      for (;;) {
        available = 0;
        if (!PeekNamedPipe(read_pipe.value, nullptr, 0, nullptr, &available, nullptr) || available == 0) break;
        std::array<char, 4096> buffer{};
        DWORD read = 0;
        if (!ReadFile(read_pipe.value, buffer.data(), std::min<DWORD>(available, static_cast<DWORD>(buffer.size())), &read, nullptr) || !read) break;
        lines.Append(buffer.data(), read);
      }
      GetExitCodeProcess(process_handle.value, &exit_code);
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  lines.Finish();
  result.exit_code = static_cast<int>(exit_code);
  result.detail = lines.detail();
  return result;
}
#else
ChildResult RunConverter(const fs::path& executable, const fs::path& input, const fs::path& output,
                         const ProgressFn& progress, const CancelFlag& cancel) {
  ChildResult result;
  int pipes[2] = {-1, -1};
  if (pipe(pipes) != 0) {
    result.detail = "The converter's progress pipe could not be created.";
    return result;
  }
  const int read_flags = fcntl(pipes[0], F_GETFL, 0);
  if (read_flags >= 0) fcntl(pipes[0], F_SETFL, read_flags | O_NONBLOCK);

  posix_spawn_file_actions_t actions;
  int error = posix_spawn_file_actions_init(&actions);
  const bool actions_initialized = error == 0;
  if (error == 0) error = posix_spawn_file_actions_adddup2(&actions, pipes[1], STDOUT_FILENO);
  if (error == 0) error = posix_spawn_file_actions_adddup2(&actions, pipes[1], STDERR_FILENO);
  if (error == 0) error = posix_spawn_file_actions_addclose(&actions, pipes[0]);
  if (error == 0) error = posix_spawn_file_actions_addclose(&actions, pipes[1]);
  if (error != 0) {
    if (actions_initialized) posix_spawn_file_actions_destroy(&actions);
    close(pipes[0]); close(pipes[1]);
    result.detail = "The converter process could not be prepared.";
    return result;
  }
  std::string exe = internal::PathToUtf8(executable), in = internal::PathToUtf8(input), out = internal::PathToUtf8(output);
  std::array<char*, 5> argv = {exe.data(), in.data(), out.data(), const_cast<char*>("--progress"), nullptr};
  pid_t child = 0;
  error = posix_spawn(&child, exe.c_str(), &actions, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  close(pipes[1]);
  if (error != 0) {
    close(pipes[0]);
    result.detail = "The converter could not be started: " + std::string(std::strerror(error));
    return result;
  }
  result.started = true;
  OutputLines lines(progress);
  bool exited = false;
  int status = 0;
  std::array<char, 4096> buffer{};
  while (!exited) {
    if (cancel.load(std::memory_order_relaxed)) {
      result.canceled = true;
      kill(child, SIGTERM);
      for (int i = 0; i < 20; ++i) {
        const pid_t waited = waitpid(child, &status, WNOHANG);
        if (waited == child) { exited = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      if (!exited) { kill(child, SIGKILL); waitpid(child, &status, 0); exited = true; }
    }
    for (;;) {
      const ssize_t got = read(pipes[0], buffer.data(), buffer.size());
      if (got > 0) { lines.Append(buffer.data(), static_cast<size_t>(got)); continue; }
      if (got < 0 && errno == EINTR) continue;
      break;
    }
    if (!exited) {
      const pid_t waited = waitpid(child, &status, WNOHANG);
      if (waited == child) exited = true;
      else if (waited < 0 && errno != EINTR) { exited = true; result.detail = "The converter process could not be monitored."; }
    }
    if (!exited) std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  for (;;) {
    const ssize_t got = read(pipes[0], buffer.data(), buffer.size());
    if (got > 0) { lines.Append(buffer.data(), static_cast<size_t>(got)); continue; }
    if (got < 0 && errno == EINTR) continue;
    break;
  }
  close(pipes[0]);
  lines.Finish();
  if (result.detail.empty()) result.detail = lines.detail();
  result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
  return result;
}
#endif

fs::path WithSuffix(const fs::path& path, const char* suffix) {
  fs::path result = path;
  result += suffix;
  return result;
}

bool HasMagic(const fs::path& path, const char* magic) {
  std::ifstream file(path, std::ios::binary);
  std::array<char, 4> header{};
  return file.read(header.data(), static_cast<std::streamsize>(header.size())) &&
         std::equal(header.begin(), header.end(), magic);
}

std::string FailureReason(const ChildResult& child) {
  if (!child.detail.empty()) return child.detail;
  if (!child.started) return "the converter could not be started";
  return "the converter stopped with error " + std::to_string(child.exit_code);
}

}  // namespace

Result ConvertWWE13TitantronOrStandIn(const fs::path& converter_executable,
                                      const fs::path& source_movie,
                                      const fs::path& stand_in_movie,
                                      const fs::path& output_movie,
                                      const std::string& wrestler_name,
                                      const ProgressFn& progress,
                                      const CancelFlag& cancel,
                                      WWE13TitantronResult* result) {
  if (!result) return Result::Fail("The titantron conversion result output is missing.", "Titantron");
  *result = {};
  if (cancel.load(std::memory_order_relaxed)) return Result::Fail("Titantron conversion was canceled.", "Titantron");

  std::error_code ec;
  if (!output_movie.parent_path().empty()) fs::create_directories(output_movie.parent_path(), ec);
  if (ec) return Result::Fail("The titantron output folder could not be created: " + ec.message(), "Titantron");
  const fs::path temporary = WithSuffix(output_movie, ".convert-tmp");
  std::string reason;
  ChildResult child;
  if (converter_executable.empty()) {
    child.detail = "the Bink converter is missing";
  } else if (!fs::is_regular_file(source_movie, ec) || ec) {
    child.detail = "the WWE '13 source movie could not be found";
  } else {
    ec.clear();
    fs::remove(temporary, ec);
    ec.clear();
    fs::remove(WithSuffix(temporary, ".part"), ec);
    ec.clear();
    const fs::path executable = fs::absolute(converter_executable, ec);
    if (ec) child.detail = "the converter path could not be resolved";
    else child = RunConverter(executable, source_movie, temporary, progress, cancel);
  }

  if (child.canceled || cancel.load(std::memory_order_relaxed)) {
    fs::remove(temporary, ec);
    fs::remove(WithSuffix(temporary, ".part"), ec);
    return Result::Fail("Titantron conversion was canceled.", "Titantron");
  }
  const bool converted = child.started && child.exit_code == 0 && HasMagic(temporary, "BIKi");
  if (converted) {
    std::error_code copy_error;
    const bool copied = fs::copy_file(temporary, output_movie, fs::copy_options::overwrite_existing, copy_error);
    fs::remove(temporary, ec);
    fs::remove(WithSuffix(temporary, ".part"), ec);
    if (!copied) return Result::Fail("The converted titantron could not be staged: " + copy_error.message(), "Titantron");
    return Result::Ok();
  }
  reason = child.started && child.exit_code == 0 && !HasMagic(temporary, "BIKi")
      ? "the converter did not create a valid BIKi movie" : FailureReason(child);
  fs::remove(temporary, ec);
  fs::remove(WithSuffix(temporary, ".part"), ec);
  if (!HasMagic(stand_in_movie, "KB2f"))
    return Result::Fail("Could not convert " + wrestler_name + "'s titantron (" + reason + "), and the WWE 2K14 stand-in is unavailable.", "Titantron");
  ec.clear();
  fs::copy_file(stand_in_movie, output_movie, fs::copy_options::overwrite_existing, ec);
  if (ec) return Result::Fail("Could not convert " + wrestler_name + "'s titantron (" + reason + "), and the stand-in could not be staged: " + ec.message(), "Titantron");
  result->used_stand_in = true;
  result->message = "Could not convert " + wrestler_name + "'s titantron (" + reason + "). The WWE 2K14 stand-in was used instead.";
  return Result::Ok();
}

}  // namespace wwe2k14::launcher
