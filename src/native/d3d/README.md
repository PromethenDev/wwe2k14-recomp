# src/native/d3d — D3D-fed renderer (decision 0006)

Stage 1 (observe only) / stage 2 (shadow drawing). Nothing here changes the shown picture until stage 3.
Ownership (one work package per file set; do not edit another package's files):
- WP1 hooks + counters + thread timing: `hooks.cpp`, `hooks.h`
- WP2 device-state reader: `device_state.h`, `device_state.cpp`
- WP3 per-draw register-file cross-check: `crosscheck.cpp`, `crosscheck.h` (+ the agreed hook-in point in nr_frontend.cpp)
- WP4 coverage census (offline): `tools/d3d/`
- WP5 shader identity: `shader_identity.cpp`, `shader_identity.h`
Stage 2 (shadow drawing; plan docs/reports/20261014-d3d-stage2-plan.md, interfaces frozen at step 0 by the integrator;
header changes after the freeze go through the integrator; every `.cpp` below is a stub until its package replaces it):
- integrator: `shadow_types.h` and all frozen headers
- WP0: `src/native/nr/nr_shared.{h,cpp}`, nr_frontend.cpp call sites (only package editing src/native/nr)
- WP5-impl: `shader_identity.{h,cpp}`
- WP6: `targets.{h,cpp}`, `resolves.{h,cpp}`
- WP7: `regview.{h,cpp}`, `draw.{h,cpp}`, `draw_compare.{h,cpp}`
- WP8: `textures.{h,cpp}`
- WP9: `capture.{h,cpp}`, `worker.{h,cpp}`, `gate.{h,cpp}`, `tools/d3d/capture-stress*` (+ the two Shadow lines in hooks.cpp via WP1 owner)
- WP10: `present.{h,cpp}`, `tools/d3d/shadow-diff.py`
- WP11: `persistent.{h,cpp}` (only if needed) + report
- WP12: `sidefile.{h,cpp}`, `tools/d3d/sidefile-check.py`
Switch: `WWE2K14_D3D_SHADOW=1|2` (needs `WWE2K14_D3D_OBSERVE=1`), default off.
Rules: decision 0006 (one state source; register file only as a read-only cross-check), owner reuse rule (SVR code
read for method, never copied). All runtime switches `WWE2K14_D3D_*`, default off.
