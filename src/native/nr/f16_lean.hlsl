// WWE2K14_D3D_F16_LEAN: render the existing fixed16 LUT mapping into an UNORM target.
struct ResolveParams {
  uint w, h;
  uint src_bpp;
  uint bpp_log2;
  uint flags;
  uint endian;
  uint dx, dy;
  uint pitch_aligned;
  int base_offset;
  uint dst_size;
  uint x0, y0;
  uint kbase, kpitch32;
  uint select;
  uint ms;
  uint dst_start;
};

[[vk::push_constant]] ResolveParams P;
[[vk::binding(0, 0)]] Texture2D<float4> g_tex;
[[vk::binding(1, 0)]] StructuredBuffer<uint> g_lut;

float4 vs_main(uint id : SV_VertexID) : SV_Position {
  const float2 p = float2((id << 1) & 2, id & 2);
  return float4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0);
}

float4 ps_main(float4 pos : SV_Position) : SV_Target0 {
  uint lx = uint(pos.x) + P.x0;
  uint ly = uint(pos.y) + P.y0;
  if ((P.flags & 0x400u) != 0u) {
    lx = min(((2u * lx + 1u) * P.dx) >> 17, P.dy & 0xFFFFu);
    ly = min(((2u * ly + 1u) * P.dx) >> 17, P.dy >> 16);
  }
  const float4 f = g_tex.Load(int3(lx, ly, 0));
  const uint4 q = uint4(int4(round(f * 32767.0))) & 0xFFFFu;
  const uint h0 = g_lut[(q.x & 0x8000u) ? 0u : min(q.x, 1024u)] & 0xFFFFu;
  const uint h1 = g_lut[(q.y & 0x8000u) ? 0u : min(q.y, 1024u)] & 0xFFFFu;
  const uint h2 = P.src_bpp == 8 ? g_lut[(q.z & 0x8000u) ? 0u : min(q.z, 1024u)] & 0xFFFFu : 0u;
  const uint h3 = P.src_bpp == 8 ? g_lut[(q.w & 0x8000u) ? 0u : min(q.w, 1024u)] & 0xFFFFu : 0u;
  // Each float is h/65535. The paired bit-exact readback in WWE2K14_D3D_F16_LEAN=check validates
  // the attachment's UNORM conversion for every sampled conversion.
  return float4(float(h0) / 65535.0, float(h1) / 65535.0,
                float(h2) / 65535.0, float(h3) / 65535.0);
}
