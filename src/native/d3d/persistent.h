// WP11 (decision 0006 stage 2): handling of the boot-recorded persistent command region (physical 0x10FEE000-0x10FF0000).
// The plan names only the file; this header is a placeholder freeze. WP11 fills it in only if its report concludes
// outcome (b) or (c) (docs/reports/20261014-d3d-persistent-region.md). Stub: nothing replayed.
#pragma once

#include <cstdint>

namespace wwe2k14::d3d::persistent {

// True once WP11 handling is active (outcome b/c); false while the region is documented as ignored/inert.
bool Active();
// Worker: called at each 0x82CB4848 (SetShaderGPRAllocation) event to replay/apply the recorded boot state.
void OnShaderGprCall(uint32_t seq);

}  // namespace wwe2k14::d3d::persistent
