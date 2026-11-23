#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "wide_core.h"

namespace wwe2k14::d3d::wide_probe {

enum class AspectReader : uint8_t {
  k8276B670,
  k827E9520,
  k827E9AD8,
  k827E9E40,
  k827EB9B8,
  k82BB6F78,
  k828469B0,
  kCount,
};

// Shared opt-in gate. The cached value leaves only the caller's conditional branch when disabled.
inline bool Enabled() {
  static const bool enabled = [] {
#ifdef _WIN32
    char* probe = nullptr;
    char* wide = nullptr;
    size_t probe_size = 0;
    size_t wide_size = 0;
    _dupenv_s(&probe, &probe_size, "WWE2K14_WIDE_PROBE");
    _dupenv_s(&wide, &wide_size, "WWE2K14_D3D_WIDE");
#else
    const char* probe = std::getenv("WWE2K14_WIDE_PROBE");
    const char* wide = std::getenv("WWE2K14_D3D_WIDE");
#endif
    const bool on = probe && std::strcmp(probe, "1") == 0 &&
                    wide_core::IsEnabled(wide_core::ParseK(wide));
#ifdef _WIN32
    std::free(probe);
    std::free(wide);
#endif
    return on;
  }();
  return enabled;
}

// Called at swap time after WIDE has selected the stretched in-match mode.
void NoteStretchedFrame();

// Opt-in W3d probe: count verified guest aspect readers and report bounded two-second windows.
void CountAspectReader(AspectReader reader);
void LogAspectReaderCounts();

// Called for eligible scene draws with the exact D3D-captured VS float block, whether squeezed or game-projected wide.
void ObserveSceneDraw(uint32_t frame, uint64_t shader_key, const uint8_t* constants,
                      const uint64_t* used_registers);

}  // namespace wwe2k14::d3d::wide_probe
