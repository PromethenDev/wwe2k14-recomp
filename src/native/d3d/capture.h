// WP9 (decision 0006 stage 2): capture of D3D state at hooked emitters (game thread) into an event stream.
// Frozen interface. Switch: WWE2K14_D3D_SHADOW = 0 (off, default) / 1 (translate-only) / 2 (shadow-draw);
// requires WWE2K14_D3D_OBSERVE=1.
//
// ===== Event record (what the worker / WP6 / WP7 / WP8 / WP10 receive) =====
// EvHeader (shadow_types.h) + payload (EvHeader::payload_bytes, 8-byte aligned start):
//   [DeltaBlock x n_blocks][data dwords of the blocks, in order]          <- what RegView::Apply() reads
//   [pad to 8][EvExt][kind data (EvExt::kind_bytes)][snapshot blocks + data (only when EvExt::flags & kExtSnapshot)]
// * Block data are HOST-order dwords (byte-swapped from the guest's big-endian memory at capture time).
// * offset_dw is the device-relative dword index (device byte offset / 4), device = guest address in EvExt::device.
// * Blocks 0..EvExt::n_mirror_blocks-1 are "mirror" blocks: device mirror memory that CHANGED since the previous record
//   (the full state in the first record / after kExtResync). The remaining blocks are "overlay" blocks: the loaded
//   data of the LOAD_ALU_CONSTANT packets the emitter put in the ring (the executed value of those registers; the
//   mirror differs from it in ~19 % of draws, OBS4) and, with WWE2K14_D3D_SHADOW_INLINE=1, inline constant writes.
//   An overlay persists (like the register file) until a later mirror block covers the same dwords;
//   WWE2K14_D3D_SHADOW_LOADMODE=temp ends it at the next record instead.
// * Applying every record's blocks in order to a zeroed device image gives the device state the game had at the Pre
//   hook of that record (capture_core.h Image does exactly this, the worker keeps one).
// * EvHeader::seq is 1-based and gap-free in worker order; EvHeader::frame = number of Swap calls before the event
//   (the kSwap event of swap k has frame k). EvHeader::packet_addr = physical address (& 0x1FFFFFFF) of the first draw
//   packet the emitter wrote, 0xFFFFFFFF if none (split draws: the other packet addresses are in DrawExt).
// * EvHeader::emitter = the hook ordinal (Em* below; bit 7 set = nested inside another hooked emitter's call).
//
// EvHeader::args per kind (always r4..r10 = args[0..6], args[7] = float(f1) bits, args[8..9] = the two words at
// caller sp+92 / +100 for the Resolve kinds, else 0):
//   kDraw      DrawIndexedVertices: r4 prim, r5 base vertex, r6 start index, r7 index count. The Emit_* emitters
//              (init-time persistent buffers, Clear's quad via Emit_7930, ...) are kDraw too: raw r4..r10, see `emitter`.
//   kDrawUP    BeginVertices (record published at EndVertices): r4 prim, r5 vertex count, r6 stride. Vertex bytes are in
//              the kind data (UpExt).
//   kClear     r4 count, r5 pRects, r6 flags, r7 D3DCOLOR, f1 Z bits (args[7]), r9 stencil (args[5]). The rects are copied
//              big-endian (ClearExt); the worker passes them to targets::OnClearData. Published at Pre; the quad draws Clear emits follow as nested kDraw records.
//   kResolve   ResolvePublic (emitter EmResolvePublic): r4 flags, r5 src rect*, r6 dest texture, r7 dest point*, r8 level,
//              r9 slice, r10 clear colour*, f1 clear Z, args[8..9] stack words; the pointed-to data is copied (ResolveExt).
//              ResolveWorker direct calls (emitter EmResolveWorker, only when not called from the public wrapper) carry the
//              worker's own register order (the wrapper inserts r6 = r7 = 0). RB_COPY_* (0x2318..0x2323) are final in the
//              state the worker holds when it sees the event (captured after the original ran).
//   kSetRT     SetRenderTarget (r4 index, r5 surface), ApplyRTDepth (r4 = pointer to 5 surfaces). Published at Post.
//   kSetDepth  SetDepthStencilSurface (r4 surface). Published at Post.
//   kBeginTiling (TilingExt: tile rects + colour copied) / kEndTiling   published at Pre; the ClearF quads / Resolve calls they make follow as separate records.
//   kSwap      Swap: args[0] = r4 = front buffer texture, args[1] = r5 = swap params (layout unknown). Published at Pre
//              (frame = index of this swap, i.e. the number of Swap calls before it). Every recorded frame (sidefile on)
//              is followed by a kExtResync record so a replay of one frame is self-contained.
//   kFullSnapshot  "state sync" record: blocks only (no action). Emitted at flush-capable hooks that are not records of
//              their own (FlushDispatcher, RunCommandBuffer, EndCommandBuffer) when the mirror changed, so dirty bits they
//              consume are not lost. A record with EvExt::flags & kExtSnapshot additionally carries a full copy of the live
//              mirror for the worker to compare (WWE2K14_D3D_SHADOW_FULLSNAP=N).
//   kShaderLoad, kAluLoad (NOT produced: no args[0]=first reg / n_blocks=0 variant exists; LOAD_ALU data are overlay
//              blocks of the draw record), kDeviceReset  never produced by WP9 (LOAD_ALU blocks ride on the draw record as overlay blocks).
//
// Guest memory access for WP7/WP8: worker code reads vertex/index/texture memory through ShadowGuestBase() (the same
// host base the hooks get; guest address A lives at ShadowGuestBase() + A). The CP gate guarantees that the game cannot
// have reused the memory of a draw before the worker finished that draw's event.
#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "shadow_types.h"

struct PPCContext;

namespace wwe2k14::d3d {

bool ShadowEnabled();  // WWE2K14_D3D_SHADOW != 0 and D3D observe on (read once)
int ShadowMode();
// WWE2K14_D3D_RING_WALK: 1 (unset/default, existing ring path), 0 (retired path), 2 (check existing path).
int ShadowRingWalkMode();
void CaptureNoteSetTexture(const PPCContext& ctx);  // F1 step 4: native SetTexture keeps the tag-scan slot re-read
bool CaptureOnProducerThread();  // F1: this thread is the bound capture producer (false before the first bind)
bool CaptureProducerBlocked();  // card S3-44: the game thread waits for a free capture slot      // 0 / 1 / 2
// Mode 1 = translate only: the worker makes NO call into nr::backend that reads or mutates state shared with the nr
// frontend / record thread (surfaces, command list, snapshots, textures). Handles it needs are local fakes.
inline bool TranslateOnly() { return ShadowMode() == 1; }
// Card S3-22: WWE2K14_D3D_NATIVE_DEPTH=1 - float32 host depth without float24 emulation (read once).
inline bool NativeDepth() {
  static const bool v = [] {
    const char* e = std::getenv("WWE2K14_D3D_NATIVE_DEPTH");
    return e && std::strcmp(e, "1") == 0;
  }();
  return v;
}

// Game thread, before / after the original guest function of a hooked address (hook-in lines added by WP1 owner).
void ShadowPre(uint32_t hook_addr, PPCContext& ctx, uint8_t* base);
void ShadowPost(uint32_t hook_addr, PPCContext& ctx, uint8_t* base);
// Card S3-58: game thread, at the shader literal loader sub_82CC1C58 before the original runs (shader_identity.cpp):
// its loads go into the open record's overlay (the one literal source). RingMakeSpace returned inside a record.
void ShadowConstLoad(PPCContext& ctx, uint8_t* base);
// Card DE-2M: DrawIndexedVertices hook, after the original body and before ShadowPost (WWE2K14_D3D_HLE_INDEXED=1).
void ShadowIndexedMarker(uint8_t* base);
void ShadowUpMarker(uint8_t* base);
void ShadowEmitMarker(uint8_t* base);  // card DE-2M stage 3: 82CB7930, after the original body  // card DE-2M stage 2: BeginVertices, after the original body
// Card DE-2M: the newest event seq the capture producer has allocated (torn-marker fallback on the CP thread).
uint32_t ShadowLatestSeq();
void ShadowNoteRingSwitch();
// Card S3-63: outermost RingMakeSpace / RingNewBuffer entry / exit (hooks.cpp): records the segment switch pairs of the
// open record.
void ShadowRingSwitchPre(const uint8_t* base, uint32_t dev);
void ShadowRingSwitchPost(const uint8_t* base, uint32_t dev, bool new_buffer);

// ---- WP9 additions (do not change the frozen calls above) ----
// Offline replay (WP12 sidefile::ReplayFeedFrame sink): pushes a recorded event (header + payload verbatim) into the same
// ring the live capture uses. Single producer; use it instead of (not together with) the live hooks. seq is renumbered.
void PublishRecorded(const EvHeader& h, const uint8_t* payload);
// Card S3-17: seq of the newest event handed to the worker queue (0 before the first).
uint32_t LastPublishedSeq();
// Guest-memory range check shared by every worker-side reader: [addr, addr+len) must lie inside one mapped guest region.
// Virtual 0x00001000..0x7FFFFFFF and 0x80000000..0x9FFFFFFF (heaps / image) are accepted as they are; the physical
// views 0xA0000000 / 0xC0000000 / 0xE0000000 are accepted only when the access stays inside their 512 MB window
// ((addr & 0x1FFFFFFF) + len <= 0x20000000). Never trust a guest pointer without this.
bool GuestRangeOk(uint32_t addr, uint64_t len);
// Thread role bookkeeping for the crash handler (1 producer is derived from the capture binding, 2 worker, 3 CP).
void NoteThreadRole(int role);
// The player closed the window: the process is being torn down (TerminateProcess). An access violation after this point
// (renderer threads racing the teardown, e.g. a queue notify on a waiter whose stack is gone) is reported in one line
// and the process exits with code 0 instead of a crash report (laptop 780M / UHD logs 2026-10-17, crash after
// "Execution complete").
void NoteWindowClosing();
const uint8_t* ShadowGuestBase();
// RR2 offline replay only (no hooks run): the replay image's host base of guest address 0.
void SetShadowGuestBase(const uint8_t* base);  // host pointer of guest address 0 (valid after the first hooked call), else null

// Emitter ordinals = order of D3D_HOOK_LIST in hooks.cpp.
enum Emitter : uint8_t {
  EmBeginVertices = 0, EmEndVertices = 1, EmDrawIndexedVertices = 2, EmEmit5ED0 = 3, EmEmit49A8 = 4, EmEmit0810 = 5,
  EmEmit0968 = 6, EmEmit7930 = 7, EmEmit50E0 = 8, EmEmitC2E0 = 9, EmResolveWorker = 10, EmResolvePublic = 11,
  EmClear = 12, EmRunCommandBuffer = 15, EmEndCommandBuffer = 18, EmBeginTiling = 19, EmEndTiling = 20,
  EmSetRenderTarget = 21, EmSetDepthStencil = 22, EmApplyRTDepth = 23, EmFlushDispatcher = 27, EmSwap = 36,
  EmNestedBit = 0x80,
};
const char* EmitterName(uint8_t emitter);

constexpr uint32_t kExtMagic = 0x39505745u;  // 'EWP9'
enum ExtFlags : uint16_t {
  kExtTruncated = 1,   // kind data cut at the size limit (UP vertex bytes)
  kExtSnapshot = 2,    // full live-mirror copy follows the kind data (FULLSNAP verification)
  kExtResync = 4,      // the mirror blocks hold the complete state (first record, device change, overflow recovery)
  kExtWrapped = 8,     // ring wrapped inside the emitter window: packet_addr / overlay blocks may be missing
  kExtVbCheck = 16,    // UpExt::hash is valid
  kExtVbHash = 32,     // card M2c9: a VbHashExt follows the DrawExt (the M2c9 VBHASH writer was removed in S3-20;
                       // kept so a side-file recording that carries it still locates the TEXHASH extension below)
  kExtTexHash = 64,    // card M2c9: a VbHashExt of texture windows follows (after the vertex one when both are set)
};
// Card M2c9 diagnostic (WWE2K14_D3D_SHADOW_TEXHASH=N: one draw in N; off by default): hashes of the first 64 KB at
// the base page of the bound texture fetch windows, taken on the game thread when the draw call returned; the worker
// re-hashes the same windows right after translating the draw and counts the ones the CPU rewrote in between.
struct VbHashExt {
  uint32_t n;
  uint32_t pad;
  struct W { uint32_t phys, len; uint64_t hash; } w[16];
};
inline uint64_t WindowHash(const uint8_t* b, uint32_t n) {  // FNV-1a over 8-byte words (+ tail bytes)
  uint64_t h = 1469598103934665603ull;
  uint32_t i = 0;
  for (; i + 8 <= n; i += 8) {
    uint64_t v;
    __builtin_memcpy(&v, b + i, 8);
    h = (h ^ v) * 1099511628211ull;
  }
  for (; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
  return h;
}
struct EvExt {
  uint32_t magic;
  uint16_t n_mirror_blocks;
  uint16_t flags;
  uint64_t dirty[5];      // device dirty masks at the Pre hook (dev+0..+32), host order (informational)
  uint32_t device;        // guest address of the device
  uint32_t kind_bytes;    // size of the kind data that follows
  uint32_t snap_blocks;   // snapshot DeltaBlock count / data dwords (flags & kExtSnapshot)
  uint32_t snap_dwords;
  uint32_t cursor_before, cursor_after;  // ring write cursor (dev+48, or dev+13844 for BeginVertices) before / after
  uint32_t key_valid;     // bit0 VS key, bit1 PS key: CurrentShaderKey() taken on the game thread right after the emitter returned
  uint64_t vs_fnv, ps_fnv;
  uint32_t vs_object, vs_code_addr, vs_bytes, ps_object, ps_code_addr, ps_bytes;
};
struct DrawExt { uint32_t n_packets; uint32_t packet_addr[8]; };  // all draw packets of a split draw (first == EvHeader)
struct ClearExt { uint32_t n_rects; /* then n_rects x 16 bytes D3DRECT exactly as in guest memory (big-endian), n_rects <= 64;
                                       n_rects == 0 or pRects null = whole surface */ };
struct TilingExt {  // BeginTiling: r4 flags, r5 tile count, r6 D3DRECT* tiles, r7 float4* colour, f1 Z, r9 stencil
  uint32_t n_tiles;      // copied tile rects (<= 64), 16 bytes each, big-endian, follow the colour
  uint32_t has_color;    // colour present
  uint8_t color_be[16];  // float4 as in guest memory
  /* then n_tiles x 16 bytes */
};
struct ResolveExt {
  uint32_t present;      // bit0 rect valid, bit1 point valid, bit2 clear colour valid
  int32_t rect[4];       // *args[1] (left, top, right, bottom as the game stores them), host order
  int32_t point[2];      // *args[3]
  float clear_rgba[4];   // *args[6]
};
struct UpExt {
  uint32_t guest_addr, total_bytes, copied_bytes, reserved;
  uint64_t hash;         // FNV-1a64 of the copied bytes at capture time (valid when kExtVbCheck)
  /* then copied_bytes of vertex data exactly as in guest memory (big-endian words) */
};

// Parsed view of a record payload.
struct EvView {
  const DeltaBlock* blocks = nullptr;
  const EvExt* ext = nullptr;
  const uint8_t* kind = nullptr;  // kind data
  const uint8_t* snap = nullptr;  // snapshot blocks + data
  bool ok = false;
};
inline EvView ParseEvent(const EvHeader& h, const uint8_t* payload) {
  EvView v;
  uint32_t total_dw = 0;
  const DeltaBlock* b = reinterpret_cast<const DeltaBlock*>(payload);
  for (uint32_t i = 0; i < h.n_blocks; ++i) total_dw += b[i].count_dw;
  uint32_t off = (h.n_blocks * uint32_t(sizeof(DeltaBlock)) + total_dw * 4u + 7u) & ~7u;
  if (off + sizeof(EvExt) > h.payload_bytes) return v;
  v.blocks = b;
  v.ext = reinterpret_cast<const EvExt*>(payload + off);
  if (v.ext->magic != kExtMagic) return v;
  v.kind = payload + off + sizeof(EvExt);
  v.snap = v.kind + ((v.ext->kind_bytes + 7u) & ~7u);
  v.ok = true;
  return v;
}

}  // namespace wwe2k14::d3d
