// WP12 (decision 0006 stage 2): D3D event side-file, see sidefile.h for the format and env.
#include "sidefile.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include <rex/logging.h>

#include "capture.h"
#include "hooks.h"

namespace wwe2k14::d3d::sidefile {

namespace {

constexpr char kMagic[8] = {'D', '3', 'D', 'S', 'I', 'D', 'E', '1'};
constexpr uint32_t kTag = 0x56454433u;
constexpr uint32_t kFileHeaderBytes = 32;
constexpr size_t kBufferBytes = 4u << 20;
constexpr uint32_t kKinds = 13;  // EvKind::kFullSnapshot + 1

const char* const kKindNames[kKinds] = {"draw", "drawup", "clear", "resolve", "setrt", "setdepth", "begintiling",
                                        "endtiling", "shaderload", "aluload", "swap", "devicereset", "fullsnapshot"};

const char* EnvStr(const char* n) {
  const char* v = std::getenv(n);
  return v && *v ? v : nullptr;
}

bool Active() { return ObserveEnabled() && ShadowEnabled(); }

void Put32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }
void Put16(uint8_t* p, uint16_t v) { std::memcpy(p, &v, 2); }
uint32_t Get32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return v;
}
uint16_t Get16(const uint8_t* p) {
  uint16_t v;
  std::memcpy(&v, p, 2);
  return v;
}

void Serialize(const EvHeader& e, uint8_t* o) {
  Put32(o + 0, e.seq);
  Put32(o + 4, e.frame);
  o[8] = uint8_t(e.kind);
  o[9] = e.emitter;
  Put16(o + 10, e.n_blocks);
  Put32(o + 12, e.payload_bytes);
  Put32(o + 16, e.packet_addr);
  for (int i = 0; i < 10; ++i) Put32(o + 20 + 4 * i, e.args[i]);
}
void Deserialize(const uint8_t* o, EvHeader* e) {
  e->seq = Get32(o + 0);
  e->frame = Get32(o + 4);
  e->kind = EvKind(o[8]);
  e->emitter = o[9];
  e->n_blocks = Get16(o + 10);
  e->payload_bytes = Get32(o + 12);
  e->packet_addr = Get32(o + 16);
  for (int i = 0; i < 10; ++i) e->args[i] = Get32(o + 20 + 4 * i);
}

// ------------------------------------------------------------------------------------------------ recording
struct Recorder {
  std::mutex mu;
  FILE* f = nullptr;
  std::vector<uint8_t> buf;
  uint64_t bytes = 0, events = 0, max_bytes = 0, dropped = 0, raw_bytes = 0;
  uint32_t from = 0, frames = 0xFFFFFFFFu;  // RR1 window [from, from + frames)
  bool mem = true;                          // RR1 guest-memory records
  bool stopped = false;
  std::string path;
  // RR2: WWE2K14_D3D_SIDEFILE_START_FILE: the window opens at the frame after the game thread first sees this file
  // (the test harness creates it once the scene runs); `from` is then that frame (kNotYet until it fires).
  static constexpr uint32_t kNotYet = 0xFFFFFFFFu;
  std::string start_file;
  std::atomic<uint32_t> trig_from{kNotYet};
  uint32_t trig_checked = kNotYet;  // game thread only: last frame the file was looked for
  bool window_done = false;          // the window's last swap was recorded: later raw records go to disk at once

  void FlushLocked() {
    if (f && !buf.empty()) {
      std::fwrite(buf.data(), 1, buf.size(), f);
      std::fflush(f);
    }
    buf.clear();
  }
};

Recorder* Rec() {
  static Recorder* r = []() -> Recorder* {
    if (!Active()) return nullptr;
    const char* p = EnvStr("WWE2K14_D3D_SIDEFILE_OUT");
    if (!p) p = EnvStr("WWE2K14_D3D_SIDEFILE");
    if (!p) return nullptr;
    auto* rec = new Recorder();  // intentionally leaked: the atexit flush runs after static destruction otherwise
    rec->path = p;
    rec->f = std::fopen(p, "wb");
    if (!rec->f) {
      REXLOG_INFO("[d3d-sidefile] cannot open {} for writing; recording off", p);
      delete rec;
      return nullptr;
    }
    const char* mb = EnvStr("WWE2K14_D3D_SIDEFILE_MAX_MB");
    rec->max_bytes = uint64_t(mb ? std::max(1, std::atoi(mb)) : 2048) << 20;
    if (const char* v = EnvStr("WWE2K14_D3D_SIDEFILE_FROM")) rec->from = uint32_t(std::strtoul(v, nullptr, 0));
    if (const char* v = EnvStr("WWE2K14_D3D_SIDEFILE_FRAMES")) rec->frames = uint32_t(std::strtoul(v, nullptr, 0));
    if (const char* v = EnvStr("WWE2K14_D3D_SIDEFILE_MEM")) rec->mem = std::strcmp(v, "0") != 0;
    if (const char* v = EnvStr("WWE2K14_D3D_SIDEFILE_START_FILE")) rec->start_file = v;
    rec->buf.reserve(kBufferBytes + (1u << 16));
    uint8_t h[kFileHeaderBytes] = {};
    std::memcpy(h, kMagic, 8);
    Put32(h + 8, kVersion);
    Put32(h + 12, kFileHeaderBytes);
    Put32(h + 16, kSerializedHeaderBytes);
    std::fwrite(h, 1, sizeof(h), rec->f);
    std::atexit([] { Flush(); });
    if (!rec->start_file.empty())
      REXLOG_INFO("[d3d-sidefile] recording to {} (max {} MB, {} frames from the frame after {} appears, memory {})", p,
                  rec->max_bytes >> 20, rec->frames, rec->start_file, rec->mem ? "on" : "off");
    else
      REXLOG_INFO("[d3d-sidefile] recording to {} (max {} MB, frames {}..{}, memory {})", p, rec->max_bytes >> 20, rec->from,
                  rec->frames == 0xFFFFFFFFu ? std::string("end") : std::to_string(uint64_t(rec->from) + rec->frames - 1),
                  rec->mem ? "on" : "off");
    return rec;
  }();
  return r;
}

// ------------------------------------------------------------------------------------------------ replay
struct Replay {
  Reader reader;
  bool loaded = false;
  // current frame
  bool have_frame = false;
  uint32_t frame = 0;
  size_t first = 0, last = 0;
  std::unordered_map<uint32_t, std::vector<size_t>> by_addr;  // packet_addr -> event indices of the current frame
  std::unordered_map<uint32_t, size_t> cursor;
  // alignment
  bool configured = false;
  uint32_t configured_frame = 0;
  bool aligned = false;
  uint32_t aligned_frame = 0;
  // stats
  uint64_t lookups = 0, hits = 0, misses = 0;
};

Replay* Rep() {
  static Replay* r = []() -> Replay* {
    if (!Active()) return nullptr;
    const char* p = EnvStr("WWE2K14_D3D_SIDEFILE_IN");
    if (!p) return nullptr;
    auto* rep = new Replay();
    std::string why;
    if (!rep->reader.Load(p, &why)) {
      REXLOG_INFO("[d3d-sidefile] cannot load {}: {}; replay off", p, why);
      delete rep;
      return nullptr;
    }
    rep->loaded = true;
    if (const char* fr = EnvStr("WWE2K14_D3D_SIDEFILE_FRAME")) {
      rep->configured = true;
      rep->configured_frame = uint32_t(std::strtoul(fr, nullptr, 0));
    }
    REXLOG_INFO("[d3d-sidefile] replay {}: {}", p, rep->reader.Summary());
    return rep;
  }();
  return r;
}

}  // namespace

bool RecordEnabled() { return Rec() != nullptr; }
bool ReplayEnabled() { return Rep() != nullptr; }

void Flush() {
  Recorder* r = Rec();
  if (!r) return;
  std::lock_guard<std::mutex> lk(r->mu);
  r->FlushLocked();
}

namespace {
// `producer`: the game thread (Record) - the only caller that may fire the start-file trigger, so the window always
// opens at a frame whose events have not been produced yet (other threads see frames later).
bool Window(Recorder* r, uint32_t frame, bool producer) {
  if (!r) return false;
  uint32_t from = r->from;
  if (!r->start_file.empty()) {
    from = r->trig_from.load(std::memory_order_acquire);
    if (from == Recorder::kNotYet && producer && frame != r->trig_checked) {
      r->trig_checked = frame;
      std::error_code ec;
      if (std::filesystem::exists(r->start_file, ec)) {
        from = frame + 1;
        r->trig_from.store(from, std::memory_order_release);
        REXLOG_INFO("[d3d-sidefile] {} seen at frame {}: recording frames {}..{}", r->start_file, frame, from,
                    uint64_t(from) + r->frames - 1);
      }
    }
    if (from == Recorder::kNotYet) return false;
  }
  return frame >= from && uint64_t(frame) - from < r->frames;
}
}  // namespace

bool InWindow(uint32_t frame) { return Window(Rec(), frame, false); }

bool MemoryRecordEnabled() {
  Recorder* r = Rec();
  return r && r->mem;
}

void RecordRaw(const uint8_t* records, size_t n) {
  Recorder* r = Rec();
  if (!r || !n) return;
  std::lock_guard<std::mutex> lk(r->mu);
  if (r->stopped) return;
  if (r->bytes + n > r->max_bytes) {
    r->stopped = true;
    r->FlushLocked();
    REXLOG_INFO("[d3d-sidefile] size limit reached after {} events ({} MB, {} MB memory); recording stopped", r->events,
                r->bytes >> 20, r->raw_bytes >> 20);
    return;
  }
  r->buf.insert(r->buf.end(), records, records + n);
  r->bytes += n;
  r->raw_bytes += n;
  if (r->buf.size() >= kBufferBytes || r->window_done) r->FlushLocked();
}

void Record(const EvHeader& e, const uint8_t* payload) {
  Recorder* r = Rec();
  if (!r) return;
  if (!Window(r, e.frame, true)) return;
  const uint32_t pb = payload ? e.payload_bytes : 0;
  const uint32_t body = kSerializedHeaderBytes + pb;
  std::lock_guard<std::mutex> lk(r->mu);
  if (r->stopped) {
    ++r->dropped;
    return;
  }
  if (r->bytes + 8 + body > r->max_bytes) {
    r->stopped = true;
    r->FlushLocked();
    REXLOG_INFO("[d3d-sidefile] size limit reached after {} events ({} MB); recording stopped", r->events,
                r->bytes >> 20);
    return;
  }
  const size_t at = r->buf.size();
  r->buf.resize(at + 8 + body);
  uint8_t* o = r->buf.data() + at;
  Put32(o, kTag);
  Put32(o + 4, body);
  EvHeader copy = e;
  copy.payload_bytes = pb;  // a null payload records as empty
  Serialize(copy, o + 8);
  if (pb) std::memcpy(o + 8 + kSerializedHeaderBytes, payload, pb);
  r->bytes += 8 + body;
  ++r->events;
  // RR4: the swap that ends the window flushes too (run 1 lost its last frame: the process ended without the atexit flush).
  const bool window_end = e.kind == EvKind::kSwap && !Window(r, e.frame + 1, false);
  if (window_end) r->window_done = true;
  if (r->buf.size() >= kBufferBytes || (e.kind == EvKind::kSwap && e.frame % 60 == 0) || window_end) r->FlushLocked();
}

// ------------------------------------------------------------------------------------------------ Reader
bool Reader::Load(const std::string& path, std::string* why) {
  events_.clear();
  blob_.clear();
  frames_.clear();
  mem_.clear();
  inv_.clear();
  shd_.clear();
  rim_.clear();
  phys_snapshot_ = false;
  version_ = 0;
  ranges_.clear();
  truncated_ = false;
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) {
    if (why) *why = "cannot open";
    return false;
  }
  std::fseek(f, 0, SEEK_END);
  const long sz = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (sz < long(kFileHeaderBytes)) {
    std::fclose(f);
    if (why) *why = "shorter than the 32-byte header";
    return false;
  }
  blob_.resize(size_t(sz));
  const size_t got = std::fread(blob_.data(), 1, blob_.size(), f);
  std::fclose(f);
  if (got != blob_.size()) {
    if (why) *why = "read error";
    return false;
  }
  if (std::memcmp(blob_.data(), kMagic, 8) != 0) {
    if (why) *why = "bad magic";
    return false;
  }
  version_ = Get32(blob_.data() + 8);
  if ((version_ != 1 && version_ != 2) || Get32(blob_.data() + 16) != kSerializedHeaderBytes) {
    if (why) *why = "unsupported version / event header size";
    return false;
  }
  size_t off = Get32(blob_.data() + 12);
  while (off < blob_.size()) {
    if (blob_.size() - off < 8) {
      truncated_ = true;
      break;
    }
    const uint8_t* p = blob_.data() + off;
    const uint32_t tag = Get32(p), body = Get32(p + 4);
    if (version_ >= 2 && tag == rr::kPsnTag && body == 4 && blob_.size() - off - 8 < 4) {  // truncated marker
      truncated_ = true;
      break;
    }
    if (version_ >= 2 && tag == rr::kPsnTag && body == 4) {  // RR4 snapshot marker
      phys_snapshot_ = true;
      off += 8 + body;
      continue;
    }
    if (version_ >= 2 && (tag == rr::kMemTag || tag == rr::kInvTag || tag == rr::kShdTag || tag == rr::kRimTag)) {
      if (blob_.size() - off - 8 < body) {
        truncated_ = true;
        break;
      }
      bool ok;
      if (tag == rr::kMemTag) {
        rr::MemRecord m;
        ok = rr::DecodeMem(p + 8, body, &m);
        if (ok) mem_.push_back(m);
      } else if (tag == rr::kRimTag) {
        rr::RimRecord ri;
        ok = rr::DecodeRim(p + 8, body, &ri);
        if (ok) rim_.push_back(ri);
      } else if (tag == rr::kShdTag) {
        rr::ShdRecord sr;
        ok = rr::DecodeShd(p + 8, body, &sr);
        if (ok) shd_.push_back(sr);
      } else {
        rr::InvRecord v;
        ok = rr::DecodeInv(p + 8, body, &v);
        if (ok) inv_.push_back(v);
      }
      if (!ok) {
        if (why) *why = "corrupt memory record at offset " + std::to_string(off);
        return false;
      }
      off += 8 + body;
      continue;
    }
    if (tag != kTag || body < kSerializedHeaderBytes) {
      if (why) *why = "corrupt record tag/length at offset " + std::to_string(off);
      return false;
    }
    if (blob_.size() - off - 8 < body) {
      truncated_ = true;
      break;
    }
    Event ev;
    Deserialize(p + 8, &ev.h);
    if (ev.h.payload_bytes != body - kSerializedHeaderBytes) {
      if (why) *why = "payload length mismatch at offset " + std::to_string(off);
      return false;
    }
    ev.payload = p + 8 + kSerializedHeaderBytes;
    events_.push_back(ev);
    off += 8 + body;
  }
  // RR1 records are written by the worker (and its helpers) in translation order; keep that order within a seq.
  std::stable_sort(mem_.begin(), mem_.end(), [](const rr::MemRecord& a, const rr::MemRecord& b) { return a.seq < b.seq; });
  std::stable_sort(inv_.begin(), inv_.end(), [](const rr::InvRecord& a, const rr::InvRecord& b) { return a.seq < b.seq; });
  // frame index (frames are contiguous in recording order; a frame number going backwards starts a new range
  // and is rejected: the recorder's frame is the game Swap count, monotonic)
  for (size_t i = 0; i < events_.size(); ++i) {
    const uint32_t fr = events_[i].h.frame;
    if (frames_.empty() || frames_.back() != fr) {
      if (!frames_.empty() && fr < frames_.back()) {
        if (why) *why = "frame number decreases at event " + std::to_string(i);
        return false;
      }
      if (!ranges_.empty()) ranges_.back().second = i;
      frames_.push_back(fr);
      ranges_.push_back({i, events_.size()});
    }
  }
  return true;
}

bool Reader::FrameRange(uint32_t frame, size_t* first, size_t* last) const {
  auto it = std::lower_bound(frames_.begin(), frames_.end(), frame);
  if (it == frames_.end() || *it != frame) return false;
  const auto& r = ranges_[size_t(it - frames_.begin())];
  if (first) *first = r.first;
  if (last) *last = r.second;
  return true;
}

size_t Reader::ForEachInFrame(uint32_t frame, const std::function<bool(const Event&)>& cb) const {
  size_t a, b, n = 0;
  if (!FrameRange(frame, &a, &b)) return 0;
  for (size_t i = a; i < b; ++i) {
    ++n;
    if (!cb(events_[i])) break;
  }
  return n;
}

bool Reader::BestFrameFor(const uint32_t* addrs, size_t n, uint32_t* frame, size_t* overlap) const {
  std::unordered_set<uint32_t> want(addrs, addrs + n);
  size_t best = 0;
  uint32_t best_frame = 0;
  for (size_t k = 0; k < frames_.size(); ++k) {
    std::unordered_set<uint32_t> seen;
    for (size_t i = ranges_[k].first; i < ranges_[k].second; ++i) {
      const uint32_t a = events_[i].h.packet_addr;
      if (a != 0xFFFFFFFFu && want.count(a)) seen.insert(a);
    }
    if (seen.size() > best) {
      best = seen.size();
      best_frame = frames_[k];
    }
  }
  if (!best) return false;
  if (frame) *frame = best_frame;
  if (overlap) *overlap = best;
  return true;
}

std::string Reader::Summary() const {
  uint64_t per[kKinds] = {}, payload = 0;
  for (const Event& e : events_) {
    if (uint32_t(e.h.kind) < kKinds) ++per[uint32_t(e.h.kind)];
    payload += e.h.payload_bytes;
  }
  std::string s = "events=" + std::to_string(events_.size()) + " frames=" + std::to_string(frames_.size());
  if (!frames_.empty()) s += " [" + std::to_string(frames_.front()) + ".." + std::to_string(frames_.back()) + "]";
  s += " payload_mb=" + std::to_string(payload >> 20) + (truncated_ ? " truncated_tail=1" : "");
  s += " version=" + std::to_string(version_);
  if (version_ >= 2)
    s += " mem_pages=" + std::to_string(mem_.size()) + " mem_mb=" + std::to_string((mem_.size() * rr::kPageBytes) >> 20) +
         " invalidations=" + std::to_string(inv_.size()) + " shaders=" + std::to_string(shd_.size()) +
         " phys_snapshot=" + (phys_snapshot_ ? "1" : "0") + " resolve_images=" + std::to_string(rim_.size());
  for (uint32_t k = 0; k < kKinds; ++k)
    if (per[k]) s += std::string(" ") + kKindNames[k] + "=" + std::to_string(per[k]);
  return s;
}

// ------------------------------------------------------------------------------------------------ replay driver
const Reader* ReplayReader() {
  Replay* r = Rep();
  return r ? &r->reader : nullptr;
}

bool ReplayBeginFrame(uint32_t recorded_frame) {
  Replay* r = Rep();
  if (!r) return false;
  if (r->have_frame) {
    REXLOG_INFO("[d3d-sidefile] frame {} replayed: lookups={} hit={} miss={}", r->frame, r->lookups, r->hits, r->misses);
  }
  r->have_frame = false;
  r->by_addr.clear();
  r->cursor.clear();
  r->lookups = r->hits = r->misses = 0;
  if (!r->reader.FrameRange(recorded_frame, &r->first, &r->last)) {
    REXLOG_INFO("[d3d-sidefile] recorded frame {} not in the file", recorded_frame);
    return false;
  }
  for (size_t i = r->first; i < r->last; ++i) {
    const uint32_t a = r->reader.at(i).h.packet_addr;
    if (a != 0xFFFFFFFFu) r->by_addr[a].push_back(i);
  }
  r->frame = recorded_frame;
  r->have_frame = true;
  return true;
}

void AutoAlign(const uint32_t* addrs, size_t n) {
  Replay* r = Rep();
  if (!r) return;
  uint32_t fr = 0;
  size_t ov = 0;
  if (r->reader.BestFrameFor(addrs, n, &fr, &ov)) {
    r->aligned = true;
    r->aligned_frame = fr;
    REXLOG_INFO("[d3d-sidefile] auto-align: recorded frame {} shares {} of {} draw packet addresses", fr, ov, n);
  } else {
    REXLOG_INFO("[d3d-sidefile] auto-align: no recorded frame shares a draw packet address ({} given)", n);
  }
}

bool ReplayBeginConfiguredFrame() {
  Replay* r = Rep();
  if (!r) return false;
  if (r->configured) return ReplayBeginFrame(r->configured_frame);
  if (r->aligned) return ReplayBeginFrame(r->aligned_frame);
  if (r->reader.frames().empty()) return false;
  return ReplayBeginFrame(r->reader.frames().front());
}

bool Lookup(uint32_t packet_addr, EvHeader* h, const uint8_t** payload) {
  Replay* r = Rep();
  if (!r) return false;
  if (!r->have_frame && !ReplayBeginConfiguredFrame()) return false;
  ++r->lookups;
  auto it = r->by_addr.find(packet_addr);
  if (it == r->by_addr.end()) {
    ++r->misses;
    return false;
  }
  size_t& c = r->cursor[packet_addr];
  if (c >= it->second.size()) {
    ++r->misses;
    return false;
  }
  const Event& ev = r->reader.at(it->second[c++]);
  if (h) *h = ev.h;
  if (payload) *payload = ev.payload;
  ++r->hits;
  return true;
}

size_t ReplayFeedFrame(const std::function<void(const EvHeader&, const uint8_t*)>& sink) {
  Replay* r = Rep();
  if (!r) return 0;
  if (!r->have_frame && !ReplayBeginConfiguredFrame()) return 0;
  for (size_t i = r->first; i < r->last; ++i) sink(r->reader.at(i).h, r->reader.at(i).payload);
  return r->last - r->first;
}

}  // namespace wwe2k14::d3d::sidefile
