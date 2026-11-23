// Creation Suite "Images": custom logos in the Paint Tool save (see internal/paint_tool.h for the format proof).
#include "launcher_core.h"

#include "internal/paint_tool.h"
#include "internal/util.h"

#include <chrono>
#include <ctime>
#include <random>
#include <system_error>

namespace wwe2k14::launcher {
namespace {

constexpr const char* kAccount = "B13EBABEBABEBABE";
constexpr const char* kTitle = "545408B2";
constexpr const char* kPackage = "00PaintTool.pt";

fs::path PayloadPath(const Paths& paths) {
  return paths.userdata_dir / kAccount / kTitle / "00000001" / kPackage / "SaveData.Dat";
}
fs::path HeaderPath(const Paths& paths) {
  return paths.userdata_dir / kAccount / kTitle / "Headers" / "00000001" / (std::string(kPackage) + ".header");
}

bool IsPlainFile(const fs::path& p) {
  std::error_code e;
  const auto st = fs::symlink_status(p, e);
  return !e && fs::is_regular_file(st) && !fs::is_symlink(st);
}

// Reads the current plain file. exists=false when there is no Paint Tool save yet.
bool ReadPlain(const Paths& paths, std::vector<uint8_t>* plain, bool* exists) {
  const fs::path file = PayloadPath(paths);
  std::error_code e;
  *exists = fs::exists(fs::symlink_status(file, e));
  if (!*exists) { *plain = internal::PtMakeEmpty(); return true; }
  if (!IsPlainFile(file)) return false;
  std::vector<uint8_t> bytes;
  if (!internal::ReadBinaryFile(file, bytes)) return false;
  return internal::PtUnpackFile(bytes, plain) && internal::PtIsValid(*plain);
}

Result WriteAtomic(const fs::path& target, const std::vector<uint8_t>& bytes) {
  std::error_code e;
  fs::create_directories(target.parent_path(), e);
  if (e) return Result::Fail("The save folder could not be prepared; no changes were made.");
  fs::path staged = target;
  staged += ".new-image";
  if (!internal::WriteBinaryFile(staged, bytes)) return Result::Fail("The save file could not be written; nothing was changed.");
  std::vector<uint8_t> check;
  if (!internal::ReadBinaryFile(staged, check) || check != bytes) {
    fs::remove(staged, e);
    return Result::Fail("The save file could not be verified; nothing was changed.");
  }
  fs::rename(staged, target, e);  // replaces an existing file atomically on both platforms
  if (e) {
    std::error_code ignore;
    fs::remove(staged, ignore);
    return Result::Fail("The save file could not be replaced; nothing was changed.");
  }
  return Result::Ok();
}

bool HasAnySaveFile(const Paths& paths) {
  std::error_code e;
  const fs::path account = paths.userdata_dir / kAccount;
  if (!fs::is_directory(account, e)) return false;
  for (fs::recursive_directory_iterator it(account, fs::directory_options::skip_permission_denied, e), end; !e && it != end; it.increment(e)) {
    std::error_code ignore;
    if (it->is_regular_file(ignore)) return true;
  }
  return false;
}

Result Commit(const Paths& paths, const std::vector<uint8_t>& plain, bool had_file) {
  std::vector<uint8_t> packed;
  if (!internal::PtPackFile(plain, &packed)) return Result::Fail("The images could not be packed; no changes were made.");
  BackupInfo backup;
  (void)had_file;
  if (HasAnySaveFile(paths)) {  // a full save backup before every change (nothing to back up on a brand-new install)
    const Result b = BackupSaves(paths, false, &backup);
    if (!b.ok) return b;
  }
  Result r = WriteAtomic(PayloadPath(paths), packed);
  if (!r.ok) return r;
  if (!IsPlainFile(HeaderPath(paths))) {
    r = WriteAtomic(HeaderPath(paths), internal::PtMakeHeader("01.CUSTOM LOGOS", kPackage));
    if (!r.ok) return r;
  }
  return Result::Ok();
}

internal::PtStamp CurrentStamp() {
  internal::PtStamp stamp;
  const std::time_t now = std::time(nullptr);
  std::tm local{};
#ifdef _WIN32
  localtime_s(&local, &now);
#else
  localtime_r(&now, &local);
#endif
  stamp.year = static_cast<uint16_t>(local.tm_year + 1900);
  stamp.month = static_cast<uint8_t>(local.tm_mon + 1);
  stamp.day = static_cast<uint8_t>(local.tm_mday);
  stamp.hour = static_cast<uint8_t>(local.tm_hour);
  stamp.minute = static_cast<uint8_t>(local.tm_min);
  stamp.second = static_cast<uint8_t>(local.tm_sec);
  stamp.weekday = static_cast<uint8_t>(local.tm_wday);
  return stamp;
}

Result StoreLogoPixels(const Paths& paths, const std::vector<uint8_t>& rgba256,
                       std::optional<LogoImageSize> requested_size, std::optional<uint32_t> target_slot,
                       uint32_t* saved_slot) {
  if (rgba256.size() != 256u * 256u * 4u)
    return Result::Fail("The drawing must be a 128 x 128 or 256 x 256 RGBA canvas. No changes were made.");
  if (requested_size && *requested_size != LogoImageSize::k128 && *requested_size != LogoImageSize::k256)
    return Result::Fail("Choose a supported logo size. No changes were made.");

  std::vector<uint8_t> plain;
  bool exists = false;
  if (!ReadPlain(paths, &plain, &exists))
    return Result::Fail("The Paint Tool save was not recognized, so it was left alone. No changes were made.");

  uint32_t slot = kLogoSlotCount;
  if (target_slot) {
    slot = *target_slot;
    if (!exists || slot >= kLogoSlotCount || !internal::PtSlotUsed(plain, slot))
      return Result::Fail("That logo slot is no longer available. Reload the logos and try again.");
    if (internal::PtSlotBuiltIn(plain, slot))
      return Result::Fail("The game's own logos cannot be edited.");
  } else {
    for (uint32_t k = 0; k < kLogoSlotCount; ++k) {
      if (!internal::PtSlotUsed(plain, k)) { slot = k; break; }
    }
    if (slot == kLogoSlotCount) return Result::Fail("All 20 image slots are full. Remove one first.");
  }

  LogoImageSize size = requested_size.value_or(LogoImageSize::k256);
  uint32_t token = 0;
  uint32_t game_stamp = 0x003E5C12;
  if (target_slot) {
    const uint32_t width = internal::PtSlotWidth(plain, slot);
    const uint32_t height = internal::PtSlotHeight(plain, slot);
    if (width != height || (width != 128 && width != 256))
      return Result::Fail("That logo has an unsupported size and was left alone.");
    size = width == 128 ? LogoImageSize::k128 : LogoImageSize::k256;
    token = internal::PtSlotToken(plain, slot);
    game_stamp = internal::PtSlotGameStamp(plain, slot);
  } else {
    std::random_device rd;
    token = rd();
  }

  const internal::PtStamp stamp = CurrentStamp();
  const int dimensions = static_cast<int>(size);
  internal::PtWriteSlot(&plain, slot, rgba256, token, stamp, game_stamp, dimensions, dimensions);
  internal::PtStampAll(&plain, stamp);
  const Result result = Commit(paths, plain, exists);
  if (result.ok && saved_slot) *saved_slot = slot;
  return result;
}

}  // namespace

std::optional<LogoList> ListLogoImages(const Paths& paths) {
  std::vector<uint8_t> plain;
  bool exists = false;
  if (!ReadPlain(paths, &plain, &exists)) return std::nullopt;
  LogoList list;
  list.file_present = exists;
  for (uint32_t k = 0; k < kLogoSlotCount; ++k) {
    if (!internal::PtSlotUsed(plain, k)) continue;
    LogoImage image;
    image.slot_index = k;
    image.id = internal::PtSlotId(plain, k);
    image.built_in = internal::PtSlotBuiltIn(plain, k);
    image.width = internal::PtSlotWidth(plain, k);
    image.height = internal::PtSlotHeight(plain, k);
    image.rgba = internal::PtDecodeCanvas(plain, k);
    list.logos.push_back(std::move(image));
  }
  return list;
}

Result AddLogoImage(const Paths& paths, const fs::path& picture, uint32_t* slot) {
  return AddLogoImage(paths, picture, LogoImageSize::k256, slot);
}

Result AddLogoImage(const Paths& paths, const fs::path& picture, LogoImageSize size, uint32_t* slot) {
  try {
    if (size != LogoImageSize::k128 && size != LogoImageSize::k256)
      return Result::Fail("Choose a supported logo size. No changes were made.");
    std::vector<uint8_t> rgba;
    std::string error;
    const bool is128 = size == LogoImageSize::k128;
    const bool loaded = is128 ? internal::LoadImageFitted128(picture, &rgba, &error)
                              : internal::LoadImageFitted256(picture, &rgba, &error);
    if (!loaded) return Result::Fail(error + " No changes were made.");
    return StoreLogoPixels(paths, rgba, size, std::nullopt, slot);
  } catch (...) {
    return Result::Fail("The picture could not be added safely; no changes were made.");
  }
}

Result AddLogoImage(const Paths& paths, const std::vector<uint8_t>& rgba256, LogoImageSize size, uint32_t* slot) {
  try {
    return StoreLogoPixels(paths, rgba256, size, std::nullopt, slot);
  } catch (...) {
    return Result::Fail("The drawing could not be added safely; no changes were made.");
  }
}

Result EditLogoImage(const Paths& paths, uint32_t slot_index, const std::vector<uint8_t>& rgba256) {
  try {
    return StoreLogoPixels(paths, rgba256, std::nullopt, slot_index, nullptr);
  } catch (...) {
    return Result::Fail("The drawing could not be saved safely; no changes were made.");
  }
}

Result RemoveLogoImage(const Paths& paths, uint32_t slot_index) {
  try {
    std::vector<uint8_t> plain;
    bool exists = false;
    if (!ReadPlain(paths, &plain, &exists) || !exists)
      return Result::Fail("The Paint Tool save was not recognized, so it was left alone. No changes were made.");
    if (slot_index >= kLogoSlotCount || !internal::PtSlotUsed(plain, slot_index))
      return Result::Fail("That image slot is already empty.");
    if (internal::PtSlotBuiltIn(plain, slot_index)) return Result::Fail("That is one of the game's own logos and cannot be removed.");
    internal::PtClearSlot(&plain, slot_index);
    return Commit(paths, plain, true);
  } catch (...) {
    return Result::Fail("The image could not be removed safely; no changes were made.");
  }
}

}  // namespace wwe2k14::launcher
