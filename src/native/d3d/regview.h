// WP7 (decision 0006 stage 2): Xenos register view built ONLY from D3D device state blocks + draw arguments.
// Frozen interface (additions only after the freeze: DrawInfo, draw(), up_vertices(), SetShaderKeys()/shader_keys(),
// SyncFromState(), CountMismatches()). Worker thread.
//
// Sources, and nothing else (decision 0006 rule): (1) the device register-mirror blocks (device_state.h kMirrorBlocks,
// device offset = block base + 4*k <-> Xenos register first_reg + k, window-scissor dwords 17/18 of the 0x2000 block
// remapped to 0x2081/0x2082 exactly as ForEachMirroredRegister does); (2) the draw arguments of the hooked emitter
// (EvHeader::args) for the registers the emitter writes inline instead of through the mirror: VGT_INDX_OFFSET (0x2102,
// DrawIndexedVertices r5, BeginVertices 0), VGT_DRAW_INITIATOR (prim, count, source select), and for BeginVertices
// draws the inline vertex-fetch constant vf95 (regs 0x48BE/0x48BF). Every other register stays at the RegisterFile
// reset value (0); see kNotD3DSourced in regview.cpp for the registers the translation reads that no D3D source
// writes.
//
// Event payload layout assumed here (WP9 owner: confirm or change `kPayloadDwordsAreGuestOrder` in regview.cpp):
//   [n_blocks x DeltaBlock (uint16 offset_dw, uint16 count_dw), device-relative DWORD offsets]
//   [sum(count_dw) dwords, HOST order, in block order]
//   kDrawUP only: [r5*r6 bytes of vertex data (raw guest byte order) as written by the game between BeginVertices and
//                 EndVertices] (length = payload_bytes - the bytes above)
//   kAluLoad (ASSUMED, not defined by capture.h): args[0] = first Xenos register index, args[1] = dword count, payload
//                 = args[1] host-order dwords, no DeltaBlocks (n_blocks 0).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "device_state.h"
#include "shadow_types.h"

namespace rex::graphics {
class RegisterFile;
}

namespace wwe2k14::d3d {

// Arguments of the draw event last applied (ApplyDrawArgs).
struct DrawInfo {
  bool valid = false;
  EvKind kind = EvKind::kDraw;
  uint32_t seq = 0, packet_addr = 0xFFFFFFFFu;
  uint32_t prim = 0;           // Xenos primitive type (r4, written unchanged into the draw initiator by the game)
  bool indexed = false;        // kDraw (DrawIndexedVertices) vs kDrawUP (BeginVertices auto index)
  uint32_t base_vertex = 0;    // kDraw r5 -> VGT_INDX_OFFSET
  uint32_t start_index = 0;    // kDraw r6
  uint32_t count = 0;          // kDraw r7 index count / kDrawUP r5 vertex count
  uint32_t up_stride_bytes = 0;  // kDrawUP r6
};

class RegView {
 public:
  RegView();
  ~RegView();
  RegView(const RegView&) = delete;
  RegView& operator=(const RegView&) = delete;
  // Apply the delta blocks of an event (payload follows the DeltaBlock list as in EvHeader). Any EvKind: state deltas
  // are applied for all of them; kDrawUP also keeps its vertex bytes, kAluLoad its register range.
  void Apply(const EvHeader& e, const uint8_t* payload);
  // Apply draw arguments (VGT_INDX_OFFSET = r5, draw initiator, inline vf95 of BeginVertices draws). Call after Apply
  // of the same event. No-op for kinds other than kDraw / kDrawUP.
  void ApplyDrawArgs(const EvHeader& e);
  const rex::graphics::RegisterFile& regs() const;

  // --- additions ---
  const DrawInfo& draw() const;
  // kDrawUP vertex bytes of the last Apply (raw guest byte order); null/0 if none.
  const uint8_t* up_vertices(size_t* bytes) const;
  // Per-draw shader identity. The worker sets these from the event (capture records CurrentShaderKey(0/1) at the
  // emitter's Pre); TranslateDraw fails ("no shader key") if the VS key is not set.
  void SetShaderKeys(const ShaderKey& vs, const ShaderKey& ps);
  bool shader_keys(ShaderKey* vs, ShaderKey* ps) const;
  // Refill every mirrored register from a full DeviceState (FULLSNAP / device reset / verification).
  void SyncFromState(const DeviceState& s);
  // Number of mirrored registers whose value here differs from `s` (the delta stream's consistency check). `first`
  // (optional) receives a description of the first difference.
  uint32_t CountMismatches(const DeviceState& s, std::string* first) const;
  // Card S3-68 (dirty-group translation): register groups whose value changed since the last TakeDirty() (every write
  // this view receives compares the old value: Apply delta blocks and ALU loads, ApplyDrawArgs; SyncFromState marks
  // everything). Bookkeeping only (the mask is not register state), hence const; worker thread.
  //   kDirtyRs: render-state registers 0x2000-0x23FF except the per-draw index registers VGT_MAX/MIN_VTX_INDX,
  //             VGT_INDX_OFFSET, VGT_MULTI_PRIM_IB_RESET_INDX (0x2100-0x2103) and VGT_DRAW_INITIATOR (0x21FC);
  //   DirtySlot(s): fetch-constant slot s (registers 0x4800 + 6 s .. + 5; vertex fetch f lives in slot f / 3).
  static constexpr uint64_t kDirtyRs = 1ull;
  static constexpr uint64_t DirtySlot(uint32_t slot) { return 1ull << (32 + slot); }
  uint64_t TakeDirty() const;

 private:
  struct Impl;
  Impl* impl_ = nullptr;
};

}  // namespace wwe2k14::d3d
