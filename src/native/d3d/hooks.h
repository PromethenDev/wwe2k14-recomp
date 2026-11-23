// WP1 (decision 0006 stage 1): observe-only hooks on the guest D3D library. WWE2K14_D3D_OBSERVE=1 enables them;
// with the switch off every hook is one predictable branch and then the original guest function.
#pragma once

#include <cstdint>

namespace wwe2k14::d3d {

// True when WWE2K14_D3D_OBSERVE=1 (read once).
bool ObserveEnabled();
// In-game FPS counter: game frames so far (Swap calls) and the longest frame since the previous call (then reset).
void GameFrameStats(uint64_t* swaps, uint64_t* max_gap_ns);

}  // namespace wwe2k14::d3d
