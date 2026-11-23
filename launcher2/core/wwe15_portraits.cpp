#include "wwe15_portraits.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace wwe2k14::launcher {
namespace {

using Bytes = WWE15PortraitBytes;

bool Fail(std::string* error, const std::string& message) {
  if (error) *error = message;
  return false;
}
void PutLe32(Bytes* b, size_t at, uint32_t v) {
  for (int i = 0; i < 4; ++i) (*b)[at + i] = uint8_t(v >> (8 * i));
}
uint32_t Le32(const Bytes& b, size_t at) {
  return uint32_t(b[at]) | (uint32_t(b[at + 1]) << 8) | (uint32_t(b[at + 2]) << 16) | (uint32_t(b[at + 3]) << 24);
}

// 1-D area weights of a box filter mapping `in` samples to `out` samples.
std::vector<std::vector<std::pair<int, double>>> BoxWeights(int in, int out) {
  std::vector<std::vector<std::pair<int, double>>> w(out);
  const double scale = double(in) / double(out);
  for (int o = 0; o < out; ++o) {
    const double a = o * scale, b = (o + 1) * scale;
    double total = 0;
    for (int i = int(std::floor(a)); i < int(std::ceil(b)) && i < in; ++i) {
      const double overlap = std::min(b, double(i + 1)) - std::max(a, double(i));
      if (overlap > 0) { w[o].emplace_back(i, overlap); total += overlap; }
    }
    for (auto& [i, x] : w[o]) x /= total;
  }
  return w;
}

uint16_t To565(const std::array<double, 3>& c) {
  const int r = std::clamp(int(std::lround(c[0] * 31.0 / 255.0)), 0, 31);
  const int g = std::clamp(int(std::lround(c[1] * 63.0 / 255.0)), 0, 63);
  const int b = std::clamp(int(std::lround(c[2] * 31.0 / 255.0)), 0, 31);
  return uint16_t((r << 11) | (g << 5) | b);
}
std::array<int, 3> From565(uint16_t v) {
  const int r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
  return {(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)};
}

void EncodeBlock(const uint8_t px[16][4], uint8_t out[16]) {
  // alpha: 8-value mode between the block's min and max
  int amin = 255, amax = 0;
  for (int i = 0; i < 16; ++i) { amin = std::min<int>(amin, px[i][3]); amax = std::max<int>(amax, px[i][3]); }
  out[0] = uint8_t(amax);
  out[1] = uint8_t(amin);
  std::array<int, 8> pal{amax, amin};
  for (int i = 1; i < 7; ++i) pal[1 + i] = ((7 - i) * amax + i * amin + 3) / 7;
  uint64_t abits = 0;
  if (amax != amin)
    for (int i = 0; i < 16; ++i) {
      int best = 0, err = 1 << 30;
      for (int k = 0; k < 8; ++k) {
        const int e = std::abs(pal[k] - px[i][3]);
        if (e < err) { err = e; best = k; }
      }
      abits |= uint64_t(best) << (3 * i);
    }
  for (int i = 0; i < 6; ++i) out[2 + i] = uint8_t(abits >> (8 * i));
  // colour: endpoints along the principal axis, inset by 1/16 of the range
  double mean[3] = {0, 0, 0};
  for (int i = 0; i < 16; ++i) for (int c = 0; c < 3; ++c) mean[c] += px[i][c] / 16.0;
  double cov[6] = {0, 0, 0, 0, 0, 0};
  for (int i = 0; i < 16; ++i) {
    const double r = px[i][0] - mean[0], g = px[i][1] - mean[1], b = px[i][2] - mean[2];
    cov[0] += r * r; cov[1] += r * g; cov[2] += r * b; cov[3] += g * g; cov[4] += g * b; cov[5] += b * b;
  }
  double axis[3] = {0.97, 0.21, 0.11};
  for (int it = 0; it < 8; ++it) {
    const double x = cov[0] * axis[0] + cov[1] * axis[1] + cov[2] * axis[2];
    const double y = cov[1] * axis[0] + cov[3] * axis[1] + cov[4] * axis[2];
    const double z = cov[2] * axis[0] + cov[4] * axis[1] + cov[5] * axis[2];
    const double n = std::sqrt(x * x + y * y + z * z);
    if (n < 1e-9) break;
    axis[0] = x / n; axis[1] = y / n; axis[2] = z / n;
  }
  double lo = 1e9, hi = -1e9;
  for (int i = 0; i < 16; ++i) {
    const double t = (px[i][0] - mean[0]) * axis[0] + (px[i][1] - mean[1]) * axis[1] + (px[i][2] - mean[2]) * axis[2];
    lo = std::min(lo, t); hi = std::max(hi, t);
  }
  const double inset = (hi - lo) / 16.0;
  lo += inset; hi -= inset;
  std::array<double, 3> e0, e1;
  for (int c = 0; c < 3; ++c) { e0[c] = mean[c] + axis[c] * hi; e1[c] = mean[c] + axis[c] * lo; }
  uint16_t c0 = To565(e0), c1 = To565(e1);
  if (c0 < c1) std::swap(c0, c1);
  uint32_t cbits = 0;
  if (c0 != c1) {
    const auto a = From565(c0), b = From565(c1);
    std::array<std::array<int, 3>, 4> cp;
    for (int c = 0; c < 3; ++c) {
      cp[0][c] = a[c]; cp[1][c] = b[c];
      cp[2][c] = (2 * a[c] + b[c]) / 3; cp[3][c] = (a[c] + 2 * b[c]) / 3;
    }
    for (int i = 0; i < 16; ++i) {
      int best = 0, err = 1 << 30;
      for (int k = 0; k < 4; ++k) {
        int e = 0;
        for (int c = 0; c < 3; ++c) { const int d = cp[k][c] - px[i][c]; e += d * d; }
        if (e < err) { err = e; best = k; }
      }
      cbits |= uint32_t(best) << (2 * i);
    }
  }
  out[8] = uint8_t(c0); out[9] = uint8_t(c0 >> 8); out[10] = uint8_t(c1); out[11] = uint8_t(c1 >> 8);
  for (int i = 0; i < 4; ++i) out[12 + i] = uint8_t(cbits >> (8 * i));
}

Bytes EncodeLevel(const Bytes& rgba, int w, int h) {
  const int bw = std::max(1, (w + 3) / 4), bh = std::max(1, (h + 3) / 4);
  Bytes out(size_t(bw) * bh * 16);
  for (int by = 0; by < bh; ++by)
    for (int bx = 0; bx < bw; ++bx) {
      uint8_t px[16][4];
      for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
          const int sx = std::min(bx * 4 + x, w - 1), sy = std::min(by * 4 + y, h - 1);
          std::memcpy(px[y * 4 + x], &rgba[(size_t(sy) * w + sx) * 4], 4);
        }
      EncodeBlock(px, &out[(size_t(by) * bw + bx) * 16]);
    }
  return out;
}

}  // namespace

bool DecodeWWE15PortraitPixels(const WWE15PortraitBytes& raw, int* side, WWE15PortraitBytes* rgba,
                               std::string* error) {
  if (raw.size() < 44) return Fail(error, "the portrait is " + std::to_string(raw.size()) + " bytes; at least 44 expected");
  const int s = ((raw[12] << 8) | raw[13]) * 256;
  if (s != 256 && s != 512) return Fail(error, "the portrait side is " + std::to_string(s) + "; 256 or 512 expected");
  const size_t need = 44 + size_t(s) * s * 4;
  if (raw.size() < need)
    return Fail(error, "the " + std::to_string(s) + "-pixel portrait is " + std::to_string(raw.size()) + " bytes; " + std::to_string(need) + " expected");
  rgba->assign(size_t(s) * s * 4, 0);
  for (int y = 0; y < s; ++y)
    for (int x = 0; x < s; ++x) {
      const uint8_t* p = &raw[44 + (size_t(s - 1 - y) * s + x) * 4];  // flip: stored bottom row first
      uint8_t* q = &(*rgba)[(size_t(y) * s + x) * 4];
      q[0] = p[0]; q[1] = p[3]; q[2] = p[2]; q[3] = p[1];
    }
  *side = s;
  return true;
}

WWE15PortraitBytes ResizeWWE15Box(const WWE15PortraitBytes& rgba, int width, int height, int new_width,
                                  int new_height) {
  if (width == new_width && height == new_height) return rgba;
  const auto wx = BoxWeights(width, new_width), wy = BoxWeights(height, new_height);
  Bytes out(size_t(new_width) * new_height * 4);
  for (int y = 0; y < new_height; ++y)
    for (int x = 0; x < new_width; ++x)
      for (int c = 0; c < 4; ++c) {
        double v = 0;
        for (const auto& [sy, fy] : wy[y])
          for (const auto& [sx, fx] : wx[x]) v += fy * fx * rgba[(size_t(sy) * width + sx) * 4 + c];
        out[(size_t(y) * new_width + x) * 4 + c] = uint8_t(std::clamp(int(std::floor(v + 0.5)), 0, 255));
      }
  return out;
}

WWE15PortraitBytes EncodeWWE15Dxt5Dds(const WWE15PortraitBytes& rgba, int width, int height) {
  int mips = 1;
  while ((std::max(width, height) >> mips) > 0) ++mips;  // log2(max) + 1
  Bytes out(128, 0);
  std::memcpy(out.data(), "DDS ", 4);
  PutLe32(&out, 4, 124);
  PutLe32(&out, 8, 0xA1007);  // caps | height | width | pixel format | mip count | linear size
  PutLe32(&out, 12, uint32_t(height));
  PutLe32(&out, 16, uint32_t(width));
  PutLe32(&out, 20, uint32_t(std::max(1, (width + 3) / 4) * std::max(1, (height + 3) / 4) * 16));
  PutLe32(&out, 28, uint32_t(mips));
  PutLe32(&out, 76, 32);
  PutLe32(&out, 80, 4);
  std::memcpy(out.data() + 84, "DXT5", 4);
  PutLe32(&out, 88, 32);
  PutLe32(&out, 108, 0x401008);  // texture | complex | mipmap
  for (int m = 0; m < mips; ++m) {
    const int w = std::max(1, width >> m), h = std::max(1, height >> m);
    const Bytes level = EncodeLevel(m == 0 ? rgba : ResizeWWE15Box(rgba, width, height, w, h), w, h);
    out.insert(out.end(), level.begin(), level.end());
  }
  return out;
}

bool DecodeWWE15Dxt5Dds(const WWE15PortraitBytes& dds, int* width, int* height, int* mips,
                        WWE15PortraitBytes* rgba, std::string* error) {
  if (dds.size() < 128 || std::memcmp(dds.data(), "DDS ", 4) != 0 || std::memcmp(dds.data() + 84, "DXT5", 4) != 0)
    return Fail(error, "not a DXT5 DDS");
  const int h = int(Le32(dds, 12)), w = int(Le32(dds, 16));
  const int bw = std::max(1, (w + 3) / 4), bh = std::max(1, (h + 3) / 4);
  if (dds.size() < 128 + size_t(bw) * bh * 16) return Fail(error, "the DDS is truncated");
  *width = w; *height = h; *mips = int(Le32(dds, 28));
  rgba->assign(size_t(w) * h * 4, 0);
  for (int by = 0; by < bh; ++by)
    for (int bx = 0; bx < bw; ++bx) {
      const uint8_t* b = &dds[128 + (size_t(by) * bw + bx) * 16];
      const int a0 = b[0], a1 = b[1];
      std::array<int, 8> ap{a0, a1};
      if (a0 > a1) for (int i = 1; i < 7; ++i) ap[1 + i] = ((7 - i) * a0 + i * a1) / 7;
      else { for (int i = 1; i < 5; ++i) ap[1 + i] = ((5 - i) * a0 + i * a1) / 5; ap[6] = 0; ap[7] = 255; }
      uint64_t abits = 0;
      for (int i = 0; i < 6; ++i) abits |= uint64_t(b[2 + i]) << (8 * i);
      const uint16_t c0 = uint16_t(b[8] | (b[9] << 8)), c1 = uint16_t(b[10] | (b[11] << 8));
      const auto a = From565(c0), c = From565(c1);
      std::array<std::array<int, 3>, 4> cp;
      for (int k = 0; k < 3; ++k) { cp[0][k] = a[k]; cp[1][k] = c[k]; cp[2][k] = (2 * a[k] + c[k]) / 3; cp[3][k] = (a[k] + 2 * c[k]) / 3; }
      const uint32_t cbits = uint32_t(b[12]) | (uint32_t(b[13]) << 8) | (uint32_t(b[14]) << 16) | (uint32_t(b[15]) << 24);
      for (int i = 0; i < 16; ++i) {
        const int x = bx * 4 + i % 4, y = by * 4 + i / 4;
        if (x >= w || y >= h) continue;
        uint8_t* q = &(*rgba)[(size_t(y) * w + x) * 4];
        const auto& col = cp[(cbits >> (2 * i)) & 3];
        q[0] = uint8_t(col[0]); q[1] = uint8_t(col[1]); q[2] = uint8_t(col[2]);
        q[3] = uint8_t(ap[(abits >> (3 * i)) & 7]);
      }
    }
  return true;
}

bool ConvertWWE15Portrait(const WWE15PortraitBytes& raw, int want_width, int want_height,
                          WWE15PortraitBytes* dds, std::string* error) {
  int side = 0;
  Bytes rgba;
  if (!DecodeWWE15PortraitPixels(raw, &side, &rgba, error)) return false;
  *dds = EncodeWWE15Dxt5Dds(ResizeWWE15Box(rgba, side, side, want_width, want_height), want_width, want_height);
  return true;
}

}  // namespace wwe2k14::launcher
