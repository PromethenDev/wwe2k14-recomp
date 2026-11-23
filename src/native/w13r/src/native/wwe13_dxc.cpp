// wwe13_d3d12.exe: whether DXC (dxcompiler.dll, delay-loaded) is next to the exe. The shipped exe
// has none: its spec-constant shaders come pre-linked (wwe13_prelink), and a variant missing from
// that cache is not drawn instead of being linked (guest_shaders.cpp). Dev builds may have it.
// From WWE '12 PC (native/wwe12_dxc.cpp).

#if defined(_WIN32) && defined(REBLUE_D3D12) && !defined(REBLUE_D3D11)
#include <windows.h>

bool Wwe13DxcAvailable() {
  static const bool available =
      LoadLibraryExW(L"dxcompiler.dll", nullptr,
                     LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32) != nullptr;
  return available;
}
#endif
