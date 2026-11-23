// WWE '13 native renderer (from WWE 2K15 PC): XenosRecomp's translator behind a plain interface.
// Compiled only into the wwe13_xenosrecomp library (cmake/native_renderer.cmake), with XenosRecomp's own
// pch.h, whose types (be<T>, ...) clash with the SDK's.
//
// WWE '13 (2026-11-17): some of its shaders make XenosRecomp read out of bounds (crash in
// ShaderRecompiler::recompile ~20 s after start). On Windows the translation runs under a structured
// exception handler: a shader that faults is reported as not converted (its draws stay skipped) instead of
// ending the game.

#include "shader_recompiler.h"

#include "xenos_translate.h"

#if defined(_WIN32)
#include <excpt.h>
#endif

namespace {

// Holds the C++ objects; the caller below has none, so it can use __try.
bool TranslateInner(const uint8_t *container, const char *prelude, size_t prelude_size, std::string *hlsl,
                    bool *pixel, uint32_t *spec_constants_mask) {
  ShaderRecompiler recompiler;
  recompiler.recompile(container, std::string_view(prelude, prelude_size));
  *hlsl = std::move(recompiler.out);
  *pixel = recompiler.isPixelShader;
  *spec_constants_mask = recompiler.specConstantsMask;
  return !hlsl->empty();
}

}  // namespace

bool Wwe13TranslateShader(const uint8_t *container, std::string_view prelude, std::string &hlsl,
                          bool &pixel, uint32_t &spec_constants_mask) {
  const char *p = prelude.data();
  const size_t n = prelude.size();
  std::string *out = &hlsl;
  bool *is_pixel = &pixel;
  uint32_t *mask = &spec_constants_mask;
#if defined(_WIN32)
  __try {
    return TranslateInner(container, p, n, out, is_pixel, mask);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
#else
  return TranslateInner(container, p, n, out, is_pixel, mask);
#endif
}
