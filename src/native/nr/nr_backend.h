// Imported into WWE 2K14 native workspace (Track E, 2026-10-13) from
//   <wwe13-recomp-nr>/src/nr/nr_backend.h
//   repo HEAD 19c8bb361a9b7d6cba65afffc628bda023da2e0c (file last changed in 98659c4),
//   original sha256 03ac8ffe744515e39ec76c0279db7e1b43c7ff1e7d2c041f33c131a0cb6b979d.
// Local changes: WWE13_NR* renamed to WWE2K14_NR* (comments/env names only).
// WWE '13 native renderer - GPU backend (plume / Vulkan).
//
// This half knows nothing about the Xbox 360 or the ReXGlue SDK: the frontend (nr_frontend.cpp)
// decodes the guest command stream into these plain descriptions. It is compiled together with
// plume into one partially linked, symbol-localised object (tools/nr/build-backend.sh) because
// plume carries its own volk/VMA that would collide with the SDK's.
//
// One queue, one command list per batch (M4 fix 1): calls only record commands; the CPU waits for the
// GPU only when a caller needs readback results or when a per-batch ring is full.
// Bindless texture tables match the converted shaders (tools/nr/xenos/shader_common_nr.h).

#pragma once

#include <cstdint>
#include <functional>
#include <cstring>
#include <memory>
#include <map>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>

namespace nr::backend {

constexpr uint32_t kNone = UINT32_MAX;

// S5-1 bounded, opt-in resolve census. Frame is attribution-only and not part of the destination key.
struct ResolveCensusKey {
  uint32_t dest_base = 0, dest_format = 0, width = 0, height = 0, resolve_class = 0;
  uint32_t source_handle = kNone, msaa = 0;
  uint64_t source_target = 0, frame = 0;
  bool valid = false;
};
struct ResolveCensusFetchKey {
  uint32_t base = 0, format = 0, width = 0, height = 0, mip_max = 0, tiled = 0;
  bool operator<(const ResolveCensusFetchKey& o) const {
    return std::tie(base, format, width, height, mip_max, tiled) <
           std::tie(o.base, o.format, o.width, o.height, o.mip_max, o.tiled);
  }
};
struct ResolveCensusSiteCounts { uint64_t calls = 0, bytes = 0; };
struct ResolveCensusFetchCounts {
  uint64_t binds = 0, rebuilds = 0, guest_detile_bytes = 0, guest_upload_bytes = 0, resolve_paste_bytes = 0;
};
struct ResolveCensusCounts {
  uint64_t resolves = 0, images = 0, copies = 0, blits = 0, compute = 0, msaa_resolves = 0;
  uint64_t bytes_copied = 0, guest_writebacks = 0, guest_writeback_bytes = 0, cpu_readbacks = 0;
  uint64_t cpu_readback_bytes = 0, consumers = 0, barriers = 0;
  std::map<uint32_t, ResolveCensusSiteCounts> copy_sites;  // nr_backend.cpp source line -> calls / bytes
  std::map<ResolveCensusFetchKey, ResolveCensusFetchCounts> fetches;
};
struct ResolveCensusRow { ResolveCensusKey key; ResolveCensusCounts counts; };
struct ResolveCensusFrame {
  uint64_t images = 0, copies = 0, blits = 0, compute = 0, msaa_resolves = 0, barriers = 0, consumers = 0;
};
bool ResolveCensusEnabled();
class ResolveCensusScope {
 public:
  explicit ResolveCensusScope(const ResolveCensusKey& key);
  ~ResolveCensusScope();
  ResolveCensusScope(const ResolveCensusScope&) = delete;
  ResolveCensusScope& operator=(const ResolveCensusScope&) = delete;
 private:
  ResolveCensusKey previous_{};
  bool previous_valid_ = false, active_ = false;
};
class ResolveCensusDrawScope {
 public:
  explicit ResolveCensusDrawScope(uint64_t frame);
  ~ResolveCensusDrawScope();
  ResolveCensusDrawScope(const ResolveCensusDrawScope&) = delete;
  ResolveCensusDrawScope& operator=(const ResolveCensusDrawScope&) = delete;
 private:
  bool active_ = false;
};
void ResolveCensusResolve(const ResolveCensusKey& key);
void ResolveCensusConsumer(uint32_t dest_base);
void ResolveCensusImage();
void ResolveCensusCopyAt(uint64_t bytes, uint32_t source_line);
#define ResolveCensusCopy(bytes) ResolveCensusCopyAt((bytes), __LINE__)
void ResolveCensusBlit(uint64_t bytes);
void ResolveCensusCompute();
void ResolveCensusMsaaResolve(uint64_t bytes);
void ResolveCensusBarrier();
void ResolveCensusReadback(uint64_t bytes);
void ResolveCensusGuestWriteback(uint64_t bytes);
void ResolveCensusFetchBind(const ResolveCensusKey& resolve_key, const ResolveCensusFetchKey& key, uint64_t frame, bool rebuilt,
                            uint64_t guest_detile_bytes, uint64_t guest_upload_bytes, uint64_t resolve_paste_bytes);
std::vector<ResolveCensusRow> TakeResolveCensusWindow();
ResolveCensusFrame TakeResolveCensusFrame(uint64_t frame);

enum class TexFormat : uint32_t { kRGBA8, kBC1, kBC2, kBC3, kR32F, kRGBA16, kRGBA16S, kR8, kRGBA8S, kRG16, kRG16S, kCount };
// Host colour target formats (guest RB_COLOR_INFO format -> host, frontend decides).
enum class TargetFormat : uint32_t { kRGBA8, kRGBA16F, kR32F, kRG16F, kRG32F, kRG16S, kRGBA16S, kD24S8, kD24FS8 };
uint32_t TargetBytesPerPixel(TargetFormat f);
enum class VtxFormat : uint32_t {
  kUnknown,
  kR32_UINT, kR32G32_UINT, kR32G32B32_UINT, kR32G32B32A32_UINT,
  kR32_SFLOAT, kR32G32_SFLOAT, kR32G32B32_SFLOAT, kR32G32B32A32_SFLOAT,
  kR8G8B8A8_UNORM, kR8G8B8A8_SNORM, kR8G8B8A8_UINT, kR8G8B8A8_SINT,
  kR16G16_SFLOAT, kR16G16B16A16_SFLOAT,
  kR16G16_UNORM, kR16G16_SNORM, kR16G16_UINT, kR16G16_SINT,
  kR16G16B16A16_UNORM, kR16G16B16A16_SNORM, kR16G16B16A16_UINT, kR16G16B16A16_SINT,
};
// Swizzle per output channel: 0-3 = source r,g,b,a; 4 = 0; 5 = 1 (Xenos fetch swizzle order).
struct TextureDesc {
  uint32_t width = 1, height = 1, levels = 1;
  TexFormat format = TexFormat::kRGBA8;
  uint8_t swizzle[4] = {0, 1, 2, 3};
  bool cube = false;  // 6 faces (+X -X +Y -Y +Z -Z), cube table
};
// Host-layout data of one mip level: rows of blocks (texels for uncompressed), tightly packed.
struct LevelData {
  std::vector<uint8_t> bytes;
  uint32_t width = 1, height = 1;  // texels
  uint32_t mip = 0, face = 0;
};
struct SamplerDesc {
  bool mag_linear = true, min_linear = true, mip_linear = true;
  uint32_t address_u = 0, address_v = 0, address_w = 0;  // 0 repeat 1 mirror 2 clamp edge 3 mirror-clamp edge 4 border
  uint32_t border = 0;  // 0 transparent black, 1 opaque black, 2 opaque white
  uint32_t max_anisotropy = 0;  // 0 = off
  float min_lod = 0.0f, max_lod = 1000.0f;
  float lod_bias = 0.0f;  // fetch constant + instruction bias (the SDK applies both)
};

struct VertexAttribute {
  uint32_t location;
  VtxFormat format;
  uint32_t binding;
  uint32_t offset_bytes;
};
// std::vector<uint8_t> whose resize() leaves new bytes uninitialised: the per-draw vertex / index copies overwrite every
// byte, and zero-filling tens of KB per draw first was a measurable part of the renderer thread's time.
template <class T>
struct DefaultInitAllocator : std::allocator<T> {
  template <class U>
  struct rebind {
    using other = DefaultInitAllocator<U>;
  };
  using std::allocator<T>::allocator;
  template <class U>
  void construct(U* p) noexcept(std::is_nothrow_default_constructible<U>::value) {
    ::new (static_cast<void*>(p)) U;
  }
  template <class U, class... A>
  void construct(U* p, A&&... a) {
    ::new (static_cast<void*>(p)) U(std::forward<A>(a)...);
  }
};
using ByteBuffer = std::vector<uint8_t, DefaultInitAllocator<uint8_t>>;
struct VertexBuffer {
  ByteBuffer data;  // host byte order
  uint32_t stride_bytes;
  // The source range before prefix_bytes is unused by this draw; the buffer's size is prefix + data.size().
  uint32_t prefix_bytes = 0;
  // cache_id != 0 means the D3D VBC backend owns this buffer (host byte order, whole buffer, prefix 0, cache_size bytes).
  // cache_upload: `data` holds the bytes, store them (replacing the id's old buffer); otherwise use the stored buffer.
  uint32_t cache_id = 0;
  uint32_t cache_size = 0;
  bool cache_upload = false;
  // Synchronous-only source range for WWE2K14_NR_VB_DIRECT. It points at the current guest bytes and is not
  // submitted to the record-thread/batch path; direct_bytes follows prefix_bytes.
  const uint8_t* direct_src = nullptr;
  uint32_t direct_bytes = 0, direct_endian = 0;
};
struct BlendTarget {
  bool enabled = false;
  uint32_t src = 2, dst = 1, op = 1, src_a = 2, dst_a = 1, op_a = 1;  // plume RenderBlend / RenderBlendOperation values
  uint8_t write_mask = 0xF;
};
struct StencilFace {
  uint32_t fail = 1, depth_fail = 1, pass = 1, compare = 8;  // plume RenderStencilOp / RenderComparisonFunction
};
struct TextureBinding {
  uint32_t slot;        // microcode fetch slot 0-31
  uint32_t dimension;   // 0 2D, 1 3D, 2 cube
  uint32_t texture;     // handle from CreateTexture
  SamplerDesc sampler;
};

struct FullFloatConstants {
  uint8_t vs[4096] = {};
  uint8_t ps[4096] = {};
};

struct Draw {
  // Surfaces (handles from Surface()) bound for this draw; kNone = not bound. Both bound ones must
  // have the same sample count.
  uint32_t color = kNone, depth = kNone;
  uint64_t vs_key = 0, ps_key = 0;
  uint32_t vb_direct_mode = 0;  // per-draw snapshot of WWE2K14_NR_VB_DIRECT (0 off, 1 fast, 2 CHECK)
  const std::vector<uint8_t>* vs_spirv = nullptr;
  const std::vector<uint8_t>* ps_spirv = nullptr;  // null: no pixel shader (depth only)
  uint32_t spec_constants = 0;
  bool sample_shading = false;  // per-sample shading (minSampleShading 1) on multisampled targets
  uint32_t topology = 4;  // plume RenderPrimitiveTopology (4 list, 5 strip)
  uint32_t cull = 1;      // plume RenderCullMode: 1 none, 2 front, 3 back
  bool front_ccw = false;
  bool depth_clamp = false;  // depth clamp instead of depth clipping (guest PA_CL_CLIP_CNTL clip_disable)
  std::vector<VertexAttribute> attributes;
  std::vector<VertexBuffer> buffers;  // index = binding
  ByteBuffer indices;                 // host order; empty = non-indexed
  bool index32 = false;
  uint32_t count = 0;                 // indices or vertices
  // Constant blocks exactly as the converted shaders address them.
  uint8_t vs_constants[4096] = {};
  uint8_t ps_constants[4096] = {};
  // Current shader usage map and retained copied-row masks for the lane-only V17e path. The latter survive Reset so
  // a transition can clear rows from the previous shader; unused rows in sparse mode stay zero.
  uint64_t vs_float_usage[4] = {}, ps_float_usage[4] = {};
  uint64_t vs_float_retained[4] = {}, ps_float_retained[4] = {};
  uint32_t const_used_mode = 0;  // lane-only WWE2K14_D3D_CONST_USED mode, copied with the draw
  std::shared_ptr<const FullFloatConstants> full_float_constants;  // CHECK-only raw full-block reference
  uint8_t shared[1024] = {};  // descriptor indices (backend fills them from `textures`), bools, ...
  std::vector<TextureBinding> textures;
  float viewport[6] = {0, 0, 1, 1, 0, 1};  // x y w h zmin zmax
  int32_t scissor[4] = {0, 0, 1, 1};       // x y w h
  bool depth_test = false, depth_write = false;
  uint32_t depth_compare = 8;  // plume RenderComparisonFunction
  bool stencil = false;
  uint32_t stencil_ref = 0, stencil_read = 0xFF, stencil_write = 0xFF;
  StencilFace stencil_front, stencil_back;
  BlendTarget blend;  // render target 0 only in M1
  float blend_constants[4] = {0, 0, 0, 0};
  uint32_t ordinal = 0;  // guest draw ordinal, for failure reports of asynchronously recorded draws
  // Record thread: texture / snapshot releases requested since the previous draw, run before this draw (a release is
  // never queued as its own entry: the frontend may hold an acquired, unsubmitted slot). (kind << 30) | id; kind 1 = texture, 2 = snapshot.
  std::vector<uint32_t> pre_ops;
  // Card S3-50 (WWE2K14_NR_RECORD_THREAD=3): backend calls queued since the previous draw (snapshots, texture builds,
  // clears, releases - in call order), run by the record thread before this draw. Their ids were reserved by the caller.
  std::vector<std::function<void()>> pre_jobs;
  // Batch-check sidecar: operation/argument hashes corresponding one-for-one with pre_jobs.
  std::vector<uint64_t> pre_job_hashes;
  // Diagnostic sidecar: stable operation labels for the D3D watchdog's record-consumer snapshot.
  std::vector<const char*> pre_job_names;
  bool jobs_only = false;  // card S3-50: a slot carrying only pre_jobs (no draw)
  // D3D VBC index buffer (index_cache_bytes bytes) is cached under index_cache_id (0 = not cached); index_cache_upload
  // stores `indices`, otherwise `indices` is empty. cache_flush drops every cached buffer first.
  uint32_t index_cache_id = 0, index_cache_bytes = 0;
  bool index_cache_upload = false;
  bool cache_flush = false;
  // Lane-only crowd fast path: immutable geometry/texture vectors may be shared by queued adjacent members.
  // The pointed-to Draw is an immutable cache template with shared_payload == nullptr.
  std::shared_ptr<const Draw> shared_payload;

  // Batch handoff: transfer a producer-local, fully translated draw into a preallocated ring slot. Dynamic payloads are
  // swapped so the producer keeps the slot's vector capacities; only the fixed draw state is copied.
  void TakeFrom(Draw& src) {
    color = src.color;
    depth = src.depth;
    vs_key = src.vs_key;
    ps_key = src.ps_key;
    vb_direct_mode = src.vb_direct_mode;
    vs_spirv = src.vs_spirv;
    ps_spirv = src.ps_spirv;
    spec_constants = src.spec_constants;
    sample_shading = src.sample_shading;
    topology = src.topology;
    cull = src.cull;
    front_ccw = src.front_ccw;
    depth_clamp = src.depth_clamp;
    attributes.swap(src.attributes);
    buffers.swap(src.buffers);
    indices.swap(src.indices);
    index32 = src.index32;
    count = src.count;
    std::memcpy(vs_constants, src.vs_constants, sizeof(vs_constants));
    std::memcpy(ps_constants, src.ps_constants, sizeof(ps_constants));
    std::memcpy(vs_float_usage, src.vs_float_usage, sizeof(vs_float_usage));
    std::memcpy(ps_float_usage, src.ps_float_usage, sizeof(ps_float_usage));
    std::memcpy(vs_float_retained, src.vs_float_retained, sizeof(vs_float_retained));
    std::memcpy(ps_float_retained, src.ps_float_retained, sizeof(ps_float_retained));
    const_used_mode = src.const_used_mode;
    full_float_constants = src.full_float_constants;
    std::memcpy(shared, src.shared, sizeof(shared));
    textures.swap(src.textures);
    std::memcpy(viewport, src.viewport, sizeof(viewport));
    std::memcpy(scissor, src.scissor, sizeof(scissor));
    depth_test = src.depth_test;
    depth_write = src.depth_write;
    depth_compare = src.depth_compare;
    stencil = src.stencil;
    stencil_ref = src.stencil_ref;
    stencil_read = src.stencil_read;
    stencil_write = src.stencil_write;
    stencil_front = src.stencil_front;
    stencil_back = src.stencil_back;
    blend = src.blend;
    std::memcpy(blend_constants, src.blend_constants, sizeof(blend_constants));
    ordinal = src.ordinal;
    pre_ops.swap(src.pre_ops);
    pre_jobs.swap(src.pre_jobs);
    pre_job_hashes.swap(src.pre_job_hashes);
    pre_job_names.swap(src.pre_job_names);
    jobs_only = src.jobs_only;
    index_cache_id = src.index_cache_id;
    index_cache_bytes = src.index_cache_bytes;
    index_cache_upload = src.index_cache_upload;
    cache_flush = src.cache_flush;
    shared_payload.swap(src.shared_payload);
  }

  // Back to the state of a freshly constructed Draw, except constant blocks and usage masks (the frontend updates the
  // current used rows and clears rows retained from the previous draw) and vector capacities. Keep in step with members.
  void Reset() {
    shared_payload.reset();
    full_float_constants.reset();
    color = depth = kNone;
    pre_jobs.clear();
    pre_job_hashes.clear();
    pre_job_names.clear();
    jobs_only = false;
    vs_key = ps_key = 0;
    const_used_mode = 0;
    vb_direct_mode = 0;
    vs_spirv = ps_spirv = nullptr;
    spec_constants = 0;
    sample_shading = false;
    topology = 4;
    cull = 1;
    front_ccw = false;
    depth_clamp = false;
    attributes.clear();
    buffers.clear();
    indices.clear();
    index_cache_id = index_cache_bytes = 0;
    index_cache_upload = cache_flush = false;
    index32 = false;
    count = 0;
    std::memset(shared, 0, sizeof(shared));
    textures.clear();
    const float vp[6] = {0, 0, 1, 1, 0, 1};
    std::memcpy(viewport, vp, sizeof(viewport));
    scissor[0] = scissor[1] = 0;
    scissor[2] = scissor[3] = 1;
    depth_test = depth_write = false;
    depth_compare = 8;
    stencil = false;
    stencil_ref = 0;
    stencil_read = stencil_write = 0xFF;
    stencil_front = stencil_back = StencilFace();
    blend = BlendTarget();
    std::memset(blend_constants, 0, sizeof(blend_constants));
    ordinal = 0;
    pre_ops.clear();
  }

  const std::vector<VertexAttribute>& Attributes() const {
    return shared_payload ? shared_payload->attributes : attributes;
  }
  const std::vector<VertexBuffer>& Buffers() const { return shared_payload ? shared_payload->buffers : buffers; }
  const ByteBuffer& Indices() const { return shared_payload ? shared_payload->indices : indices; }
  const std::vector<TextureBinding>& Textures() const { return shared_payload ? shared_payload->textures : textures; }
};

// shader_dir: converted-shader directory used by the resolve / texture-detile setup.
// Direct GPU presentation (WWE2K14_NR_DIRECT_PRESENT=1, Vulkan only): before Init, draw on the emulator's own Vulkan
// device and queue (plume adopts them) so a snapshot image can be presented without a CPU readback. Opaque
// handles (VkInstance, PFN_vkGetInstanceProcAddr, VkPhysicalDevice, VkDevice, VkQueue, std::recursive_mutex*).
struct SharedVulkanDevice {
  void* instance = nullptr;
  void* get_instance_proc_addr = nullptr;
  uint32_t api_version = 0;
  void* physical_device = nullptr;
  void* device = nullptr;
  uint32_t queue_family = 0;
  void* queue = nullptr;
  void* queue_mutex = nullptr;
  bool descriptor_indexing = false, scalar_block_layout = false, buffer_device_address = false;
  bool null_descriptor = false, sampler_mirror_clamp_to_edge = false, geometry_shader = false;
};
void SetSharedVulkanDevice(const SharedVulkanDevice& device);
// Card C3 (D3D12 shared present; WWE2K14_NR_BACKEND=d3d12 + WWE2K14_NR_DIRECT_PRESENT=1, SDK in D3D12 mode with
// nr_d3d12_shared_device): before Init, create the D3D12 backend on the SDK's ID3D12Device and draw on its direct
// queue (plume D3D12ExistingDevice), so a snapshot can be handed to the SDK's D3D12 presenter without a copy.
// Opaque handles (ID3D12Device*, ID3D12CommandQueue*), owned by the SDK (plume AddRefs them).
struct SharedD3D12Device {
  void* device = nullptr;
  void* direct_queue = nullptr;
};
void SetSharedD3D12Device(const SharedD3D12Device& device);
// Card C3: what the last successful PresentViewOfSnapshot on the shared D3D12 device returned besides the resource
// (its return value is then the ID3D12Resource* as an integer, in a state including NON_PIXEL_SHADER_RESOURCE):
// the SRV component mapping of the requested swizzle, and the plume fence value the submit that finished the image
// signals (on the same direct queue as the presenter). False when the last present was not a D3D12 one.
struct D3D12PresentInfo {
  uint32_t srv_format = 0;         // DXGI_FORMAT
  uint32_t component_mapping = 0;  // D3D12 Shader4ComponentMapping
  void* fence = nullptr;           // ID3D12Fence*
  uint64_t fence_value = 0;
};
bool TakeD3D12PresentInfo(D3D12PresentInfo* out);
// Persistent Vulkan pipeline cache file (before Init; "" = none). MaybeSavePipelineCache: once per swap, writes the
// cache a few seconds after the last new pipeline.
void SetPipelineCacheFile(const std::string& path);
void MaybeSavePipelineCache();
// Card S3-57 (WWE2K14_NR_PREWARM=1): once per process, at the first swap after Init, creates the pipelines of the
// recorded key list (<cache stem>.keys) on a background thread into the pipeline cache. `shaders` returns every
// SPIR-V byte vector of the loaded shader set (read-only, alive for the process); the list names shaders by hash.
void MaybeStartPipelinePrewarm(const std::function<std::vector<const std::vector<uint8_t>*>()>& shaders);
// Card R14: progress of the pipeline prewarm (UI thread reads it) and the before-play wait (swap thread, once).
struct PrewarmStatus {
  bool running = false, before_play = false;
  size_t done = 0, total = 0;
  double seconds = 0.0;
};
struct DirectVertexUploadStats;
struct RecReuseStats;
PrewarmStatus GetPrewarmStatus();
void WaitPipelinePrewarm();
bool UsesSharedDevice();  // Init adopted the shared device
bool Init(const std::string& shader_dir, std::string* error);
bool Ready();
// Lane-only V17c frame scheduler. Call before translating each D3D draw so phase changes precede its backend submit.
void SetV17cFrame(uint32_t frame);
uint32_t DirectVertexUploadMode();
bool CanDirectVertexUpload(size_t allocation_bytes, uint64_t ps_key, uint32_t mode, bool* skip_legacy_copy);
DirectVertexUploadStats GetDirectVertexUploadStats();
RecReuseStats GetRecReuseStats();
// WWE2K14_NR_FIX_ALT: phase changes only at the caller's swap boundary; B-phase knobs use an atomic phase flag.
bool FixAltEnabled();
int FixAltPeriodSeconds();
bool FixAltPhaseB();
int FixAltFrame(uint64_t now_ns);
// Card U2c: sample WWE2K14_D3D_CROWD_GROUP once at the frame boundary after FA1's optional phase update. On a mode change,
// drain/flush old-mode draws before publishing the next-frame mode. Returns -1 if unchanged; otherwise the new mode.
int CrowdFrameStart();
// Card U2c: report the latched mode's counters at the swap callback; this does not resample the knob.
void CrowdFrame(uint32_t swap);
std::vector<std::string> CrowdTakeLog();
// Surface fallback warnings queued by the backend and drained by the D3D layer at the swap boundary.
std::vector<std::string> SurfaceTakeWarnings();
// Card S3-29b diagnostic: 0 none, 1 Kick ring-wrap fence, 2 WaitSerial fence, 3 WaitAll fence, 4 record-thread Sync.
uint32_t BackendWaitCode();
// Card S3-39 (WWE2K14_D3D_SCALE_DYNAMIC=1 with WWE2K14_D3D_SCENE_SCALE): scaled surfaces keep a native-size set too.
// SetSceneScaleActive(false) makes every such surface use its native set (menus sharp), true the scaled one; call
// between frames. Returns the number of surfaces switched.
bool SurfaceHasAlt(uint32_t surface);
uint32_t SetSceneScaleActive(bool active);
// Card S3-6 (draw-off mode): free retired buffers/textures/snapshots per presented frame (2 frames later) instead of
// only at the next full GPU wait. No effect with WWE2K14_NR_FRAME_SLOTS=1 (frame slots already do this).
void SetRetireByFrame(bool on);
// Card S3-15 (draw-off): colour snapshots are copied only when read or before their source is written again.
void SetLazySnapshots(bool on);
// created, materialized, by_reader, by_write, skipped (never copied) - cumulative.
void LazySnapshotStats(uint64_t out[5]);
// Card S3-8: submit everything recorded and wait for the GPU to finish it (caller holds the backend lock).
void FinishGpuWork();
// Card S3-8: split form for waiting without holding the lock: KickForSync (lock held) returns the serial to reach,
// GpuReachedSerial (lock held briefly, non-blocking) retires finished lists and reports whether all up to it finished.
uint64_t KickForSync();
bool GpuReachedSerial(uint64_t serial);
// True when the active backend consumes DXIL (D3D12); shader files are then <name>.dxil instead of .spv.
// The single backend selector used before Init as well as by Init; true only for the Windows-only
// WWE2K14_NR_BACKEND=d3d12 request. An unset/other value stays on Vulkan.
bool BackendWantsDxil();
bool ShaderFormatIsDxil();
std::string DeviceName();

// WWE2K14_NR_MEM_OWNERS=1 (default off): D3D frontend age counts at each present;
// backend attribution samples only when MemoryOwnersDue() is true (10 s cadence).
struct MemoryOwnerFrontendAges {
  uint64_t texture_entries = 0, texture_old600 = 0;
  uint64_t resolve_entries = 0, resolve_old600 = 0;
  std::vector<uint32_t> resolve_snapshot_ids;
};
constexpr uint64_t MemoryOwnerResidual(uint64_t total, uint64_t attributed) noexcept {
  return total > attributed ? total - attributed : 0;
}
bool MemoryOwnersDue();
// GAP-mem1 (WWE2K14_NR_MEM_LEAN=bits, default 0, read once): shrink memory that has no picture effect.
//  1 the resolve history ring holds a snapshot only for its newest record   2 snapshot pool cap 128 MB, not 512
//  4 upload rings start at 16 MB and grow on demand, not 64 MB              8 constants ring 6144 slots, not 12288
//  16 reserved: allocator blocks 32 MB (lane mem1 only, refuted; no effect in this build, its plume change is not carried)
inline constexpr uint32_t kMemLeanRing = 1, kMemLeanPool = 2, kMemLeanUpload = 4, kMemLeanConst = 8;
uint32_t MemLeanMode();
std::string MemoryOwnersLine(const MemoryOwnerFrontendAges& frontend);

// A host render target: one per SDK RenderTargetKey (EDRAM base, pitch, MSAA, depth/colour, format),
// created once per key. Depth surfaces (kD24S8 / kD24FS8) are D24 unorm S8 / D32F S8 as in the SDK. Colour surfaces of 16/32-bit-per-component
// formats are also viewed as integers (exact bits for EDRAM transfers and clears).
uint32_t Surface(uint64_t key, uint32_t width, uint32_t height, bool depth, TargetFormat format, uint32_t samples,
                 std::string* error);
// Card S3-52: frees an evicted render target (waits for the GPU first; rare). false: unknown handle or still in use by a pending lazy copy.
bool ReleaseSurface(uint32_t handle);
// Resolve reuse (WWE2K14_NR_RESOLVE_REUSE=1): SnapshotColor copies a region of an RGBA8 colour surface (averaged if
// multisampled) into a new sampled image and returns its id (kNone on failure); TextureFromSnapshot makes a
// bindless texture (view with desc.swizzle) on that image without any detile/upload; ReleaseSnapshot drops the id.
uint32_t SnapshotColor(uint32_t surface, uint32_t x, uint32_t y, uint32_t w, uint32_t h, std::string* error);
void ReleaseSnapshot(uint32_t id);
uint32_t TextureFromSnapshot(uint32_t id, const TextureDesc& desc, std::string* error);
// Card M2c3 (d3d shadow: resolve destinations written by several sub-rectangle resolves). NewSnapshotLike: a new w x h
// snapshot image of snapshot `like`'s format (contents undefined until copied into). CopySnapshotRegion: copies the
// w x h region at (sx, sy) of snapshot `src` to (dx, dy) of snapshot `dst` (same format; the caller keeps both rects
// inside their images). TextureFromSnapshotCrop: a texture of the fetch size holding region (0,0)-(desc.width,
// desc.height) of a snapshot at least that large (any format TextureFromSnapshot accepts), in a new image.
// Card S3-69: `clear` = clear the new image to transparent black (as WWE2K14_D3D_CANVAS_CLEAR=1 does for every canvas).
uint32_t NewSnapshotLike(uint32_t like, uint32_t w, uint32_t h, std::string* error, bool clear = false);
bool CopySnapshotRegion(uint32_t dst, uint32_t dx, uint32_t dy, uint32_t src, uint32_t sx, uint32_t sy, uint32_t w,
                        uint32_t h, std::string* error);
uint32_t TextureFromSnapshotCrop(uint32_t id, const TextureDesc& desc, std::string* error);
// Card M2c4: a cube texture (desc.cube, desc.width x desc.height faces, one level) whose face f is region
// (0,0)-(width,height) of snapshot faces[f] (all six the same format, each at least that large).
uint32_t TextureCubeFromSnapshots(const uint32_t faces[6], const TextureDesc& desc, std::string* error);
// Card M2c5: a 2D texture with `levels` mips whose level n is region (0,0)-(max(1,w>>n),max(1,h>>n)) of snapshot
// ids[n] (all the same format, each at least that large).
uint32_t TextureFromSnapshotLevels(const uint32_t* ids, uint32_t levels, const TextureDesc& desc, std::string* error);
// Card M2c6: copies rects (x0,y0,x1,y1 quadruples, `count` of them) of snapshot `snapshot` to the same coordinates of
// level 0 of texture `texture` (made by CreateTexture). The image formats must match (checked).
// Card S3-33h: snapshot (w x h from its origin) into mip `level` of a same-format texture.
bool CopySnapshotToTextureLevel(uint32_t texture, uint32_t snapshot, uint32_t level, uint32_t w, uint32_t h,
                                std::string* error);
// Card S3-69 (5c): snapshot (w x h from its origin) into array layer / cube face `layer` of mip `level` of a texture
// of the same format (cube textures made by TextureCubeFromSnapshots).
bool CopySnapshotToTextureLayer(uint32_t texture, uint32_t snapshot, uint32_t level, uint32_t layer, uint32_t w,
                                uint32_t h, std::string* error);
bool CopySnapshotRectsToTexture(uint32_t texture, uint32_t snapshot, const int32_t* rects, uint32_t count,
                                std::string* error);
// Card S3-69 (5a): true when the backend rejects the MipsOn/CubeOn 5b/5c paths (record-thread queueing makes their
// queued CreateTexture/copy calls unreliable; see dest_images MipsOn/CubeOn). Vulkan build: always false.
bool IsD3D12();
// Card S3-69 (5a): fills the whole w x h snapshot `snapshot` from a tight CPU byte vector (w * h * format size,
// w rows of w * format size bytes; no padding). Same format as the snapshot's. Uploads via a staging buffer +
// copy like CreateTexture level 0 (Vulkan only).
bool FillSnapshotRows(uint32_t snapshot, uint32_t w, uint32_t h, const std::vector<uint8_t>& bytes,
                      std::string* error);
// RetainSnapshot returns a second id for the same image (each id is released on its own).
uint32_t RetainSnapshot(uint32_t id);
// Direct GPU presentation: submits the recorded work (no wait) and returns a VkImageView (as an integer) of snapshot
// `id` with the given swizzle (TextureDesc::swizzle encoding), in SHADER_READ_ONLY_OPTIMAL. The view and its image
// stay alive for the next few presents (the presenter may still sample them). 0 on failure.
// Reads snapshot `id` (RGBA8, w x h) back to the CPU (WWE2K14_NR_SHOT_EVERY; one readback).
bool ReadSnapshot(uint32_t id, uint32_t w, uint32_t h, std::vector<uint8_t>& bytes, std::string* error);
// RR2 offline replay (no presenter): the frame boundary PresentViewOfSnapshot ends a frame with (submit recorded work,
// FrameEnd retirement), without creating a view or needing the shared device.
void OfflineFrameEnd();
// Record-then-replay (resolve images made before the recording window): reads snapshot `id` back in its own format
// (single-sample 2D colour, tight rows; *format = RenderFormat value) / makes a new snapshot of that format from such
// bytes. false / kNone (error set) for depth, multisampled or block-compressed snapshots.
bool ReadSnapshotRaw(uint32_t id, uint32_t* format, uint32_t* w, uint32_t* h, std::vector<uint8_t>& bytes,
                     std::string* error);
uint32_t CreateSnapshotRaw(uint32_t format, uint32_t w, uint32_t h, const std::vector<uint8_t>& bytes, std::string* error);
// Reads mip 0 / face 0 of a live sampled texture back in its upload format. Diagnostic use only; one GPU readback.
bool ReadTexture(uint32_t handle, TexFormat format, uint32_t w, uint32_t h, std::vector<uint8_t>& bytes,
                 std::string* error);
// Diagnostic/check readback of mip `level` (one face of a non-cube texture) from its live host image.
bool ReadTextureLevel(uint32_t handle, TexFormat format, uint32_t level, uint32_t w, uint32_t h,
                      std::vector<uint8_t>& bytes, std::string* error);
// Card S3-47a: *width / *height (optional) receive the presented image's size - the physical size when the snapshot
// has a physical companion (whole-surface snapshot of a surface scaled above native size), otherwise left untouched.
uint64_t PresentViewOfSnapshot(uint32_t id, const uint8_t swizzle[4], std::string* error, uint32_t* width = nullptr,
                               uint32_t* height = nullptr);
// Card S3-44 diagnostic (WWE2K14_NR_FLASH_TRACE=1): pops the present index (0-based count of PresentViewOfSnapshot calls)
// of the next detected spike / repeat-old frame.
bool FlashTakeEvent(uint64_t* frame);
// Card S3-44 diagnostic: measure snapshot `id` (RGBA8) for the present being built, labelled `tag` (resolve dest base).
void FlashTapSnapshot(uint32_t id, uint32_t tag);
// Host size of a surface returned by Surface(); false for kNone / unknown handles.
bool SurfaceSize(uint32_t surface, uint32_t* width, uint32_t* height);
// Clears a rectangle of a surface (resolve-with-clear). Colour: `rgba` floats, or with
// `integer_view` the raw bits of `rgba` (as uint32) through the integer view.
bool ClearColor(uint32_t surface, int32_t x, int32_t y, int32_t w, int32_t h, const float rgba[4], bool integer_view,
                std::string* error);
bool ClearDepthStencil(uint32_t surface, int32_t x, int32_t y, int32_t w, int32_t h, bool depth, float z,
                       bool stencil, uint32_t s, std::string* error);
uint32_t CreateTexture(const TextureDesc& desc, const std::vector<LevelData>& levels, std::string* error);
// FL-4 Design A (WWE2K14_D3D_LATE_TEX=4, src/native/d3d/late_upload.cpp): rewrite the level bytes CreateTexture
// uploaded through its staging buffer, while that command list has not been submitted. Same staging layout and row
// rules as CreateTexture; false (with *error) when the texture was released, the list was submitted, or a level size
// differs - the caller keeps its translate-time bytes in that case.
bool RefillTextureUpload(uint32_t handle, const std::vector<LevelData>& levels, std::string* error);
// FL-4 Design A: defer the early submit (WWE2K14_NR_EARLY_SUBMIT) while a texture upload is held. The backend does
// not know what the hold is for; the holder clears it at its release boundary.
void SetKickHold(bool hold);
// Frees a texture and recycles its table slot. Recorded commands may still sample it, so the texture and
// its slot are kept until the next GPU wait; call it between draws.
void ReleaseTexture(uint32_t handle);

bool ExecuteDraw(const Draw& draw, std::string* error);
// Record thread (WWE2K14_NR_RECORD_THREAD=1, see nr_record_queue.h): AcquireDraw returns a cleared Draw to fill (a pooled
// slot; valid until SubmitDraw or the next AcquireDraw). SubmitDraw hands it to the record thread (async: returns true
// at once, a failure is reported later through TakeDrawFailures) or, with the switch off, records it immediately
// exactly as ExecuteDraw does. Every other backend call first waits until all submitted draws are recorded, so the
// call order the frontend sees is the order on the GPU timeline; GetStats()/the profile reads wait too.
Draw* AcquireDraw();
bool SubmitDraw(Draw* draw, std::string* error);
struct ConstUsedStats {
  uint64_t copied_registers = 0, cleared_registers = 0, used_row_mismatches = 0;
  uint64_t batch_hash_differences = 0, crowd_decision_differences = 0;
};
void ConstUsedRecordCopy(uint32_t copied, uint32_t cleared, uint32_t mismatches);
void ConstUsedRecordBatchDifference();
void ConstUsedRecordCrowdDifference();
ConstUsedStats ConstUsedTakeStats();
void ConstUsedFrameBoundary();
// Releases a producer-local batched draw that was dropped during translation; pending backend operations stay ordered
// ahead of the next submitted ring entry.
void AbandonDraw();
struct DrawFailure {
  uint32_t ordinal;
  std::string error;
};
std::vector<DrawFailure> TakeDrawFailures();  // failures of asynchronous draws since the last call (waits for the thread)
bool RecordThreadActive();
bool TakeRecordThreadCpuDeltaNs(uint64_t* delta_ns);  // opt-in per-swap CPU delta; does not sync the queue
uintptr_t RecordThreadNativeHandle();  // pthread_t (Linux) / HANDLE (Windows) of the record thread, else 0 (PC sampler)
unsigned long RecordThreadHandle();  // pthread_t of the record thread on Linux, else 0 (PC sampler)
// Cheap, non-waiting read of Stats::total_ns for the frontend's own exclusive-time accounting.
uint64_t TotalNs();
// Colour of a colour surface, rect x,y,w,h, in the host format (rows of w pixels); multisampled
// surfaces are averaged first (the guest copy's k0123 sample select).
bool ReadColor(uint32_t surface, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
               std::vector<uint8_t>& bytes, std::string* error);

// Resolve / fixed16 conversion setup shared by D3D host-image snapshots. ResolveAvailable() reports whether its
// descriptor layout and LUT storage were created; the caller fills the LUT before SnapshotFixed16 is used.
struct ResolveParams {
  uint32_t w, h, src_bpp, bpp_log2, flags, endian, dx, dy, pitch_aligned;
  int32_t base_offset;
  uint32_t dst_size, x0, y0, kbase, kpitch32, select, ms;
  uint32_t dst_start;  // physical address of the destination extent (G2: its pages must be in the mirror)
};
bool ResolveAvailable();
// lut: 65536 fixed16 entries (int16 bits -> uint16) then 256 rgba4 entries (byte -> nibble), built by the
// frontend with the CPU expressions.
bool SetResolveLut(const std::vector<uint32_t>& lut, std::string* error);
// The float24 depth of a flat depth surface as the R32F texel values a k_24_8_FLOAT fetch of the D3D depth copy-out
// would read, in a new snapshot image (TextureFromSnapshot with a kR32F desc; ReleaseSnapshot). p = ResolveParams.
// kNone on failure.
bool DepthSnapshotAvailable();
uint32_t SnapshotDepthR32F(uint32_t surface, const ResolveParams& p, std::string* error);
// D3D fixed16 colour resolve (p.flags & 2) as the 16-bit UNORM texel values a
// k_16_16(_16_16) fetch of the copy reads after detiling, in a new snapshot image (TextureFromSnapshot with a
// kRGBA16 / kRG16 desc; ReleaseSnapshot). (x, y) = source rectangle origin. kNone on failure.
bool Fixed16SnapshotAvailable();
uint32_t SnapshotFixed16(uint32_t surface, uint32_t x, uint32_t y, const ResolveParams& p, std::string* error);
// Texture levels detiled on the GPU from explicit source bytes into a snapshot (tools/nr/xenos/detile.hlsl).
struct DetileParams {
  uint32_t src_base, row_pitch, pitch_blocks, off_x, off_y, bw, bh, bpb_log2, tiled, endian, mode, out_offset,
      out_size;
};
// mode: 0 raw bytes, 1 rgba4, 2 k_24_8 (UNorm24), 3 k_24_8_FLOAT (20e4). Modes 2/3 require a shader
// directory carrying the edram_detile.depth24 marker (DetileDepth24Available()); old detile shaders only
// understand modes 0/1.
struct GpuLevel {
  uint32_t width = 1, height = 1, mip = 0, face = 0;
  DetileParams p{};
};
bool DetileAvailable();
// True when the detile pipeline is loaded AND its shader directory carries the edram_detile.depth24
// sidecar marker (i.e. the edram_detile.spv was built from a detile.hlsl with depth24 modes 2/3).
bool DetileDepth24Available();
// Card S3-69 B2-perf2: upload one bounded guest-memory source range into an isolated compute-detile pool, detile it
// with the existing edram_detile pipeline, and copy the tight result into a destination canvas snapshot. `source`
// starts at level.p.src_base; its final partial page is zero-padded, so bytes beyond source_bytes are not read.
// Optional `dump_level` reads back the GPU-detiled bytes and waits (trace only). Timings are host upload/dispatch
// recording time; the existing GPU_ITEMS timer reports actual GPU dispatch time when enabled.
struct SeedDetileStats {
  uint64_t upload_ns = 0, dispatch_submit_ns = 0, source_bytes = 0, padded_upload_bytes = 0;
};
bool DetileSnapshotGpu(uint32_t snapshot, const GpuLevel& level, uint32_t source_bytes, const uint8_t* source,
                       std::vector<uint8_t>* dump_level, SeedDetileStats* stats, std::string* error);

// M4 instrument I1 (off unless SetProfiling(true); the D3D runtime enables it with WWE2K14_NR_PROFILE=1):
// inclusive wall time of every public call (outermost call only), GPU round trips (Submit = execute +
// fence wait), pipeline creation, buffer creation and readback bytes. The caller reads and resets.
enum ProfFn : uint32_t {
  kProfSurface, kProfClear, kProfCreateTexture, kProfReleaseTexture, kProfExecuteDraw, kProfReadColor, kProfResolve,
  kProfSeedDetile,
  // Sub-timers inside ExecuteDraw (decompose draw_exec; always on when profiling, independent of the
  // opt-in render flags). These are not public calls: ProfScope counts only the outermost public call,
  // so the backend uses a separate SubTimer that adds to fn_ns[..]/fn_calls[..] without touching total_ns.
  kProfExecConsts, kProfExecPipeline, kProfExecUpload, kProfExecRecord, kProfFnCount
};
// WWE2K14_NR_RECORD_BATCH_STATS=1: producer-side record-consumer drain points, counted per frontend frame.
enum BatchDrainKind : uint32_t {
  kBatchDrainPublicSurface, kBatchDrainPublicClear, kBatchDrainPublicCreateTexture, kBatchDrainPublicReleaseTexture,
  kBatchDrainPublicDraw, kBatchDrainPublicReadColor, kBatchDrainPublicResolve, kBatchDrainPublicDetile,
  kBatchDrainValueCall, kBatchDrainSurfaceRelease, kBatchDrainSurfaceAlt, kBatchDrainTextureOrder,
  kBatchDrainReadSnapshot, kBatchDrainReadTexture, kBatchDrainSnapshotTable, kBatchDrainPresent,
  kBatchDrainResolveLut, kBatchDrainKick, kBatchDrainFinish, kBatchDrainDetile, kBatchDrainFrameTail,
  kBatchDrainRingFull, kBatchDrainKindCount
};
struct Stats {
  uint64_t fn_ns[kProfFnCount] = {}, fn_calls[kProfFnCount] = {};
  uint64_t total_ns = 0;                      // sum of fn_ns
  uint64_t submits = 0, submit_ns = 0;        // Submit(): time blocked waiting for the GPU (fence waits)
  uint64_t kicks = 0;                         // Kick(): non-waiting early submits (WWE2K14_NR_EARLY_SUBMIT)
  uint64_t pipelines = 0, pipeline_ns = 0;    // createGraphicsPipeline (draw)
  uint64_t pipelines_adopted = 0;             // WWE2K14_NR_PREWARM_ADOPT: warm pipelines used by a live draw
  uint64_t buffers = 0;                       // createBuffer calls
  uint64_t ring_fallbacks = 0;                // vertex/index uploads that fell back to per-draw createBuffer despite the upload ring
  uint64_t pipeline_collisions = 0;           // WWE2K14_NR_DRAW_FAST=1: hash hit whose full key bytes differed (true collision; expect 0)
  uint64_t readback_bytes = 0, upload_bytes = 0;
  uint64_t elide_passes = 0;                  // WWE2K14_NR_STATE_ELIDE=1: draws that had to (re)set the framebuffer (each begins a new render pass)
  uint64_t elide_binds_skipped = 0;           // WWE2K14_NR_STATE_ELIDE=1: redundant bind calls skipped (pipeline/layout/4 sets/viewport/scissor)
  uint64_t const_blocks_written = 0;          // WWE2K14_NR_CONST_DEDUPE=1: blocks actually written to the ring (0 when the flag is off)
  uint64_t const_blocks_reused = 0;           // WWE2K14_NR_CONST_DEDUPE=1: blocks whose unchanged bytes reused a prior address (0 when off)
  uint64_t const_wraps = 0;                   // card P24: constants ring wraps (Submit() without CONST_FENCE, fenced reuse with it)
  uint64_t const_wrap_wait_ns = 0;            // card P24: CPU time blocked by those wraps (the Submit, or the fenced list waits)
  uint64_t const_fence_waits = 0;             // card P24 (WWE2K14_NR_CONST_FENCE=1): command lists waited for so their ring slots could be reused
  // Card P22: vkCmdPipelineBarrier calls nr caused (counted in plume; every one plume records), barrier structs in
  // them, and calls with an all-stage mask on a side (every call in the default mode).
  uint64_t snap_direct_resolve = 0, snap_pool_hits = 0, snap_pool_misses = 0, snap_pool_size = 0;  // card L2-B (size = gauge)
  uint64_t tex_alloc_fail = 0;  // card M2b2-A: NewTexture failures (VkImage allocation failed; nullptr returned)
  uint64_t vk_barrier_calls = 0, vk_barrier_items = 0, vk_barrier_legacy = 0;
  // U7 physical-size snapshot path and its downsample comparison (WWE2K14_NR_PHYS_SNAP[_CHECK]).
  uint64_t phys_snap_color = 0, phys_snap_depth = 0, phys_snap_logical_materialized = 0;
  uint64_t phys_snap_checks = 0, phys_snap_mismatch_texels = 0, phys_snap_check_failures = 0;
  // CHECK tolerance view: texels off by more than 4/255 (colour) or 1e-3 (depth); worst colour step, worst depth x1e6.
  uint64_t phys_snap_over_tol = 0, phys_snap_max_color = 0, phys_snap_max_depth_e6 = 0;
  // WWE2K14_NR_GPU_TIMERS=1: GPU time per public call kind (timestamps at each change of kind), harvested
  // after each fence wait - so it lands in the frame whose Submit waited for it.
  uint64_t gpu_ns[kProfFnCount] = {};
  uint64_t gpu_lists = 0, gpu_marks = 0, gpu_overflow = 0;
  // WWE2K14_NR_RECORD_THREAD=1: draw_exec / exec_* above are then the record thread's time (not part of total_ns, which
  // is the renderer thread's own busy time); these describe the hand-off.
  uint64_t rec_draws = 0;      // draws recorded by the record thread
  uint64_t rec_wait_ns = 0;    // renderer thread blocked waiting for the record thread at a barrier (included in total_ns)
  uint64_t rec_tail_ns = 0;    // wait for the record thread's backlog at the end-of-frame stats read (not in total_ns: with profiling
                               // off that backlog overlaps the next frame's parse)
  uint64_t rec_syncs = 0;      // barriers that actually had to wait
  uint64_t rec_full_waits = 0; // Acquire() found every slot in flight
  uint64_t rec_full_wait_ns = 0;  // renderer thread blocked in those Acquire() calls (AcquireDraw: not in total_ns, so the
                                  // frontend attributes it to the FeScope around the draw, draw_state)
  uint64_t rec_sleeps = 0;     // record thread went to sleep
  uint64_t rec_failed = 0;
  uint64_t rec_barrier[kProfFnCount + 1] = {};     // barriers taken, by the public call that needed them (with draws pending); [kProfFnCount] = snapshot/misc calls
  uint64_t rec_barrier_ns[kProfFnCount + 1] = {};  // renderer-thread wait of those barriers
  uint64_t batch_drain_calls[kBatchDrainKindCount] = {}, batch_drain_waits[kBatchDrainKindCount] = {};
  uint64_t batch_drain_ns[kBatchDrainKindCount] = {};  // time the translate thread waited for queued record work
  uint64_t batch_check_frames = 0, batch_check_mismatches = 0;
};

struct DirectVertexUploadStats {
  uint32_t mode = 0;
  uint64_t direct_draws = 0, direct_bytes = 0;
  uint64_t check_draws = 0, check_bytes = 0, mismatch_draws = 0, mismatch_bytes = 0;
  uint64_t not_record_queue = 0, not_batch = 0, not_crowd_candidate = 0, not_ring_room = 0, not_mode_off = 0;
};

struct RecReuseStats {
  uint32_t mode = 0;
  uint64_t hits = 0, checks = 0, mismatches = 0;
};

struct RecordBatchWatchdogState {
  uint64_t head = 0;          // published producer head
  uint64_t committed = 0;     // producer tail, including entries not yet batch-published
  uint64_t tail = 0;          // record consumer completion sequence
  uint64_t spurious_recoveries = 0;  // timed waits that found work missed by notification
  uint64_t executing_slot = 0;
  uint64_t current_job_hash = 0;
  uint64_t wait_serial = 0;
  uint32_t queue_state = 0;   // 0 idle/spinning, 1 executing callback, 2 asleep awaiting work, 3 notifying producer
  uint32_t phase = 0;         // 0 idle, 1 slot, 2 release, 3 async job, 4 draw
  uint32_t current_ordinal = 0;
  uint32_t current_item = 0;
  uint32_t consumer_wait = 0; // 0 none, 1 Plume queue submit, 2 command-fence wait, 3 record-thread mutex
  uint32_t backend_wait_site = 0; // existing reason: 0 none, 1 Kick, 2 WaitSerial, 3 WaitAll, 4 producer Sync
  const char* operation = nullptr;
  const char* wait_name = nullptr;
  bool producer_blocked = false;
  bool producer_sync_waiting = false;
};

void SetProfiling(bool on);
Stats& GetStats();
// Per-frame diagnostics for the record batch; empty when WWE2K14_NR_RECORD_BATCH_STATS is off.
std::string BatchDrainStatsLine();
void RecordQueueFrame(uint64_t frame);  // night 11-18: WWE2K14_NR_RECORD_SLOTS(_AB)
// Flush and close one frame's producer/consumer operation streams when WWE2K14_NR_RECORD_BATCH_CHECK=1.
void RecordBatchFrameEnd(uint64_t frame);
// Atomic-only snapshot for the d3d-shadow hang watchdog; does not drain the queue.
bool GetRecordBatchWatchdogState(RecordBatchWatchdogState* state);
// Card M2b2-B: per memory heap "h0=usage/budgetMB ..." from VMA (vmaGetHeapBudgets, thread-safe); empty without a device.
std::string HeapBudgets();
// Issue plan step 5 (11-18, GH126/140/159/166 driver stops on 4-8 GB iGPU PCs): when VMA's total block bytes reach a new
// 256 MB high, logs one "[nr-mem-kinds]" line splitting every live VMA allocation into render targets / depth, sampled
// textures and buffers (vmaBuildStatsString, detailed). At most 16 lines a session; WWE2K14_NR_MEM_KINDS=0 switches it off.
std::string MemKindsOnGrowth();  // empty when nothing to log
// Card V1: usage / budget bytes of the first DEVICE_LOCAL heap (the same vmaGetHeapBudgets sample HeapBudgets prints;
// with WWE2K14_NR_MEM_BUDGET=0 the budget is VMA's heap-size estimate). false without a device.
bool LocalHeapBudget(uint64_t* usage, uint64_t* budget);
// Card V1 round 2: bytes of render-target images (colour/depth image, MSAA resolved copy, scaled alt set; live and
// retired-not-yet-freed) placed in heap 0 when it is device-local - the same partition as [nr-mem-owners] rt=.
uint64_t SurfaceDeviceLocalBytes();
// Card V1 round 3: trigger-heap bytes for one live surface handle (0 for an unknown/released handle or host placement).
// Both, and LocalHeapBudget, use one heap: the first DEVICE_LOCAL heap (review F1; logged once if it is not heap 0).
uint64_t SurfaceLocalHeapBytes(uint32_t handle);
// V19: batched live-handle allocation sizes and lazy-read state. Bytes include each surface's image, MSAA resolve,
// scaled alternate set, and FSR1 physical snapshots that are owned by that SurfaceObj. pending_read means a queued lazy
// snapshot still reads the surface; callers must not evict it. Returns false when this Vulkan-only query is unavailable.
struct SurfaceMemoryInfo {
  uint64_t local_heap_bytes = 0;
  bool pending_read = false;
  bool live = false;
  bool retired = false;
};
bool GetSurfaceMemoryInfo(const std::vector<uint32_t>& handles, std::vector<SurfaceMemoryInfo>* out);
// Card D1 (WWE2K14_NR_GPU_DRAWS=1 with GPU_TIMERS=1 and profiling): GPU timestamp pair per draw / backend operation,
// summarised per frame on stderr ([nr-gpu-frame], every 60 frames [nr-gpu-top]). GpuDrawsFrame = the frame boundary,
// called once per swap after the frame's work; `frame` = the [nr-profile] frame number. No-op when off.
void GpuDrawsFrame(uint64_t frame, bool scene_once);
bool GpuDrawsActive();

}  // namespace nr::backend
