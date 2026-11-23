#!/usr/bin/env bash
# Imported into WWE 2K14 native workspace (Track E, 2026-10-13) from
#   <wwe13-recomp-nr>/tools/nr/build-backend.sh
#   repo HEAD 19c8bb361a9b7d6cba65afffc628bda023da2e0c (file last changed in 480bbfe),
#   original sha256 f403bb0e37afbe42fe3f87f43c76c0fdb9537ad0fe7a94a79e67453baba8db75.
# Local changes: XXHASH_DIR env (the xxHash include dir; upstream derived it from BACKEND_SRC/../../thirdparty).
# Build the native renderer's GPU backend (src/nr/nr_backend.cpp) together with plume's Vulkan
# backend into ONE archive that can sit next to the ReXGlue SDK: plume compiles its own volk and
# VMA (incl. ~700 global vk* dispatch pointers) which collide with the SDK's. Partial-link both
# objects, then localise vma*/Vma*/volk*/Volk*/vk* (method from the first attempt's
# tools/wwe_render_build_plume.sh, Linux path). Env: PLUME_DIR BACKEND_SRC OUT_ARCHIVE BUILD_DIR.
set -euo pipefail
CXX=${CXX:-clang++}
CXX_FLAGS=${CXX_FLAGS:-"-std=c++17 -O2 -g -fPIC -fvisibility=hidden"}
OBJCOPY=${OBJCOPY:-/usr/lib/llvm-20/bin/llvm-objcopy}
command -v "$OBJCOPY" >/dev/null || OBJCOPY=objcopy
mkdir -p "$BUILD_DIR" "$(dirname "$OUT_ARCHIVE")"
ROOT=$(cd "$(dirname "$BACKEND_SRC")/../.." && pwd)
XXHASH_DIR=${XXHASH_DIR:-$ROOT/thirdparty/xxHash}
INCLUDES=(-I"$PLUME_DIR" -I"$PLUME_DIR/contrib/volk" -I"$PLUME_DIR/contrib/Vulkan-Headers/include"
          -I"$PLUME_DIR/contrib/VulkanMemoryAllocator/include" -I"$(dirname "$BACKEND_SRC")"
          -I"$XXHASH_DIR")
objs=()
for src in "$PLUME_DIR/plume_vulkan.cpp" "$BACKEND_SRC"; do
  obj="$BUILD_DIR/$(basename "$src").o"
  # shellcheck disable=SC2086
  # BACKEND_DEFINES: -DWWE2K14_NO_TEST_TOOLS=1 from CMake when WWE2K14_TEST_TOOLS is OFF (public builds carry no test switches).
  $CXX $CXX_FLAGS ${BACKEND_DEFINES:-} "${INCLUDES[@]}" -c "$src" -o "$obj"
  objs+=("$obj")
done
ld -r "${objs[@]}" -o "$BUILD_DIR/nr_backend_combined.o"
"$OBJCOPY" -w --localize-symbol='vma*' --localize-symbol='Vma*' --localize-symbol='*Vma*' \
  --localize-symbol='volk*' --localize-symbol='Volk*' --localize-symbol='vk*' \
  "$BUILD_DIR/nr_backend_combined.o" "$BUILD_DIR/nr_backend_local.o"
rm -f "$OUT_ARCHIVE"
ar rcs "$OUT_ARCHIVE" "$BUILD_DIR/nr_backend_local.o"
echo "[nr-backend] wrote $OUT_ARCHIVE"
