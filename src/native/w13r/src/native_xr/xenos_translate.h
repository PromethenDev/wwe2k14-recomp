// WWE 2K15 native renderer: Xbox 360 shader container -> HLSL (XenosRecomp, see
// xenos_translate.cpp). `prelude` is XenosRecomp's shader_common.h text.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

bool Wwe13TranslateShader(const uint8_t *container, std::string_view prelude, std::string &hlsl,
                            bool &pixel, uint32_t &spec_constants_mask);
