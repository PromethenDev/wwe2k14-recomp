// Host test of card GAP-pass2: PlanFor, against plume's real format enum. Build:
//   g++ -std=c++20 -Wall -Wextra -Werror -I<lane>/src/native/nr -I<lane>/plume src/native/nr/tests/rt-view-formats-test.cpp
#include <cstdio>

#include "plume_render_interface_types.h"
#include "rt_view_formats.h"

using F = plume::RenderFormat;
using nr_rt_views::PlanFor;

static int g_fail = 0;
static void Check(bool ok, const char* what) {
  if (!ok) {
    std::printf("FAIL %s\n", what);
    ++g_fail;
  }
}

int main() {
  const F u = F::UNKNOWN;
  // Off: exactly the old behaviour.
  Check(!PlanFor(false, false, F::R8G8B8A8_UNORM, u, u).known, "off keeps mutable");
  Check(!PlanFor(false, false, F::R16G16_SNORM, F::R16G16_UINT, u).known, "off keeps mutable, 16 bit");
  // D3D12 has no such flag; its typeless path stays untouched.
  Check(!PlanFor(true, true, F::R16G16_SNORM, F::R16G16_UINT, u).known, "d3d12 untouched");
  // RGBA8 has no integer view (IntegerFormat returns UNKNOWN): viewed only as itself.
  auto rgba8 = PlanFor(true, false, F::R8G8B8A8_UNORM, u, u);
  Check(rgba8.known && rgba8.second == u, "rgba8 not mutable");
  // Every target format with an integer view names exactly that view.
  const F pairs[][2] = {{F::R16G16_SNORM, F::R16G16_UINT},
                        {F::R16G16_FLOAT, F::R16G16_UINT},
                        {F::R16G16B16A16_SNORM, F::R16G16B16A16_UINT},
                        {F::R16G16B16A16_FLOAT, F::R16G16B16A16_UINT},
                        {F::R32_FLOAT, F::R32_UINT},
                        {F::R32G32_FLOAT, F::R32G32_UINT}};
  for (const auto& p : pairs) {
    auto plan = PlanFor(true, false, p[0], p[1], u);
    Check(plan.known && plan.second == p[1], "integer partner listed");
  }
  // A partner equal to the own format is no second format.
  Check(PlanFor(true, false, F::R32_UINT, F::R32_UINT, u).second == u, "same format is not a second view");
  // Fixed16 lean target: only ever its own UNORM format.
  auto lean = PlanFor(true, false, F::R16G16_UNORM, u, u);
  Check(lean.known && lean.second == u, "f16 lean target not mutable");
  std::printf(g_fail ? "rt-view-formats: FAIL\n" : "rt-view-formats: PASS\n");
  return g_fail ? 1 : 0;
}
