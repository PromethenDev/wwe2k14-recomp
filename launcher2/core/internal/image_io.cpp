// Image loading for the Creation Suite "Images" tab (PNG/JPG/BMP/TGA/GIF through stb_image) and a small PNG writer.
#include "internal/paint_tool.h"

#include "internal/util.h"

#define MINIZ_HEADER_FILE_ONLY
#include <miniz.h>
#undef MINIZ_HEADER_FILE_ONLY

#include <cstring>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wcast-qual"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#endif
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_PSD
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_NO_STDIO
#include "stb/stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace wwe2k14::launcher::internal {

namespace {

bool LoadImageFitted(const std::filesystem::path& file, int target_size, std::vector<uint8_t>* rgba,
                     std::string* error) {
  auto fail = [&](const char* m) { if (error) *error = m; return false; };
  std::vector<uint8_t> bytes;
  if (!ReadBinaryFile(file, bytes) || bytes.empty()) return fail("The picture could not be read.");
  if (bytes.size() > 64u * 1024 * 1024) return fail("The picture file is too large (over 64 MB).");
  int w = 0, h = 0, comp = 0;
  if (!stbi_info_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &comp))
    return fail("This is not a picture the launcher can read. Use a PNG or JPG.");
  if (w < 1 || h < 1 || w > 16384 || h > 16384) return fail("The picture size is not supported (up to 16384 x 16384).");
  unsigned char* pixels = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &comp, 4);
  if (!pixels) return fail("This picture could not be decoded. Use a PNG or JPG.");
  if (target_size == 128) *rgba = ExpandRgba128To256(FitRgbaTo128(pixels, w, h));
  else *rgba = FitRgbaTo256(pixels, w, h);
  stbi_image_free(pixels);
  return true;
}

}  // namespace

bool LoadImageFitted256(const std::filesystem::path& file, std::vector<uint8_t>* rgba, std::string* error) {
  return LoadImageFitted(file, 256, rgba, error);
}

bool LoadImageFitted128(const std::filesystem::path& file, std::vector<uint8_t>* rgba, std::string* error) {
  return LoadImageFitted(file, 128, rgba, error);
}

bool WritePngRgba(const std::filesystem::path& file, const uint8_t* rgba, int width, int height) {
  std::vector<uint8_t> raw;
  raw.reserve(size_t(height) * (size_t(width) * 4 + 1));
  for (int y = 0; y < height; ++y) {
    raw.push_back(0);
    raw.insert(raw.end(), rgba + size_t(y) * width * 4, rgba + size_t(y + 1) * width * 4);
  }
  mz_ulong cap = mz_compressBound(static_cast<mz_ulong>(raw.size()));
  std::vector<uint8_t> z(cap);
  mz_ulong zs = cap;
  if (mz_compress2(z.data(), &zs, raw.data(), static_cast<mz_ulong>(raw.size()), 6) != MZ_OK) return false;
  z.resize(zs);
  std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  auto chunk = [&](const char* type, const std::vector<uint8_t>& data) {
    auto be = [&](uint32_t v) { for (int i = 3; i >= 0; --i) out.push_back(uint8_t(v >> (8 * i))); };
    be(static_cast<uint32_t>(data.size()));
    const size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    be(static_cast<uint32_t>(mz_crc32(MZ_CRC32_INIT, out.data() + start, out.size() - start)));
  };
  std::vector<uint8_t> ihdr(13, 0);
  for (int i = 0; i < 4; ++i) { ihdr[i] = uint8_t(uint32_t(width) >> (24 - 8 * i)); ihdr[4 + i] = uint8_t(uint32_t(height) >> (24 - 8 * i)); }
  ihdr[8] = 8; ihdr[9] = 6;
  chunk("IHDR", ihdr);
  chunk("IDAT", z);
  chunk("IEND", {});
  return WriteBinaryFile(file, out);
}

}  // namespace wwe2k14::launcher::internal
