// Card GAP-pass2 (WWE2K14_NR_RT_VIEW_FORMATS=1, default off): which view formats a colour target declares at creation.
// Vulkan only. A colour target made mutable with no format list may lose lossless framebuffer compression on some drivers
// (candidate on Intel and AMD integrated GPUs; the speed effect is what the switch measures). No format is removed from any
// reader: the only views of a colour target are its own format and, for 16- and 32-bit targets, IntegerFormat().
#pragma once

namespace nr_rt_views {

template <class Format>
struct Plan {
  bool known = false;     // false: the old behaviour (mutable, no list)
  Format second = Format{};  // the only other view format; `unknown` = viewed only as itself
};

template <class Format>
constexpr Plan<Format> PlanFor(bool switch_on, bool d3d12, Format own, Format integer, Format unknown) {
  Plan<Format> p;
  p.second = unknown;
  if (!switch_on || d3d12) return p;
  p.known = true;
  if (integer != unknown && integer != own) p.second = integer;
  return p;
}

}  // namespace nr_rt_views
