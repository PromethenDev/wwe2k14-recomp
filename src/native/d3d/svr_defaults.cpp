// SVR-style renderer defaults (owner decision 2026-10-19 morning: "fully convert"; docs/PLAN-SVR-FULL.md track B).
// Keep the accepted step 7 / step 8a defaults ON; each switch remains an owner A/B opt-out (e.g.
// WWE2K14_D3D_DEST_IMAGES=0). B6-6 retires unrelated renderer selectors, not this defaults shim.
//   step 6  movie frames in order  (card S3-67) is NOT a default: UHD 630 pairs 2026-10-19 showed worse 30 fps pacing
//           with it (on-grid30 36-38 % vs 51-54 % without); opt in with WWE2K14_D3D_LATE_TEX=4 WWE2K14_D3D_LATE_LEARN=d3d.
//   step 7  textures after match 1 (card S3-69):  WWE2K14_D3D_DEST_IMAGES=1, _DEST_CANVAS=1, _DEST_OBJ_BYPASS=1
//   step 8a no ring state loads    (card S3-70):  WWE2K14_D3D_RING_LOADS=0
//   S5-1    resolves into persistent textures (card S5-1, owner OK 2026-10-21; UHD 630 480p locked 30: 4/4 pairs
//           better, gaps>=70 ms 76 -> 48 per 600 s): WWE2K14_D3D_RESOLVE_INTO_TEX=1
//   S3-71   NULL-rect resolves clipped to the D3D copy destination (Heenan's runtime-built attire; owner OK 2026-10-21):
//           WWE2K14_D3D_RESOLVE_CLAMP_DEST=1
//   De-emulation pieces 2-4 + lean conversions (owner test package 1022b, 2026-10-22): indexed/UP/clear-helper DRAWs ->
//           ordering markers, guest state uploads removed, tag-guided scan (WWE2K14_D3D_HLE_INDEXED=1, _STATE_UPLOAD=0,
//           _TAG_SCAN=1); fixed16 + depth resolve conversions without staging buffers/copies, CHECKed bit-exact
//           (WWE2K14_D3D_F16_LEAN=1, WWE2K14_D3D_DEPTH_LEAN=1)
//   Piece 5 step 1 (card DE-5A, 2026-10-22 evening): no capture ring-window walk / gate address map (CHECK: 0 consumed outputs;
//           full lane-A match normal): WWE2K14_D3D_RING_WALK=0
// WWE2K14_D3D_SVR_DEFAULTS=0 bypasses these defaults globally; the individual switches remain independently overrideable.
// Applied by a static initializer, i.e. before main() and before any switch is read (every reader caches its value in
// a function-local static on first use, after main has started).
#include <cstdlib>

namespace wwe2k14::d3d {
namespace {

void SetDefault(const char* name, const char* value) {
  if (std::getenv(name)) return;  // explicit launcher/user value wins
#if defined(_WIN32)
  _putenv_s(name, value);
#else
  setenv(name, value, 0);
#endif
}

struct SvrDefaults {
  SvrDefaults() {
    const char* off = std::getenv("WWE2K14_D3D_SVR_DEFAULTS");
    if (off && off[0] == '0') return;
    SetDefault("WWE2K14_D3D_DEST_IMAGES", "1");
    SetDefault("WWE2K14_D3D_DEST_CANVAS", "1");
    SetDefault("WWE2K14_D3D_DEST_OBJ_BYPASS", "1");
    SetDefault("WWE2K14_D3D_RING_LOADS", "0");
    SetDefault("WWE2K14_D3D_RESOLVE_INTO_TEX", "1");
    SetDefault("WWE2K14_D3D_RESOLVE_CLAMP_DEST", "1");
    SetDefault("WWE2K14_D3D_HLE_INDEXED", "1");
    SetDefault("WWE2K14_D3D_STATE_UPLOAD", "0");
    SetDefault("WWE2K14_D3D_TAG_SCAN", "1");
    SetDefault("WWE2K14_D3D_TRUST_GROUP", "0x7F");  // N2-GT 11-18: gt-check 24.5 M scans 0 miss, full scans/f 276 -> ~13
    SetDefault("WWE2K14_D3D_F16_LEAN", "1");
    SetDefault("WWE2K14_D3D_DEPTH_LEAN", "1");
    SetDefault("WWE2K14_D3D_RING_WALK", "0");
    SetDefault("WWE2K14_NR_MEM_BUDGET", "2");
    SetDefault("WWE2K14_D3D_NATIVE_EFA4", "1");  // V9b: native EFA4 material PS, T3 exact, UHD -24.6 %/draw (falls back if absent)  // card U2: keep device-local heaps inside the driver budget (780M fix)
  }
};
const SvrDefaults g_svr_defaults;

}  // namespace
}  // namespace wwe2k14::d3d
