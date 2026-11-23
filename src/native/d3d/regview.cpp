// WP7: see regview.h. Worker thread. Writes only its own RegisterFile.
#include "regview.h"
#include "capture.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>

namespace wwe2k14::d3d {

namespace {
// Event payload dwords: false = host order (as DeviceState stores them), true = raw guest (big-endian) words.
constexpr bool kPayloadDwordsAreGuestOrder = false;

// Registers the nr state decode reads (nr_frontend.cpp Draw) that NO D3D source writes: they keep the RegisterFile
// reset value. Listed for the WP7 card (docs/cards) and for the `[d3d-draw]` startup line.
//   PA_SC_WINDOW_OFFSET 0x2080 (tile window offset; the D3D side has no tiling, window offset 0 = untiled draw)
//   PA_SC_VIZ_QUERY / VGT_MULTI_PRIM_IB_RESET_INDX etc. are in the mirror (0x2100 block) and ARE sourced.
// Register 0x2081/0x2082 (window scissor) is sourced from mirror dwords 17/18 of the 0x2000 block.
constexpr uint32_t kNotD3DSourced[] = {0x2080};
[[maybe_unused]] constexpr size_t kNotD3DSourcedCount = sizeof(kNotD3DSourced) / sizeof(kNotD3DSourced[0]);

constexpr uint32_t kRegVgtIndxOffset = 0x2102;
constexpr uint32_t kRegVgtDrawInitiator = 0x21FC;
constexpr uint32_t kRegFetchBase = 0x4800;

// dev dword index -> Xenos register (0 = not a mirrored register). Built once from the device_state.cpp mapping
// (ForEachMirroredRegister walks the blocks in kMirrorBlocks order, dwords in order), so the block table and the scissor
// remap have ONE definition.
struct DevMap {
  std::vector<uint32_t> reg;  // indexed by device dword offset
  DevMap() {
    DeviceState zero;
    uint32_t last = 0;
    for (uint32_t i = 0; i < kMirrorBlockCount; ++i) last = std::max(last, (kMirrorBlocks[i].dev_offset >> 2) + kMirrorBlocks[i].dwords);
    reg.assign(last, 0);
    uint32_t b = 0, k = 0;
    ForEachMirroredRegister(zero, [&](uint32_t r, uint32_t) {
      while (b < kMirrorBlockCount && k >= kMirrorBlocks[b].dwords) { ++b; k = 0; }
      reg[(kMirrorBlocks[b].dev_offset >> 2) + k] = r;
      ++k;
    }, true);
  }
};
const DevMap& Map() {
  static const DevMap m;
  return m;
}
// Value transform per register as ForEachMirroredRegister applies it (window scissor, device_state.cpp).
inline uint32_t Xform(uint32_t reg, uint32_t v) {
  if (reg == 0x2081 || reg == 0x2082) return (v & 0x3FFFu) | (v & 0x3FFF0000u);
  return v;
}
inline uint32_t Dw(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return kPayloadDwordsAreGuestOrder ? __builtin_bswap32(v) : v;
}
}  // namespace

struct RegView::Impl {
  rex::graphics::RegisterFile regs;
  DrawInfo draw;
  std::vector<uint8_t> up;
  ShaderKey vs, ps;
  bool keys = false;
  uint64_t dirty = ~0ull;  // card S3-68: groups changed since the last TakeDirty (all at start)
  // Card S3-68: every register write goes through here; a changed value marks its group dirty.
  void Write(uint32_t reg, uint32_t v) {
    uint32_t& r = regs.values[reg];
    if (r == v) return;
    r = v;
    if (reg >= 0x2000 && reg < 0x2400) {
      if (!((reg >= 0x2100 && reg <= 0x2103) || reg == 0x21FC)) dirty |= RegView::kDirtyRs;
    } else if (reg >= kRegFetchBase && reg < kRegFetchBase + 32 * 6) {
      dirty |= RegView::DirtySlot((reg - kRegFetchBase) / 6);
    }
  }
};

RegView::RegView() : impl_(new Impl()) {}
RegView::~RegView() { delete impl_; }

void RegView::Apply(const EvHeader& e, const uint8_t* payload) {
  Impl& m = *impl_;
  if (e.kind == EvKind::kDrawUP) m.up.clear();
  if (!payload) return;
  if (e.kind == EvKind::kAluLoad && e.n_blocks == 0) {  // ASSUMED format, see regview.h
    const uint32_t first = e.args[0], n = e.args[1];
    if (first + n <= rex::graphics::RegisterFile::kRegisterCount && n * 4 <= e.payload_bytes)
      for (uint32_t i = 0; i < n; ++i) m.Write(first + i, Dw(payload + 4 * i));
    return;
  }
  const uint8_t* blocks = payload;
  const uint8_t* data = payload + size_t(e.n_blocks) * sizeof(DeltaBlock);
  const std::vector<uint32_t>& map = Map().reg;
  for (uint32_t i = 0; i < e.n_blocks; ++i) {
    DeltaBlock b;
    std::memcpy(&b, blocks + i * sizeof(DeltaBlock), sizeof(b));
    for (uint32_t k = 0; k < b.count_dw; ++k) {
      const uint32_t dw = uint32_t(b.offset_dw) + k;
      if (dw < map.size() && map[dw]) m.Write(map[dw], Xform(map[dw], Dw(data + 4 * k)));
    }
    data += size_t(b.count_dw) * 4;
  }
  if (e.kind == EvKind::kDrawUP) {
    // WP9 layout (capture.h): vertex bytes live in the kind data (UpExt header, then copied_bytes) after EvExt.
    const EvView v = ParseEvent(e, payload);
    if (v.ok && v.ext->kind_bytes >= sizeof(UpExt)) {
      UpExt up;
      std::memcpy(&up, v.kind, sizeof up);
      if (up.copied_bytes && sizeof(UpExt) + uint64_t(up.copied_bytes) <= v.ext->kind_bytes)
        m.up.assign(v.kind + sizeof(UpExt), v.kind + sizeof(UpExt) + up.copied_bytes);
    }
  }
}

void RegView::ApplyDrawArgs(const EvHeader& e) {
  Impl& m = *impl_;
  if (e.kind != EvKind::kDraw && e.kind != EvKind::kDrawUP) return;
  DrawInfo& d = m.draw;
  d = DrawInfo();
  d.valid = true;
  d.kind = e.kind;
  d.seq = e.seq;
  d.packet_addr = e.packet_addr;
  d.prim = e.args[0] & 0x3F;
  if (e.kind == EvKind::kDraw) {
    // DrawIndexedVertices (85:15843-15853): type-0 write of r5 to 0x2102, then DRAW_INDX with source = DMA.
    d.indexed = true;
    d.base_vertex = e.args[1];
    d.start_index = e.args[2];
    d.count = e.args[3];
    m.Write(kRegVgtIndxOffset, d.base_vertex);
    m.Write(kRegVgtDrawInitiator, ((d.count & 0xFFFF) << 16) | d.prim);  // index-size bit: TranslateDraw (IB object)
  } else {
    // BeginVertices (85:15060-15300): VGT_INDX_OFFSET 0, DRAW_INDX auto index (ori 128), inline vf95: dword4 = address|3
    // (the address is the ring reservation; the shadow reads the captured bytes instead), dword5 = 0x10000002 |
    // (size_in_dwords << 2 & 0x3FFFFFC), size = (r5*r6) >> 2.
    d.indexed = false;
    d.count = e.args[1];
    d.up_stride_bytes = e.args[2];
    const uint32_t size_dw = uint32_t((uint64_t(e.args[1]) * e.args[2]) >> 2);
    m.Write(kRegVgtIndxOffset, 0);
    m.Write(kRegVgtDrawInitiator, ((d.count & 0xFFFF) << 16) | d.prim | 128u);
    m.Write(kRegFetchBase + 95 * 2, 3);
    m.Write(kRegFetchBase + 95 * 2 + 1, 0x10000002u | ((size_dw << 2) & 0x3FFFFFCu));
  }
}

const rex::graphics::RegisterFile& RegView::regs() const { return impl_->regs; }
const DrawInfo& RegView::draw() const { return impl_->draw; }
const uint8_t* RegView::up_vertices(size_t* bytes) const {
  if (bytes) *bytes = impl_->up.size();
  return impl_->up.empty() ? nullptr : impl_->up.data();
}
void RegView::SetShaderKeys(const ShaderKey& vs, const ShaderKey& ps) {
  impl_->vs = vs;
  impl_->ps = ps;
  impl_->keys = true;
}
bool RegView::shader_keys(ShaderKey* vs, ShaderKey* ps) const {
  if (!impl_->keys) return false;
  if (vs) *vs = impl_->vs;
  if (ps) *ps = impl_->ps;
  return true;
}

void RegView::SyncFromState(const DeviceState& s) {
  ForEachMirroredRegister(s, [&](uint32_t reg, uint32_t v) { impl_->regs.values[reg] = v; }, true);
  impl_->dirty = ~0ull;  // card S3-68
}

uint64_t RegView::TakeDirty() const {
  const uint64_t m = impl_->dirty;
  impl_->dirty = 0;
  return m;
}

uint32_t RegView::CountMismatches(const DeviceState& s, std::string* first) const {
  uint32_t n = 0;
  ForEachMirroredRegister(s, [&](uint32_t reg, uint32_t v) {
    // Registers the draw arguments override (inline writes) are not compared.
    if (reg == kRegVgtIndxOffset) return;
    if (reg >= kRegFetchBase + 95 * 2 && reg <= kRegFetchBase + 95 * 2 + 1 && !impl_->draw.indexed) return;
    if (impl_->regs.values[reg] != v) {
      if (n == 0 && first) {
        char b[96];
        std::snprintf(b, sizeof(b), "reg 0x%04X view=0x%08X state=0x%08X", reg, impl_->regs.values[reg], v);
        *first = b;
      }
      ++n;
    }
  }, true);
  return n;
}

}  // namespace wwe2k14::d3d
