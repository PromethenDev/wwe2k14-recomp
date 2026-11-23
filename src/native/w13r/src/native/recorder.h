#pragma once

#include <cstdint>
#include <functional>
#include <rex/ppc/context.h>

// WWE '13 native-renderer recorder (cvar wwe13_record_frames, default 0 = off), for offline replay.
//
// <dir>/recording.w13r = FileHeader, then chunks (ChunkHeader + payload, deflate unless stored_bytes ==
// raw_bytes). The decompressed payloads, concatenated, are a stream of Records (RecordHeader + body):
//   kBasePage / kFramePage  {u32 space (0 virtual, 1 physical), u32 page offset in that space, 4096 bytes}
//       kBasePage: full snapshot of readable game memory, taken at the first recorded frame.
//       kFramePage: a page that changed since the previous scan. A scan runs at the first hook of
//       every frame, after the game's job threads have written that frame's commands and data.
//   kDevice    {u32 device address, then runs {u16 offset, u16 length, bytes...}}: changed bytes of the
//              D3D device block (register shadows, constants) since the previous hook.
//   kCall      EventRecord: one hooked D3D call, in order.
//   kFrameEnd  {u32 frame index}
// Replay: restore memory from page records, the device block from kDevice before each call, then run
// the renderer side of the hook with the recorded registers.
namespace svr::recorder {
constexpr char kMagic[8] = {'W', '1', '3', 'R', 'E', 'C', '2', 0};
constexpr uint32_t kVersion = 2;
constexpr uint32_t kPageBytes = 4096;
constexpr uint32_t kDeviceBytes = 0x5FB0;  // sizeof(bd::gpu::D3DDevice), reblue/gpu/d3d.h

struct FileHeader {
  char magic[8];
  uint32_t version;
  uint32_t page_bytes;
};
struct ChunkHeader {
  uint32_t raw_bytes;
  uint32_t stored_bytes;
};
// kShader {u32 shader object, u32 pixel, container bytes} / kDecl {u32 declaration object, elements up to stream
// 0xFF}: the game hands shader microcode and vertex elements to its Create calls once, so the recorder keeps a copy
// of every one made since boot and writes them all at the start (and each new one before its Create call); replay
// registers them first and skips the recorded Create calls.
// kMem {u32 space (0 virtual, 1 physical), u32 address/offset, u32 length, bytes}: memory the game wrote mid-frame
// that a draw reads before the next frame-start scan (BeginVertices data at EndVertices, a vertex/index buffer at
// its Unlock, the rect/point/colour blocks Resolve, Clear and SetScissorRect read through pointers); written just
// before the call record that consumes it.
enum RecordType : uint32_t { kBasePage = 1, kFramePage = 2, kDevice = 3, kCall = 4, kFrameEnd = 5, kShader = 6, kDecl = 7, kMem = 8, kTarget = 9 };
// kTarget {u32 texture VA, u32 scale in quarters (4 = 1x), u32 face, u32 level, u32 width, u32 height, u32 row pitch,
// texels}: a GPU-written texture's content at the start (after the base snapshot), e.g. reflection cubes the game
// renders once at match load and only samples afterwards. 4-byte-per-texel formats only.
struct RecordHeader {
  uint32_t type;
  uint32_t bytes;  // body size
};
struct EventRecord {
  uint32_t hook_id;  // HookId(name of the hooked subroutine, e.g. "D3DDevice_DrawIndexedVertices")
  uint32_t flags;    // bit 0: return_value is meaningful (SVR_HOOK_RESULT)
  uint64_t timestamp_ns;
  uint64_t return_value;
  uint64_t registers[21];  // r1-r13 followed by f1-f8, raw PPCContext register bits
};

// FNV-1a of the hooked subroutine name: stable across builds and backends.
uint32_t HookId(const char* name);
void RecordHook(const char* name, const PPCContext& args, uint32_t return_value, bool has_return);

// --- Replay hook table -----------------------------------------------------
// The SVR_HOOK_* macros register the renderer side of every hook here, keyed by
// HookId(name), so the replay can run it from the recorded registers without the
// game. The registration happens in the same translation unit as the (often
// file-static) renderer function, so nothing needs to become externally visible.
enum class HookKind : uint32_t { kCall = 0, kResult = 1 };

// A renderer entry the replay invokes with the recorded registers. For kCall (a
// BEFORE/AFTER hook) the trampoline ignores return_value and re-translates the
// registers through HostToGuestFunction; for kResult (a RESULT hook) the observer
// reads return_value directly. Same shape either way.
using ReplayHookFn = void (*)(PPCContext& args, uint32_t return_value, uint8_t* base);

struct HookEntry {
  uint32_t id;
  const char* name;
  ReplayHookFn fn;
  HookKind kind;
};

// Registers a hook, returning its stored entry (callable from a static initializer).
// Idempotent: a duplicate name returns the existing entry.
const HookEntry* RegisterHook(const char* name, ReplayHookFn fn, HookKind kind);

// Looks a hook up by its FNV-1a id (EventRecord.hook_id), or nullptr when this build
// does not hook that call (e.g. a call compiled out by wwe13_d3d_names.h switches).
const HookEntry* FindHook(uint32_t id);

// Iterates every registered hook (diagnostics / the replay's summary).
void ForEachHook(const std::function<void(const HookEntry&)>& visitor);
}  // namespace svr::recorder
