// WP9 (decision 0006 stage 2): CP-side gate so the command processor does not run past a draw before the shadow worker
// has read that draw's guest data. Changes CP timing only. Frozen interface: GateCpDraw / GateCpSwap.
//
// Hook-in rules for the nr frontend (WP0):
//   * call GateCpDraw(draw.packet_guest_address) at the TOP of Frontend::OnDraw, BEFORE taking BackendLock():
//     the worker holds BackendLock in S2-D while it works on the very events the gate is waiting for, so waiting
//     inside the lock would only end by the 50 ms timeout.
//   * call GateCpSwap(n) with n = 0-based ordinal of the swap the CP is executing (the same numbering as
//     EvHeader::frame of the kSwap event: the k-th Swap call of the game thread is frame k), also before the lock.
// Both are one cached-bool branch when WWE2K14_D3D_SHADOW is off.
#pragma once

#include <cstdint>

namespace wwe2k14::d3d {

// CP thread: wait until worker_done_seq >= seq(packet_addr) (timeout WWE2K14_D3D_SHADOW_GATE_TIMEOUT_MS, default 50 ->
// counted gate_timeout and released). Unpaired packets (MB 0x10F persistent region, replays older than the map) do not
// wait. `packet_addr` is the SDK's ObservedDraw::packet_guest_address (physical; bits above 0x1FFFFFFF are ignored).
void GateCpDraw(uint32_t packet_addr);
// Card DE-2M: CP thread, at an indexed-draw marker (hle_marker.h) carrying event seq `seq`: the same arrival + wait.
void GateCpSeq(uint32_t seq);
// Card S3-44 diagnostic: called on the CP thread after GateCpDraw's wait, with the draw's packet address.
void SetCpDrawProbe(void (*fn)(uint32_t));
// Card S3-44 (WWE2K14_D3D_LATE_TEX=1): worker thread - wait (bounded) until the CP thread has reached the draw with event
// seq `seq` (the game kicked the ring past it, so guest data it reads is final). Must not hold BackendLock.
bool GateWaitCpArrival(uint32_t seq, uint32_t timeout_ms);
// Card S3-44 (WWE2K14_D3D_LATE_TEX=2): ring kicks. OnGuestKick is the SDK CP_RB_WPTR hook (guest thread); the worker waits
// (bounded) until the guest kicked the ring after the draw with event seq `seq` was recorded.
void OnGuestKick(uint32_t value);
bool GateWaitKickAfter(uint32_t seq, uint32_t timeout_ms);
// Card S3-29b diagnostic: where the CP thread is in the nr Observer callbacks (printed by the capture watchdog).
void SetCpWhere(uint32_t where);
uint32_t CpWhere();
// CP thread: wait for the kSwap event of game frame `frame` (everything of that frame is then consumed).
void GateCpSwap(uint32_t frame);
// Stage 3 / S3-1 (WWE2K14_NR_PRESENT_D3D=1): same wait, with the outcome - 0 frame consumed by the worker, 1 timeout,
// 2 unpaired (no kSwap event recorded for this frame), 3 gate off.
int GateCpSwapStatus(uint32_t frame);

// Card S3-17: CP thread, before BackendLock: wait until the worker consumed every event published so far (all D3D
// calls the game made before this point, e.g. the resolve a CPU read follows). 0 done, 1 timeout, 3 gate off.
int GateWaitPublished(uint32_t timeout_ms, uint64_t* waited_ns);

// ---- WP9 internal (capture/worker side), additions that do not change the frozen calls above ----
struct GateStats {
  uint64_t registered = 0, passed = 0, waited = 0, timeouts = 0, unpaired = 0, wait_ns = 0;
  uint64_t swap_waited = 0, swap_timeouts = 0, swap_unpaired = 0, swap_wait_ns = 0, max_wait_ns = 0;
  uint64_t markers = 0;  // card DE-2M
};
void GateConfigure(bool shadow_on);                         // capture Init only (never calls ShadowEnabled)
bool GateEnabled();                                         // WWE2K14_D3D_SHADOW on and WWE2K14_D3D_SHADOW_GATE != 0
void GateRegister(uint32_t packet_addr, uint32_t seq);      // game thread, at record time
void NoteDrawKick(uint32_t seq);                            // card S3-44 / DE-2M: kick count at a published draw
void GateRegisterSwap(uint32_t frame, uint32_t seq);        // game thread, when the kSwap event is published
void GateAdvance(uint32_t done_seq);                        // worker thread: every event up to done_seq is consumed
uint32_t GateDoneSeq();
int64_t GateSwapPublishedNs(uint32_t frame);  // card S3-44 diagnostic: steady-clock ns of that frame's Swap publish
GateStats GateSnapshot();                                   // any thread (relaxed reads)

}  // namespace wwe2k14::d3d
