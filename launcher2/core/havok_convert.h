#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace wwe2k14::launcher {

// Havok binary packfiles (32-bit big-endian, Xbox 360) in WWE '13 / WWE 2K14 character cloth leaves (PACH 9000/9002/9010/9012).
// WWE '13 writes hk_2011.3, WWE 2K14 reads hk_2012.2; several class layouts changed (owner 11-16: Ministry Undertaker's cape
// floated). The converter walks the source file's own embedded reflection (__types__: hkClass / hkClassMember) and writes every
// member at the template file's layout, matched by name, with the documented moves, defaults and value rules (gravity sign).
// The template is a stock WWE 2K14 packfile with the same layout rules (from the player's own game files); its __classnames__
// and __types__ sections are reused byte for byte, a new __data__ section is built with fresh fixups.

// "hk_2011.3.0-r1" etc.; empty when the bytes are not a packfile.
std::string HavokPackfileVersion(const std::vector<uint8_t>& packfile);

bool ConvertHavokPackfile(const std::vector<uint8_t>& source, const std::vector<uint8_t>& layout_template,
                          std::vector<uint8_t>* output, std::string* error);

}  // namespace wwe2k14::launcher
