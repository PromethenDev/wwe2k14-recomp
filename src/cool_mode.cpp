// Public release builds (WWE2K14_TEST_TOOLS=OFF): test tool compiled to a no-op (owner rule 2026-10-26).
#ifdef WWE2K14_NO_TEST_TOOLS
namespace wwe2k14::cool {
void Apply() {}
}  // namespace wwe2k14::cool
#else
// TEST-only "Cool mode" (lane all-1104, owner 2026-11-04: in-match slowdown on the AMD 780M laptop, plugged in + Silent).
// Evidence: on battery the laptop runs the game's CPU work about 2x slower yet holds a steady 60 fps and almost never
// enters the slow state (1.5 % of match windows vs 19.7 % plugged in). WWE2K14_ECOQOS=1 asks Windows to treat this process
// as efficiency-class work (EcoQoS: SetProcessInformation / ProcessPowerThrottling EXECUTION_SPEED), the documented way
// for one process to give up high boost clocks without touching the user's power plan. Off unless the variable is set.
// Applied once in Wwe2k14App::OnPreSetup, after test_env::Apply.

#if !defined(REX_UWP)

#include <cstdlib>
#include <cstring>

#include <rex/logging.h>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace wwe2k14::cool {

void Apply() {
  const char* e = std::getenv("WWE2K14_ECOQOS");
  if (!e || std::strcmp(e, "1") != 0) return;
#if defined(_WIN32)
  using SetFn = BOOL(WINAPI*)(HANDLE, PROCESS_INFORMATION_CLASS, LPVOID, DWORD);
  const auto set_info =
      reinterpret_cast<SetFn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetProcessInformation"));
  if (!set_info) {
    REXLOG_WARN("[cool] SetProcessInformation not available: Cool mode off");
    return;
  }
  PROCESS_POWER_THROTTLING_STATE state{};
  state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
  state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
  state.StateMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
  const BOOL ok = set_info(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof(state));
  if (ok) {
    REXLOG_WARN("[cool] Cool mode ON: EcoQoS requested for the whole game process");
  } else {
    REXLOG_WARN("[cool] Cool mode request FAILED, error {}", GetLastError());
  }
#else
  REXLOG_INFO("[cool] WWE2K14_ECOQOS is Windows only: ignored");
#endif
}

}  // namespace wwe2k14::cool

#endif
#endif  // WWE2K14_NO_TEST_TOOLS
