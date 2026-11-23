// RR2 (docs/tasks/RR-record-replay.md): offline replay of a side-file recording (RR1) inside the game executable,
// before the SDK runtime starts: no kernel, no game code, no CP, no window. The recorded events go straight to the
// D3D worker (WorkerConsume, one thread, recording order); the recorded guest-memory pages are written into a
// standalone SDK Memory before the event that read them; recorded write-watch invalidations go to the texture watch.
// At each swap the presented front buffer is read back to a PNG.
//
// Env: WWE2K14_RR_PLAY=<side-file>   turns the mode on (the process exits when the replay ends)
//      WWE2K14_RR_OUT=<dir>          output folder (default rr-out): frame-gNNNNNN.png (game frame) + summary.txt
//      WWE2K14_RR_SWAPS=<n>          stop after n swaps (default: all)
// Use the shipped launcher env otherwise (same renderer switches as the recorded run). The mode forces only what has
// to differ offline: D3D_OBSERVE=1, D3D_SHADOW=2, NR_PRESENT_D3D=1, NR_DRAW_OFF=1, D3D_NATIVE_BOOT=1,
// NR_DIRECT_PRESENT=0 (no presenter), D3D_LATE_TEX / D3D_SIDEFILE_OUT / D3D_SIDEFILE unset.
// Exit code: 0 replayed, 2 file did not load, 3 guest memory init failed, 4 renderer not ready, 5 no frame presented,
// 6 worker read guest memory that no recorded page covered (the recording missed a read).
#pragma once

#include <cstdint>

namespace wwe2k14::d3d::rr {

// Returns only when WWE2K14_RR_PLAY is unset. Call before anything reads the renderer environment (OnPreSetup end).
void RunReplayIfRequested();

// Live run, CP thread holding the backend lock, at each presented swap: with WWE2K14_RR_LIVE_OUT=<dir> and a side-file
// recording window (sidefile.h), writes the picture of every swap inside the window as frame-gNNNNNN.png (the names
// the replay writes) for tools/rr/rr-diff.py. One GPU readback per swap inside the window only; off otherwise.
void MaybeLiveShot(uint32_t snap, uint32_t w, uint32_t h, const uint8_t swizzle[4], uint32_t game_frame);

}  // namespace wwe2k14::d3d::rr
