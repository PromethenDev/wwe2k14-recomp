// SDK observer adapter. D3D-owned lifecycle and draw-off behavior live in native/d3d/runtime.cpp.
#include <rex/graphics/draw_observer.h>

#include <cstdlib>
#include <cstdio>
#include <cstring>

#include "../d3d/present.h"
#include "../d3d/runtime.h"

namespace nr {
namespace {

class Observer final : public rex::graphics::DrawObserver {
 public:
  void OnDraw(const rex::graphics::ObservedDraw& draw) override { wwe2k14::d3d::runtime::Get().OnDraw(draw); }
  void OnCompareFrame(bool begin, uint32_t frame) override {
    wwe2k14::d3d::runtime::Get().OnCompareFrame(begin, frame);
  }
  void OnSwap(uint32_t frame) override { wwe2k14::d3d::runtime::Get().OnSwap(frame); }
  void OnGuestMemoryWritten(uint32_t base, uint32_t length) override {
    wwe2k14::d3d::runtime::Get().OnGuestMemoryWritten(base, length);
  }
  void OnBeforeSwap() override { wwe2k14::d3d::runtime::Get().OnBeforeSwap(); }
  void OnCpuVisibility(uint32_t base, uint32_t length) override {
    wwe2k14::d3d::runtime::Get().OnCpuVisibility(base, length);
  }
  void OnBeforeInterrupt(uint32_t interrupt) override {
    wwe2k14::d3d::runtime::Get().OnBeforeInterrupt(interrupt);
  }
};

Observer observer;

void RequireModeSwitch(const char* name, const char* required) {
  const char* current = std::getenv(name);
  if (current && std::strcmp(current, required) != 0) {
    // Registration runs once during static initialization, before any renderer callback can cache these values.
    std::fprintf(stderr, "[nr] %s=%s ignored while WWE2K14_NR selects the D3D-fed renderer; using %s\n", name,
                 current, required);
  }
#ifdef _WIN32
  if (_putenv_s(name, required) != 0) {
    std::fprintf(stderr, "[nr] failed to require %s=%s for the D3D-fed renderer\n", name, required);
  }
#else
  if (setenv(name, required, 1) != 0) {
    std::fprintf(stderr, "[nr] failed to require %s=%s for the D3D-fed renderer\n", name, required);
  }
#endif
}

// WWE2K14_NR is the sole renderer selector. With no selection, leave the SDK's emulated renderer and its observer
// registration untouched. The selected D3D-fed path requires all four transitional mode values below.
void RegisterFromEnvironment() {
  static bool done = false;
  if (done) return;
  const char* mode = std::getenv("WWE2K14_NR");
  if (!mode || (std::strcmp(mode, "shadow") != 0 && std::strcmp(mode, "replace") != 0)) return;
  done = true;
  {
    RequireModeSwitch("WWE2K14_D3D_OBSERVE", "1");
    RequireModeSwitch("WWE2K14_D3D_SHADOW", "2");
    RequireModeSwitch("WWE2K14_NR_DRAW_OFF", "1");
    RequireModeSwitch("WWE2K14_NR_PRESENT_D3D", "1");
    rex::graphics::SetDrawObserver(&observer);
    // Card DE-2M: indexed-draw ordering markers reach the gate through the SDK NOP hook. The switch is read at the first
    // NOP (after main: svr_defaults' static initializer may set it, and static initialization order across files is not
    // defined); off, the hook returns at once.
    rex::graphics::SetNrNopHook([](const rex::graphics::NrNopEvent& e) {
      static const bool on = [] {
        const char* hle = std::getenv("WWE2K14_D3D_HLE_INDEXED");
        return hle && std::strcmp(hle, "1") == 0;
      }();
      if (on) wwe2k14::d3d::runtime::Get().OnNop(e);
    });
  }
}

#if !defined(REX_UWP)
// PC: the launcher sets the environment before the process starts, so register before main().
struct Registration {
  Registration() { RegisterFromEnvironment(); }
} registration;
#endif

}  // namespace

// Xbox (UWP, XB1 2026-11-09): the environment is filled from the package's native-env.txt in OnPreSetup, after static
// initialization, so src/uwp_env.cpp registers here once it has applied the settings.
void RegisterFrontendFromEnvironment() { RegisterFromEnvironment(); }
}  // namespace nr
