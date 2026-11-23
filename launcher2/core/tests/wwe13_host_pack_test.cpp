// Finding WWE 2K14 Add-On 2 (the import's host offer pack) by its catalog, not its folder name (#103).
#include "../wwe13_pipeline.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace wwe2k14::launcher;
namespace fs = std::filesystem;

namespace {

constexpr const char* kStandard = "745BEE31BC19100A3EE21606E089EC25F739099C54";
constexpr const char* kOutput = "045BEE31BC19100A3EE21606E089EC25F739099C54";

void Check(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

// Synthetic catalog: "DLCC", pack id at +8, offer count at +12, 0x40-byte offers from +0x1c with the wrestler id at +16.
std::vector<uint8_t> Catalog(uint16_t pack_id, const std::vector<uint16_t>& offer_ids) {
  std::vector<uint8_t> bytes(0x1c + offer_ids.size() * 0x40, 0);
  bytes[0] = 'D'; bytes[1] = 'L'; bytes[2] = 'C'; bytes[3] = 'C';
  bytes[8] = static_cast<uint8_t>(pack_id); bytes[9] = static_cast<uint8_t>(pack_id >> 8);
  bytes[12] = static_cast<uint8_t>(offer_ids.size()); bytes[13] = static_cast<uint8_t>(offer_ids.size() >> 8);
  for (size_t i = 0; i < offer_ids.size(); ++i) {
    bytes[0x1c + i * 0x40 + 16] = static_cast<uint8_t>(offer_ids[i]);
    bytes[0x1c + i * 0x40 + 17] = static_cast<uint8_t>(offer_ids[i] >> 8);
  }
  return bytes;
}

void Pack(const fs::path& userdata, const std::string& name, const std::vector<uint8_t>& catalog) {
  const fs::path info = userdata / "0000000000000000" / "545408B2" / "00000002" / name / "info";
  fs::create_directories(info);
  std::ofstream(info / "catalog.dlc", std::ios::binary)
      .write(reinterpret_cast<const char*>(catalog.data()), static_cast<std::streamsize>(catalog.size()));
}

}  // namespace

int main() {
  try {
    const fs::path root = fs::path(WWE2K14_LAUNCHER_TEST_BINARY_DIR) / "wwe13-host-pack-test";
    fs::remove_all(root);
    const std::vector<uint8_t> host = Catalog(2, {254, 338, 105});
    const std::vector<uint8_t> other_add_on_2 = Catalog(2, {254, 2});  // the second, smaller "Add-On 2" offer pack
    const std::vector<uint8_t> content_pack = Catalog(3, {338});

    {  // No packs at all.
      const fs::path ud = root / "none";
      fs::create_directories(ud);
      Check(FindWWE2K14HostPack(ud).empty(), "empty userdata must find no host pack");
    }
    {  // Standard name.
      const fs::path ud = root / "standard";
      Pack(ud, "267E5576B180BEBEABD7BE8863B0F5D62D7D37DE54", other_add_on_2);
      Pack(ud, kStandard, host);
      Check(FindWWE2K14HostPack(ud) == kStandard, "standard host pack name");
    }
    {  // Added under another file name: found by contents; look-alikes skipped.
      const fs::path ud = root / "renamed";
      Pack(ud, "267E5576B180BEBEABD7BE8863B0F5D62D7D37DE54", other_add_on_2);
      Pack(ud, "A09DECBF32B71FF36D0CBE0AF56B1618CA47CEEB54", content_pack);
      Pack(ud, "WWE 2K14 Add-On 2", host);
      Check(FindWWE2K14HostPack(ud) == "WWE 2K14 Add-On 2", "renamed host pack must be found by its catalog");
    }
    {  // Only the import's own output and a half-installed copy: neither counts.
      const fs::path ud = root / "output-only";
      Pack(ud, kOutput, host);
      Pack(ud, "addon2.partial", host);
      Check(FindWWE2K14HostPack(ud).empty(), "the import output and .partial folders are not the host pack");
    }
    {  // Standard name present but not a host catalog: a correct renamed copy wins.
      const fs::path ud = root / "bad-standard";
      Pack(ud, kStandard, other_add_on_2);
      Pack(ud, "addon2", host);
      Check(FindWWE2K14HostPack(ud) == "addon2", "a non-host catalog under the standard name is skipped");
    }
    {  // #135 (Linux): an add-on copied with an upper-case INFO folder is still found (case-sensitive file names).
      const fs::path ud = root / "upper-case";
      Pack(ud, "WWE 2K14 Add-On 2", host);
      const fs::path base = ud / "0000000000000000" / "545408B2" / "00000002" / "WWE 2K14 Add-On 2";
      fs::rename(base / "info", base / "INFO");
      Check(FindWWE2K14HostPack(ud) == "WWE 2K14 Add-On 2", "an INFO/ folder (other letter case) must still be found");
    }
    {  // Truncated catalog is rejected.
      const fs::path ud = root / "truncated";
      std::vector<uint8_t> cut = host;
      cut.resize(0x1c + 0x40);
      Pack(ud, kStandard, cut);
      Check(FindWWE2K14HostPack(ud).empty(), "a truncated catalog is not the host pack");
    }
    {  // #118/#120 (2026-11-10): only the 4-offer pack 2 (267E5576..) - it hosts the import now.
      const fs::path ud = root / "small-only";
      Pack(ud, "267E5576B180BEBEABD7BE8863B0F5D62D7D37DE54", other_add_on_2);
      Pack(ud, "A09DECBF32B71FF36D0CBE0AF56B1618CA47CEEB54", content_pack);
      Check(FindWWE2K14HostPack(ud) == "267E5576B180BEBEABD7BE8863B0F5D62D7D37DE54", "the 4-offer pack 2 hosts when Add-On 2 is absent");
      Check(WWE13HostOutputPack(FindWWE2K14HostPack(ud)) == "067E5576B180BEBEABD7BE8863B0F5D62D7D37DE54", "its output name");
    }
    {  // Both versions: the standard 32-offer one still wins (unchanged behaviour for those players).
      const fs::path ud = root / "both";
      Pack(ud, "267E5576B180BEBEABD7BE8863B0F5D62D7D37DE54", other_add_on_2);
      Pack(ud, kStandard, host);
      Check(FindWWE2K14HostPack(ud) == kStandard, "standard Add-On 2 preferred over the 4-offer version");
      Check(WWE13HostOutputPack(kStandard) == kOutput, "standard output name unchanged");
    }
    {  // After an import from the 4-offer version, its output alone is not a host (no second import on top).
      const fs::path ud = root / "small-output-only";
      Pack(ud, "067E5576B180BEBEABD7BE8863B0F5D62D7D37DE54", other_add_on_2);
      Check(FindWWE2K14HostPack(ud).empty(), "the 4-offer import output is not a host");
    }
    {  // A host under another file name keeps the standard output name (#103 behaviour).
      Check(WWE13HostOutputPack("WWE 2K14 Add-On 2") == kOutput, "renamed host output name");
    }
    {  // Only content packs (3, 4, 5): no host.
      const fs::path ud = root / "content-only";
      Pack(ud, "A09DECBF32B71FF36D0CBE0AF56B1618CA47CEEB54", content_pack);
      Check(FindWWE2K14HostPack(ud).empty(), "content packs never host");
    }
    fs::remove_all(root);
    std::cout << "wwe13_host_pack_test: PASS\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "wwe13_host_pack_test: FAIL: " << error.what() << "\n";
    return 1;
  }
}
