// T2 sampled, one-line glitch diagnostics. All observations use the current D3D draw state and its guest memory.
#pragma once

#include <cstddef>
#include <cstdint>

namespace wwe2k14::d3d::glitch {

struct DrawContext {
  uint32_t frame = 0;
  uint32_t draw = 0;
  uint32_t event_seq = 0;
  uint64_t vs_key = 0, ps_key = 0;
  bool sampled = false;
};

uint32_t SampleInterval();  // WWE2K14_GLITCH_CHECK, unset/invalid/zero = off
void BeginDraw(uint32_t frame, uint32_t event_seq);
void SetShaderKeys(uint64_t vs_key, uint64_t ps_key);
const DrawContext& CurrentDraw();

void MissingShader(const DrawContext& draw, uint8_t stage, uint64_t shader_key, const char* reason,
                   uint64_t ucode_key = 0, uint32_t dwords = 0, const char* dump = "none");
void CheckConstants(const DrawContext& draw, const uint8_t* vs_constants, const uint8_t* ps_constants,
                    const uint64_t* vs_constant_usage, const uint64_t* ps_constant_usage, size_t bytes_per_stage);

// A texture probe is attempted only once per fetch identity and only on sampled frames.
bool ClaimUnwrittenTextureProbe(const DrawContext& draw, uint32_t base, uint32_t mip_base, uint32_t mip_bytes,
                                uint32_t format, uint32_t width, uint32_t height, uint32_t dimension);
void UnwrittenTextureCandidate(const DrawContext& draw, uint32_t slot, uint32_t base, uint32_t bytes,
                               uint32_t mip_base, uint32_t mip_bytes, uint32_t format, uint32_t width,
                               uint32_t height, uint32_t dimension, uint32_t zero_dwords, uint32_t sampled_dwords);

void FetchDimensionMismatch(const DrawContext& draw, uint32_t slot, uint32_t base, uint32_t format,
                            uint32_t fetch_dimension, uint32_t shader_dimension, const char* reason);
void FetchFormatMismatch(const DrawContext& draw, uint32_t slot, uint32_t base, uint32_t fetch_format,
                         uint32_t resolve_kind, uint32_t resolve_format, uint32_t width, uint32_t height,
                         const char* reason);
void StaleResolveFallback(const DrawContext& draw, uint32_t slot, uint32_t fetch_base, uint32_t fetch_format,
                          uint32_t width, uint32_t height, uint32_t resolve_base, uint64_t resolve_seq,
                          const char* reason);

}  // namespace wwe2k14::d3d::glitch
