// D3D-fed renderer lifecycle hosted by the D3D module. Packet draw translation is not part of this interface.
#pragma once

#include <cstddef>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include <rex/graphics/draw_observer.h>
#include <rex/system/xmemory.h>

namespace wwe2k14::d3d::runtime {

// Installed as the SDK DrawObserver by nr_frontend's static registration. All GPU state used to render is still
// captured from the game's D3D calls; these callbacks only coordinate the D3D worker, shared backend and SDK present.
class Runtime final {
 public:
  Runtime();

  void OnDraw(const rex::graphics::ObservedDraw& draw);
  void OnNop(const rex::graphics::NrNopEvent& event);  // card DE-2M
  void OnSwap(uint32_t frame);
  void OnBeforeSwap();
  void OnGuestMemoryWritten(uint32_t physical_base, uint32_t length);
  void OnCpuVisibility(uint32_t physical_base, uint32_t length);
  void BootFromWorker();  // F4 row 19 (WWE2K14_D3D_NATIVE_BOOT=1): no CP event needed to start the backend
  void OnBeforeInterrupt(uint32_t interrupt);

  // Packet-only observer events intentionally have no D3D-runtime work.
  void OnCompareFrame(bool begin, uint32_t frame);

 private:
  void EnsureBackend(uint32_t ordinal);
  void PresentD3dOnly();
  void GuestWrite(uint32_t start, const std::vector<uint8_t>& bytes);
  void MaybeShot(uint32_t snapshot, uint32_t width, uint32_t height, const uint8_t swizzle[4]);
  void EmitProfile(uint32_t swap);

  rex::memory::Memory* memory_ = nullptr;
  std::atomic<bool> draw_off_init_{false};  // F4: the worker may start the backend before the CP sees its first event
  uint64_t torn_markers_ = 0;  // card DE-2M
  bool skip_set_ = false;
  bool backend_init_failed_ = false;
  bool sampler_started_ = false;
  uint32_t swap_ordinal_ = 0;
  uint64_t d3d_present_ordinal_ = 0;
  bool d3d_frame_ready_ = false;
  uint64_t profile_frame_ = 0;
  uint64_t profile_frame_start_ns_ = 0;
  uint64_t s3_d3d_ = 0, s3_fb_gate_ = 0, s3_fb_missing_ = 0;
  uint64_t s3_off_draws_ = 0, s3_cpu_vis_ = 0, s3_cpu_vis_served_ = 0, s3_cpu_vis_refused_ = 0;
  uint64_t s3_swz_missing_ = 0;
  uint64_t cpu_vis_wait_ns_ = 0;
  int cpu_vis_wait_status_ = 3;
  int shots_written_ = 0;
  long long shot_next_s_ = 0;
  static constexpr uint64_t kFlashInfo = 64;
  std::string flash_info_[kFlashInfo];
  uint64_t flash_presents_ = 0;
};

Runtime& Get();

// Called by sub_82CB6318 only while WWE2K14_NR_FIX_ALT is active.
void NoteFixAltGuestFlip(uint64_t delta_us, uint32_t interval);

}  // namespace wwe2k14::d3d::runtime
