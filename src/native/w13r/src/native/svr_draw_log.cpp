// Native renderer dev aid: everything one frame draws, in order.
//
//   SVR_DRAW_LOG=7000   (frame index, counted in presents; logs/game.log gets a [drawlog] line per
//                        draw, clear and resolve of that frame)
//   F9 in game          (logs the next frame, any time)
//
// Per draw: the game's render target / depth surface and the host target they map to, viewport,
// scissor, shaders, and every bound texture (the game's D3D texture and its host counterpart).

#include "svr_draw_log.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <string>

#include <fmt/format.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/d3d.h"
#include "gpu/device.h"
#include "gpu/resources.h"
#include "gpu/vertex_declaration.h"
#include "svr_resources.h"

namespace bd::gpu {
namespace {

u64 TargetFrame() {
  static const u64 frame = [] {
    const char *env = std::getenv("SVR_DRAW_LOG");
    return env ? std::strtoull(env, nullptr, 0) : 0ull;
  }();
  return frame;
}


std::string Host(const GuestTexture *t) {
  if (!t)
    return "-";
  return fmt::format("{}x{} f{}", t->width, t->height, u32(t->format));
}

u64 ShaderHash(const GuestShader *s) {
  return s && s->shaderCacheEntry ? s->shaderCacheEntry->hash : 0ull;
}

}  // namespace

// F9 in game logs the next frame (no frame number to guess). Polled once per frame.
static bool KeyFrameActive() {
  static std::atomic<u64> s_polled{~0ull};
  static std::atomic<u64> s_target{0};
  const u64 frame = SvrFrameIndex();
#if defined(_WIN32)
  const bool f9 = s_polled.exchange(frame) != frame && (GetAsyncKeyState(VK_F9) & 1);
#else
  const bool f9 = false;  // diagnostic hotkey: Windows only (the draw log cvars still work)
#endif
  if (f9) {
    s_target = frame + 1;
    BD_INFO("[drawlog] F9: logging frame {}", frame + 1);
  }
  return s_target.load() == frame;
}

bool SvrDrawLogActive() {
  const u64 target = TargetFrame();
  return (target && SvrFrameIndex() == target) || KeyFrameActive();
}

void SvrDrawLogDraw(u32 device_guest, const char *name, u32 primitive, bool indexed, u32 count,
                    u32 start, i32 base) {
  if (!SvrDrawLogActive())
    return;
  static u32 s_index = 0;
  const auto *dev = bd::mem::at<const D3DDevice>(device_guest);
  if (!dev)
    return;
  auto &s = state();
  std::string textures;
  for (u32 slot = 0; slot < kTextureSlots; ++slot) {
    if (!s.textures[slot])
      continue;
    // Texture fetch constant `slot` (6 dwords at device + 0x480): format, tiling, pitch, size.
    std::string fc;
    for (u32 d = 0; d < 6; ++d)
      fc += fmt::format("{}{:08X}", d ? "." : "", bd::mem::load<u32>(device_guest + 0x480u +
                                                                    slot * 24u + d * 4u));
    textures += fmt::format(" t{}[{} fc {}]", slot, Host(s.textures[slot]), fc);
  }
  // Register shadows: the 0x2100 block at +0x28CC, the 0x2200 block at +0x2934 (one dword each).
  const auto reg = [&](u32 r) -> u32 {
    const u32 off = r >= 0x2200 ? 0x2934u + (r - 0x2200u) * 4u : 0x28CCu + (r - 0x2100u) * 4u;
    return bd::mem::load<u32>(device_guest + off);
  };
  const auto regf = [&](u32 r) {
    const u32 v = reg(r);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
  };
  if (const auto *decl = s.vertex_declaration) {
    std::string elements;
    for (u32 i = 0; i < decl->inputElementCount; ++i) {
      const auto &e = decl->inputElements[i];
      if (e.slotIndex == 15)
        continue;
      elements += fmt::format(" {}{}:s{}+{}:f{}", e.semanticName ? e.semanticName : "?",
                              e.semanticIndex, e.slotIndex, e.alignedByteOffset, u32(e.format));
    }
    BD_INFO("[drawlog]   decl{}", elements);
  }
  BD_INFO("[drawlog]   blend 0x{:08X} colorctl 0x{:08X} alpharef {:.3f}", reg(0x2201), reg(0x2202),
          regf(0x210E));
  BD_INFO("[drawlog]   vte 0x{:X} clip 0x{:X} mode 0x{:X} depthctl 0x{:08X} colormask 0x{:X} "
          "ps-c13 {:.4g},{:.4g},{:.4g},{:.4g} ps-c14 {:.4g},{:.4g},{:.4g},{:.4g}",
          reg(0x2206), reg(0x2204), reg(0x2205), reg(0x2200), reg(0x2104),
          float(dev->psFloatConstants[13][0]), float(dev->psFloatConstants[13][1]),
          float(dev->psFloatConstants[13][2]), float(dev->psFloatConstants[13][3]),
          float(dev->psFloatConstants[14][0]), float(dev->psFloatConstants[14][1]),
          float(dev->psFloatConstants[14][2]), float(dev->psFloatConstants[14][3]));
  BD_INFO("[drawlog] #{} {} prim {} {} count {} start {} base {} | rt 0x{:08X}[{}] ds 0x{:08X}[{}] "
          "| vp {:.0f},{:.0f} {:.0f}x{:.0f} z {:.2f}-{:.2f} sc {},{}-{},{} | vs {:016X} ps {:016X} "
          "|{}",
          s_index++, name, primitive, indexed ? "indexed" : "", count, start, base,
          u32(dev->renderTargetShadow[0]), Host(s.render_target), u32(dev->depthStencilShadow),
          Host(s.depth_stencil), float(dev->viewport.X), float(dev->viewport.Y),
          float(dev->viewport.Width), float(dev->viewport.Height), float(dev->viewport.MinZ),
          float(dev->viewport.MaxZ), i32(dev->scissorRect.left), i32(dev->scissorRect.top),
          i32(dev->scissorRect.right), i32(dev->scissorRect.bottom), ShaderHash(s.vertex_shader),
          ShaderHash(s.pixel_shader), textures);
}

void SvrDrawLogResolve(u32 device_guest, u32 flags, u32 source_rect_va, u32 dest_va,
                       u32 dest_point_va) {
  if (!SvrDrawLogActive())
    return;
  const auto *dev = bd::mem::at<const D3DDevice>(device_guest);
  std::string rect = "full";
  if (const auto *r = bd::mem::try_at<const D3DRect>(source_rect_va))
    rect = fmt::format("{},{}-{},{}", i32(r->left), i32(r->top), i32(r->right), i32(r->bottom));
  std::string point = "0,0";
  if (const auto *p = bd::mem::try_at<const be_i32>(dest_point_va))
    point = fmt::format("{},{}", i32(p[0]), i32(p[1]));
  BD_INFO("[drawlog] resolve flags 0x{:X} from rt 0x{:08X} rect {} -> 0x{:08X} at {}", flags,
          dev ? u32(dev->renderTargetShadow[0]) : 0u, rect, dest_va, point);
}

void SvrDrawLogVertices(u32 data_va, u32 count, u32 stride) {
  if (!SvrDrawLogActive() || !data_va || !stride)
    return;
  for (u32 v = 0; v < count && v < 4; ++v) {
    std::string line;
    for (u32 k = 0; k < stride / 4 && k < 8; ++k)
      line += fmt::format(" {:08X}", bd::mem::load<u32>(data_va + v * stride + k * 4));
    BD_INFO("[drawlog]   v{}:{}", v, line);
  }
}

void SvrDrawLogClear(u32 device_guest, u32 count, u32 rects_va, u32 flags, u32 color) {
  if (!SvrDrawLogActive())
    return;
  const auto *dev = bd::mem::at<const D3DDevice>(device_guest);
  std::string rects;
  for (u32 i = 0; i < count && i < 8; ++i) {
    if (const auto *r = bd::mem::try_at<const D3DRect>(rects_va + i * 16u))
      rects += fmt::format(" {},{}-{},{}", i32(r->left), i32(r->top), i32(r->right),
                           i32(r->bottom));
  }
  BD_INFO("[drawlog] clear flags 0x{:X} color 0x{:08X} rt 0x{:08X} rects {}{}", flags, color,
          dev ? u32(dev->renderTargetShadow[0]) : 0u, count, rects);
}

}  // namespace bd::gpu
