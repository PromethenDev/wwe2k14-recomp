// Public release builds (WWE2K14_TEST_TOOLS=OFF): test tool compiled to a no-op (owner rule 2026-10-26).
#ifdef WWE2K14_NO_TEST_TOOLS
#include <functional>
namespace wwe2k14::test_env {
void Apply() {}
void StartExitTimer(std::function<void()>) {}
}  // namespace wwe2k14::test_env
#else
// TEST-only settings override (lane all-1104, owner 2026-11-04: A/B tests on the owner laptop without editing the
// launcher). If TEST-ENV.txt exists next to the game exe, each "NAME=VALUE" line (WWE2K14_* names only, '#' comments)
// replaces the launcher's value in this process's environment; "NAME=" removes the variable. Applied first thing in
// Wwe2k14App::OnPreSetup, before native renderer code reads the environment (same point as the Xbox uwp_env::Apply).
// Every applied line is logged as "[test-env]" so the game log shows which settings a session ran with.

#if !defined(REX_UWP)

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <string>
#include <string_view>
#include <thread>

#include <rex/logging.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <climits>
#include <unistd.h>
#endif

namespace wwe2k14::test_env {
namespace {

std::string Trim(std::string_view s) {
  size_t b = 0, e = s.size();
  while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
  while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
  return std::string(s.substr(b, e - b));
}

std::string ExeDir() {
#if defined(_WIN32)
  char path[MAX_PATH * 2] = {};
  const DWORD n = GetModuleFileNameA(nullptr, path, DWORD(sizeof(path)));
  if (!n || n >= sizeof(path)) return {};
  std::string p(path, n);
#else
  char path[PATH_MAX] = {};
  const ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
  if (n <= 0) return {};
  std::string p(path, size_t(n));
#endif
  const size_t slash = p.find_last_of("\\/");
  return slash == std::string::npos ? std::string() : p.substr(0, slash + 1);
}

bool Set(const std::string& name, const std::string& value) {
#if defined(_WIN32)
  return _putenv_s(name.c_str(), value.c_str()) == 0;  // empty value removes the variable
#else
  return value.empty() ? unsetenv(name.c_str()) == 0 : setenv(name.c_str(), value.c_str(), 1) == 0;
#endif
}

}  // namespace

void Apply() {
  const std::string dir = ExeDir();
  if (dir.empty()) return;
  std::ifstream in(dir + "TEST-ENV.txt");
  if (!in) return;
  REXLOG_WARN("[test-env] TEST-ENV.txt found: applying test overrides");
  std::string line;
  int applied = 0;
  while (std::getline(in, line) && applied < 64) {
    const std::string t = Trim(line);
    if (t.empty() || t[0] == '#') continue;
    const size_t eq = t.find('=');
    if (eq == std::string::npos) continue;
    const std::string name = Trim(std::string_view(t).substr(0, eq));
    const std::string value = Trim(std::string_view(t).substr(eq + 1));
    if (name.rfind("WWE2K14_", 0) != 0 || name.size() > 64 || name.find_first_of(" \t") != std::string::npos) {
      REXLOG_WARN("[test-env] ignored '{}' (only WWE2K14_ names)", name);
      continue;
    }
    const char* before = std::getenv(name.c_str());
    const std::string old = before ? before : "<unset>";
    if (Set(name, value)) {
      REXLOG_WARN("[test-env] {}={} (launcher had {})", name, value.empty() ? "<unset>" : value, old);
      ++applied;
    } else {
      REXLOG_WARN("[test-env] failed to set {}", name);
    }
  }
}

// TEST-only timed exit (ARC-TESTER, 2026-11-11): unattended remote runs must end without a forced kill (a taskkill /F on
// the media server left the Intel driver degraded, memory no-force-kill-uhd). WWE2K14_TEST_EXIT_AFTER=<seconds> asks the
// game window to close after that many seconds; request_close runs the same Window::RequestClose a player's close button
// reaches (ReXApp::OnClosing: TerminateTitle, then the SDK's own close_exit_timeout_seconds exit). Unset = nothing starts.
void StartExitTimer(std::function<void()> request_close) {
  const char* e = std::getenv("WWE2K14_TEST_EXIT_AFTER");
  if (!e || !*e) return;
  const long s = std::strtol(e, nullptr, 10);
  if (s <= 0 || s > 86400) {
    REXLOG_WARN("[test-exit] WWE2K14_TEST_EXIT_AFTER='{}' ignored (expected 1..86400 seconds)", e);
    return;
  }
  REXLOG_WARN("[test-exit] window close requested after {} s (WWE2K14_TEST_EXIT_AFTER, test only)", s);
  std::thread([s, f = std::move(request_close)] {
    std::this_thread::sleep_for(std::chrono::seconds(s));
    REXLOG_WARN("[test-exit] {} s reached: requesting the window close", s);
    rex::FlushLogging();
    f();
  }).detach();
}

}  // namespace wwe2k14::test_env

#endif
#endif  // WWE2K14_NO_TEST_TOOLS
