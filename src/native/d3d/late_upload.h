// FL-4 "Design A" (WWE2K14_D3D_LATE_TEX=4): late texture reads without a per-draw wait.
//
// WWE2K14_D3D_LATE_TEX=1..3 waits for the CP thread (or the guest kick) at every draw whose texture the game is seen to
// rewrite after its D3D call but before the CP reaches the draw (movie planes, card S3-44) - a stall on the render path.
// Mode 4 records the level upload normally, but holds it (the backend's early submits are deferred) and re-reads the
// guest bytes at a release boundary - present (the game's own frame boundary) or, with WWE2K14_D3D_IRQ_SYNC=1, the GPU
// interrupt marker - which is when the console GPU would have read them. Only textures whose base is in draw.cpp's
// learned late-base set take part; nothing here runs unless WWE2K14_D3D_LATE_TEX=4.
//
// One source per state item: the registry copies the translate-time TextureKey / guest layout / format plan and the
// translate-time content hash (ne.hash at textures.cpp:1435); the release re-reads guest memory with the same
// sh::TextureHash and re-detiles with the same sh::ComputeLevelGeom / sh::DetileLevelCpu as translate. The upload
// bytes are written into the same staging buffer CreateTexture recorded them into, and only while that command list
// has not been submitted (the backend checks its own submission serial).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "../nr/nr_shared.h"

namespace wwe2k14::d3d::late_upload {

// WWE2K14_D3D_LATE_TEX=4, read once (like draw.cpp's other mode switches).
bool On();

// Card S3-44a: mode 4 and WWE2K14_D3D_LATE4_HITS != 0 (default on within mode 4): textures.cpp rebuilds a cache hit on
// a late base (instead of trusting it) so it registers here too.
bool HitsOn();

// draw.cpp's learned late-base set (g_late_bases, guarded by g_probe_mu) - the single source for "this base is late".
bool IsLateBase(uint32_t base_start);

// Called by textures.cpp right after a successful nb::CreateTexture for a late base. `levels` is the count the upload
// used; the release re-detiles `faces * per_face` levels starting at `first_level`. hash_lo/hash_hi are ne.hash.
void Register(uint32_t handle, const std::string& ckey, const nr::shared::TextureKeys::TextureKey& key,
              const rex::graphics::texture_util::TextureGuestLayout& layout, const nr::shared::TexFormatPlan& plan,
              uint32_t faces, uint32_t first_level, uint32_t per_face, size_t levels_count, uint64_t hash_lo,
              uint64_t hash_hi, bool forced_hit = false);

// Card S3-67 (WWE2K14_D3D_LATE_LEARN=d3d|census, defined in draw.cpp): resolves the release learner's translate-time
// probes; Release calls it after its refill loop on both paths. No-op in the default (cp) mode.
void LearnAtRelease();

// Release boundaries; the caller holds the backend lock (mode 2) and must re-submit afterwards for the bytes to reach
// the GPU. Present: the game's own swap. Irq: WWE2K14_D3D_IRQ_SYNC's GPU interrupt marker.
enum Why { kPresent, kIrq };
void Release(Why why);

}  // namespace wwe2k14::d3d::late_upload
