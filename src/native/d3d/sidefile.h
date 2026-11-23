// WP12 (decision 0006 stage 2): record/replay of the WP9 event stream beside .xtr traces, keyed by packet address.
// The frozen part (first block) is unchanged; everything below "WP12 additions" is new API (additions only).
// Depends on WP9's EvHeader record format; the payload (DeltaBlocks + data) is stored and returned VERBATIM, never
// interpreted here. Off unless WWE2K14_D3D_OBSERVE=1 and WWE2K14_D3D_SHADOW=1|2 (then the env below decides).
//
// Env (read once):
//   WWE2K14_D3D_SIDEFILE_OUT=<path> | WWE2K14_D3D_SIDEFILE=<path>  live recording (OUT wins; file is truncated)
//   WWE2K14_D3D_SIDEFILE_IN=<path>                                 replay (REX_REPLAY_TRACE offline run)
//   WWE2K14_D3D_SIDEFILE_FRAME=<n>   recorded game frame (EvHeader::frame) that the replayed .xtr corresponds to;
//                                    unset -> AutoAlign() result if the frontend supplied one, else the first frame
//   WWE2K14_D3D_SIDEFILE_MAX_MB=<n>  recording stops (logged once) after n MB (default 2048)
//   WWE2K14_D3D_SIDEFILE_FROM=<f>    RR1: record only events of game frames >= f (default 0)
//   WWE2K14_D3D_SIDEFILE_FRAMES=<n>  RR1: ... and < f + n (default: no end)
//   WWE2K14_D3D_SIDEFILE_MEM=0       RR1: no guest-memory / invalidation records (event stream only, as version 1)
//   WWE2K14_D3D_SIDEFILE_START_FILE=<path>  RR2: instead of _FROM, the window opens at the frame after the game
//                                    thread first sees <path> (test harness: created once the scene runs)
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "rr_mem_core.h"
#include "shadow_types.h"

namespace wwe2k14::d3d::sidefile {

// WWE2K14_D3D_SIDEFILE_OUT/IN; read once.
bool RecordEnabled();
bool ReplayEnabled();
// Live: append one event (header + payload) to the side file. Game thread (WP9 capture), cheap (memcpy into a buffer
// under a mutex; the buffer is written when full and every 60 frames). payload may be null when payload_bytes == 0.
void Record(const EvHeader& e, const uint8_t* payload);
// Replay, CP thread (nr hook-in point): the next not yet returned event of the CURRENT replay frame recorded for
// `packet_addr` (a packet address occurs once per frame per execution; repeats are returned in recording order);
// false if none. The payload pointer stays valid for the process lifetime.
bool Lookup(uint32_t packet_addr, EvHeader* h, const uint8_t** payload);

// ===================================================== WP12 additions =====================================================

// ---- file format (little endian; version 2; version 1 files still load) ----
//   header (32 B): "D3DSIDE1" | u32 version=2 | u32 header_bytes=32 | u32 ev_header_bytes=60 | u32 flags=0 |
//                  u32 reserved[2]
//   record: u32 tag=0x56454433 ("3DEV") | u32 body_bytes | body = EvHeader serialized field by field (seq, frame,
//           u8 kind, u8 emitter, u16 n_blocks, payload_bytes, packet_addr, args[10]; 60 B) + payload_bytes of payload.
//   Version 2 adds the RR1 records "MEM1" / "INV1" (rr_mem_core.h), interleaved in recording order; they carry the
//   seq of the event the worker was translating.
//   No footer: a truncated tail (process killed) is tolerated; the loader stops at the first incomplete record.
constexpr uint32_t kVersion = 2;
constexpr uint32_t kSerializedHeaderBytes = 60;

// Flush the live buffer to disk now (also done at exit and every 60 frames). Safe when not recording.
void Flush();
// RR1: true when game frame `frame` lies inside the recording window (always false when not recording).
bool InWindow(uint32_t frame);
// RR1: true while recording guest memory (recording on and WWE2K14_D3D_SIDEFILE_MEM != 0).
bool MemoryRecordEnabled();
// RR1: append complete pre-encoded records (tag + length + body, rr_mem_core.h Encode*) under the size limit.
void RecordRaw(const uint8_t* records, size_t bytes);

// One loaded event. `payload` points into the loaded blob.
struct Event {
  EvHeader h;
  const uint8_t* payload;
};

// Whole-file reader (offline tools, tests, and the replay path). Not thread safe for Load; const afterwards.
class Reader {
 public:
  // false + *why on a bad/missing file. A truncated tail is accepted (truncated() reports it).
  bool Load(const std::string& path, std::string* why);
  size_t size() const { return events_.size(); }
  const Event& at(size_t i) const { return events_[i]; }  // recording order = seq order
  bool truncated() const { return truncated_; }
  // Distinct recorded frames, ascending, and the [first, last) event index range of one frame (empty when unknown).
  const std::vector<uint32_t>& frames() const { return frames_; }
  bool FrameRange(uint32_t frame, size_t* first, size_t* last) const;
  // Iterate the events of one frame in order (return false from cb to stop). Returns events visited.
  size_t ForEachInFrame(uint32_t frame, const std::function<bool(const Event&)>& cb) const;
  // Recorded frame whose packet-address set overlaps `addrs` (the draw packets of the replayed .xtr frame, in any
  // order) the most; *overlap = matching count. false if no frame shares any address.
  bool BestFrameFor(const uint32_t* addrs, size_t n, uint32_t* frame, size_t* overlap) const;
  // Summary line: events, frames, per-kind counts, payload MB.
  std::string Summary() const;
  // RR1 records in recording order (sorted by seq; empty for version 1 files).
  const std::vector<rr::MemRecord>& mem() const { return mem_; }
  const std::vector<rr::InvRecord>& inv() const { return inv_; }
  const std::vector<rr::ShdRecord>& shd() const { return shd_; }  // RR4: shader table (any order)
  bool phys_snapshot() const { return phys_snapshot_; }          // RR4: recording opened with a physical snapshot
  const std::vector<rr::RimRecord>& rim() const { return rim_; }  // RR5: resolve registry at window open
  uint32_t version() const { return version_; }

 private:
  std::vector<uint8_t> blob_;
  std::vector<Event> events_;
  std::vector<uint32_t> frames_;
  std::vector<std::pair<size_t, size_t>> ranges_;  // parallel to frames_
  std::vector<rr::MemRecord> mem_;
  std::vector<rr::InvRecord> inv_;
  std::vector<rr::ShdRecord> shd_;
  bool phys_snapshot_ = false;
  std::vector<rr::RimRecord> rim_;
  uint32_t version_ = 0;
  bool truncated_ = false;
};

// ---- replay driver (CP thread; the nr hook-in point calls these, WP0 owner adds the lines) ----
// Selects the recorded frame the next Lookups serve and resets the per-address cursors. Call at the start of every
// replayed frame (a REX_REPLAY_TRACE_REPEAT repeat plays the same frame again: call it again with the same value).
// Returns false if replay is off or the frame is not in the file.
bool ReplayBeginFrame(uint32_t recorded_frame);
// ReplayBeginFrame(configured frame): WWE2K14_D3D_SIDEFILE_FRAME, else the AutoAlign()ed frame, else the first.
bool ReplayBeginConfiguredFrame();
// Frontend supplies the draw packet addresses of the .xtr frame being replayed (before the first draw) so the frame
// can be chosen by overlap (Reader::BestFrameFor). Used by ReplayBeginConfiguredFrame when _FRAME is unset.
void AutoAlign(const uint32_t* draw_packet_addrs, size_t n);
const Reader* ReplayReader();  // null unless ReplayEnabled() and the file loaded

// ---- feed to the shadow pipeline (WP9 interface TODO) ----
// Calls sink(event) for every event of the current replay frame in recording order (all kinds, including those with
// no packet address: set-RT, clear, resolve, shader/ALU loads, swap). The intended sink is WP9's worker input: it
// must publish an event into the same ring that EventReader::Next() drains, so WP3/WP7 checks run exactly as live.
// TODO(WP9 integration): wwe2k14::d3d::capture has no public "publish recorded event" entry yet (capture.h only has
// ShadowPre/ShadowPost); once WP9 exposes `void PublishRecorded(const EvHeader&, const uint8_t* payload)` the
// replay frontend calls ReplayFeedFrame(PublishRecorded). Until then nothing consumes replayed events.
size_t ReplayFeedFrame(const std::function<void(const EvHeader&, const uint8_t*)>& sink);

}  // namespace wwe2k14::d3d::sidefile
