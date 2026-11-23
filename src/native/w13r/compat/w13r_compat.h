// w13r_compat.h - lets the WWE '13 v1.6 renderer (written for ReXGlue SDK 0.10) build on 2K14's SDK 0.7.3.
// Force-included before every w13r source. Decision 0008 (owner 2026-11-21).
#pragma once
#include <cstring>
#include <string_view>
#include <rex/types.h>
#include <rex/ppc/types.h>
namespace rex {
template <typename T> using MappedPtr = PPCPointer<T>;  // SDK 0.10 renamed PPCPointer to MappedPtr
}  // namespace rex
#include "w13r_globals.inc"
