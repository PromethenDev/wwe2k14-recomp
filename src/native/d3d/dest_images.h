// Card S3-69 (SVR plan SV-6): resolve destinations as persistent host images - switches, counters and the bounded
// registry-history diagnostic. The registry itself stays in textures.cpp (keyed by guest address); this module only
// owns the switches, the counters printed in the periodic "[d3d-tex]" line and WWE2K14_D3D_DEST_TRACE.
//
// Switches (all default OFF; the old snapshot + derived-copy path stays compiled):
//   WWE2K14_D3D_DEST_IMAGES=1   master; the sub-switches below act only with it
//   WWE2K14_D3D_DEST_CANVAS=1   5a: partial resolves compose into one guest-seeded host image of the whole destination
//                               (pitch x RB_COPY_DEST_PITCH height), written in place; fetches inside it bind it
//   WWE2K14_D3D_DEST_CANVAS_REUSE=1 (default OFF): reuse a retained same-object canvas only while its complete seed
//                               source remains write-watch clean; bounded to 32 MiB / 64 destination objects
//   WWE2K14_D3D_DEST_MIPS=1     5b: mip chains of resolve levels as one persistent image, changed levels copied in place
//   WWE2K14_D3D_DEST_CUBE=1     5c: cubes of resolved faces as one persistent image, changed faces copied in place
//   WWE2K14_D3D_DEST_TRACE=1    diagnostic (independent of the master): registration history per destination and
//                               one line per refused fetch class (bounded); also turns on the object-identity counter
#pragma once
#include <cstdint>
#include <string>

namespace wwe2k14::d3d::dest {

bool On();         // WWE2K14_D3D_DEST_IMAGES=1
bool CanvasOn();   // 5a
bool CanvasReuseOn();  // opt-in: reuse a prior seeded canvas for the same destination object and unchanged guest bytes
bool MipsOn();     // 5b WWE2K14_D3D_DEST_MIPS=1: one persistent mip image per destination chain, levels copied in place
bool ObjBypassOn();  // 5e WWE2K14_D3D_DEST_OBJ_BYPASS=1: a fetch of another texture object over stale entries reads its own texture
bool CubeOn();     // 5c WWE2K14_D3D_DEST_CUBE=1: one persistent cube image per destination, faces copied in place
bool TraceOn();    // WWE2K14_D3D_DEST_TRACE=1
bool CountOn();    // On() || TraceOn(): object-identity / canvas counters are kept and printed

struct Counters {
  uint64_t canvas_new = 0;          // new canvas images created (any reason)
  uint64_t canvas_forced = 0;       // ... of which only because 5a composes a partial origin resolve
  uint64_t canvas_inplace = 0;      // resolves composed into an existing canvas image (no new image)
  uint64_t canvas_seeded = 0;       // new canvases successfully seeded from the bounded guest source (5a)
  uint64_t canvas_too_big = 0;      // 5a declined: whole-destination image would exceed the size cap
  uint64_t canvas_cache_reuse = 0;  // same-object seeded canvas reused after the full-source write watch stays clean
  uint64_t canvas_cache_miss = 0, canvas_cache_evict = 0;
  uint64_t seed_detile_ns = 0, seed_detile_calls = 0;  // CPU fallback counter, expected to remain zero for B2-perf2
  uint64_t seed_gpu_submit_ns = 0, seed_gpu_calls = 0;  // host submission time; GPU_ITEMS supplies device time
  uint64_t seed_upload_ns = 0, seed_upload_calls = 0, seed_upload_bytes = 0, seed_upload_padded_bytes = 0;
  uint64_t seed_source_transform_ns = 0, seed_source_transform_calls = 0;
  uint64_t seed_trace_cpu_ns = 0, seed_trace_cpu_calls = 0;  // bounded first-eight DEST_TRACE reference only
  uint64_t seed_cache_bytes = 0;
  uint64_t uncovered_binds = 0;     // 5a: fetches bound to a canvas region not fully written by resolves
  uint64_t obj_eq = 0, obj_ne = 0, obj_null = 0;  // fetch at an entry's start: texture_ptr[slot] vs resolve dest object
  uint64_t obj_fetch_null = 0, obj_dest_null = 0;  // ... obj_null split: fetch slot pointer 0 / resolve dest object 0
  uint64_t obj_part_eq = 0, obj_part_ne = 0;       // fetch overlapping an entry NOT at its start: same / other object
  uint64_t obj_bypass = 0;  // 5e: fetches that read their own texture past entries of other destination objects
  uint64_t mip_new = 0, mip_inplace = 0, mip_levels_copied = 0;     // 5b
  uint64_t cube_new = 0, cube_inplace = 0, cube_faces_copied = 0;  // 5c
  uint64_t dest_bytes = 0, dest_bytes_high = 0;   // registry image bytes (sampled at the periodic log)
  uint32_t dest_entries = 0, dest_canvases = 0;
};
Counters& C();
std::string Section();  // " | dest: ..." for the periodic [d3d-tex] line ("" when CountOn() is false)

// ---- WWE2K14_D3D_DEST_TRACE history (worker thread only) ----
struct RegRec {
  uint64_t frame = 0, seq = 0;
  int32_t rect[4] = {};
  uint32_t pitch = 0, dest_h = 0, info = 0, start = 0, length = 0, dest_texture = 0;
  int kind = -1;
  bool canvas = false, has_point = false;
  int32_t point[2] = {};
  uint64_t dest_fp = 0;   // sample fingerprint of the whole destination range at registration
  uint32_t dest_zero = 0; // zero dwords among the 256 sampled
};
void NoteRegistration(uint32_t dest_base, const RegRec& r);
void NoteDrop(uint32_t dest_base, uint64_t seq, uint64_t frame, const char* reason);
// History of `dest_base` as text (most recent last), "" if none.
std::string History(uint32_t dest_base);
// First-registration fingerprint of the destination (0 = unknown).
bool FirstFingerprint(uint32_t dest_base, uint64_t* fp, uint64_t* frame);
// One trace line per (base, fmt, w, h, class) at most `kMaxLines` in total; true = print this one.
bool TraceRefusalOnce(uint32_t base, uint32_t fmt, uint32_t w, uint32_t h, const std::string& cls);

}  // namespace wwe2k14::d3d::dest
