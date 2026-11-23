#ifdef NDEBUG
#undef NDEBUG
#endif

#include "launcher_core.h"

#include "internal/paint_tool.h"
#include "internal/util.h"

#include <cassert>
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;
using namespace wwe2k14::launcher;

int main() {
  const fs::path root = fs::temp_directory_path() / "wwe2k14-paint-images-test";
  std::error_code e;
  fs::remove_all(root, e);
  Paths paths;
  paths.exe_dir = root;
  paths.userdata_dir = root / "userdata";
  paths.backups_dir = root / "backups";
  paths.logs_dir = root / "logs";
  fs::create_directories(paths.userdata_dir / "B13EBABEBABEBABE" / "545408B2" / "00000001");
  // BackupSaves wants at least one save file to archive.
  assert(internal::WriteBinaryFile(paths.userdata_dir / "B13EBABEBABEBABE" / "545408B2" / "00000001" / "x.bin",
                                   std::vector<uint8_t>{1, 2, 3}));

  const auto none = ListLogoImages(paths);
  assert(none && !none->file_present && none->logos.empty());

  // A 300x100 PNG with transparency.
  std::vector<uint8_t> px(300 * 100 * 4);
  for (int y = 0; y < 100; ++y)
    for (int x = 0; x < 300; ++x) {
      uint8_t* p = &px[(y * 300 + x) * 4];
      p[0] = uint8_t(x); p[1] = uint8_t(y * 2); p[2] = 90; p[3] = x < 20 ? 0 : 255;
    }
  const fs::path png = root / "test.png";
  assert(internal::WritePngRgba(png, px.data(), 300, 100));

  uint32_t slot = 99;
  Result r = AddLogoImage(paths, png, &slot);
  assert(r.ok && slot == 0);
  r = AddLogoImage(paths, png, &slot);
  assert(r.ok && slot == 1);
  auto list = ListLogoImages(paths);
  assert(list && list->file_present && list->logos.size() == 2);
  assert(list->logos[0].slot_index == 0 && list->logos[1].slot_index == 1);
  assert(list->logos[0].width == 256 && list->logos[0].height == 256);
  // 300x100 fitted into 256: height 85/86, letterboxed; row 0 is transparent, centre opaque.
  const auto& img = list->logos[0].rgba;
  assert(img[3] == 0 && img[(128 * 256 + 128) * 4 + 3] == 255);
  assert(fs::exists(paths.userdata_dir / "B13EBABEBABEBABE/545408B2/Headers/00000001/00PaintTool.pt.header"));

  // A per-import 128x128 choice stores a compact 128-pixel raster and lists its thumbnail correctly.
  r = AddLogoImage(paths, png, LogoImageSize::k128, &slot);
  assert(r.ok && slot == 2);
  list = ListLogoImages(paths);
  assert(list && list->logos.size() == 3 && list->logos[2].slot_index == 2);
  assert(list->logos[2].width == 128 && list->logos[2].height == 128);
  assert(list->logos[2].rgba.size() == 256 * 256 * 4);
  assert(list->logos[2].rgba[3] == 0 && list->logos[2].rgba[(128 * 256 + 128) * 4 + 3] == 255);
  std::vector<uint8_t> edited128(256 * 256 * 4);
  for (size_t i = 0; i < 256 * 256; ++i) {
    edited128[i * 4] = 30; edited128[i * 4 + 1] = 90; edited128[i * 4 + 2] = 210; edited128[i * 4 + 3] = 255;
  }
  assert(EditLogoImage(paths, 2, edited128).ok);
  list = ListLogoImages(paths);
  assert(list && list->logos[2].width == 128 && list->logos[2].height == 128);
  assert(list->logos[2].rgba == edited128);
  assert(RemoveLogoImage(paths, 2).ok);
  list = ListLogoImages(paths);
  assert(list && list->logos.size() == 2 && list->logos[0].slot_index == 0 && list->logos[1].slot_index == 1);

  r = RemoveLogoImage(paths, 0);
  assert(r.ok);
  list = ListLogoImages(paths);
  assert(list && list->logos.size() == 1 && list->logos[0].slot_index == 1);
  r = AddLogoImage(paths, png, &slot);
  assert(r.ok && slot == 0);  // freed slot is reused
  for (int i = 0; i < 18; ++i) assert(AddLogoImage(paths, png, &slot).ok);
  r = AddLogoImage(paths, png, &slot);
  assert(!r.ok && r.error.find("20") != std::string::npos);
  assert(!RemoveLogoImage(paths, 7).ok == false);

  // New editor canvases use the Add path, while edits keep the target slot and its native resolution.
  std::vector<uint8_t> drawn(256 * 256 * 4);
  for (size_t i = 0; i < 256 * 256; ++i) {
    drawn[i * 4] = 12; drawn[i * 4 + 1] = 180; drawn[i * 4 + 2] = 70; drawn[i * 4 + 3] = 255;
  }
  assert(AddLogoImage(paths, drawn, LogoImageSize::k256, &slot).ok && slot == 7);
  std::vector<uint8_t> redrawn = drawn;
  for (size_t i = 0; i < 256 * 256; ++i) {
    redrawn[i * 4] = 210; redrawn[i * 4 + 1] = 40; redrawn[i * 4 + 2] = 25;
  }
  assert(EditLogoImage(paths, 7, redrawn).ok);
  list = ListLogoImages(paths);
  bool found_redrawn = false;
  if (list) for (const LogoImage& logo : list->logos) {
    if (logo.slot_index == 7) found_redrawn = logo.width == 256 && logo.height == 256 && logo.rgba == redrawn;
  }
  assert(found_redrawn);
  assert(!AddLogoImage(paths, std::vector<uint8_t>{1, 2, 3}, LogoImageSize::k256).ok);

  // Not a picture / unknown format: refused, nothing changed.
  const fs::path junk = root / "junk.png";
  assert(internal::WriteBinaryFile(junk, std::vector<uint8_t>{'n', 'o', 'p', 'e'}));
  assert(!AddLogoImage(paths, junk).ok);

  // A damaged Paint Tool save is left alone.
  const fs::path payload = paths.userdata_dir / "B13EBABEBABEBABE/545408B2/00000001/00PaintTool.pt/SaveData.Dat";
  assert(internal::WriteBinaryFile(payload, std::vector<uint8_t>{'Z', 'L', 'I', 'B', 1, 2, 3}));
  assert(!ListLogoImages(paths).has_value());
  assert(!AddLogoImage(paths, png).ok);
  std::cout << "paint_images_test ok\n";
  fs::remove_all(root, e);
  return 0;
}
