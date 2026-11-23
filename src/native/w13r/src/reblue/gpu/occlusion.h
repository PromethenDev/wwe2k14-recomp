/**
 * @file    gpu/occlusion.h
 * @brief   Sun visibility occlusion query, behind the lens flare.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once

#include <plume_render_interface.h>
#include <rex/types.h>

namespace bd::gpu {

class Occlusion {
public:
  // Begin and End bracket the sun test quad draw.
  static void Begin();
  static void End();

  // Feeds D3DQuery_GetData.
  static u32 Count();

  // The counting PS the pipeline cache swaps in while a query is open.
  static plume::RenderShader *CountPS();
};

} // namespace bd::gpu
