#!/usr/bin/env bash
# Windows (COFF) build of the nr backend for the WWE 2K14 native workspace (2026-10-13).
# Adapted from <wwe13-recomp-nr>/tools/nr/build-backend-win.sh
#   (repo HEAD 19c8bb361a9b7d6cba65afffc628bda023da2e0c, read-only reference).
# Local changes: Vulkan + (decision 0007, 2026-10-18 night) plume_d3d12.cpp / D3D12MemAlloc.cpp as in the reference
#   (D3D12MA symbols localised, NDEBUG; used only with WWE2K14_NR_BACKEND=d3d12);
#   XXHASH_DIR env like the workspace build-backend.sh; the rename list is the union of vk*/volk*/Vma*
#   symbols DEFINED by plume_vulkan.obj, applied to BOTH objects, so nr_backend.obj's references to
#   plume's volk/VMA resolve to plume's copy (the COFF stand-in for `ld -r` + --localize-symbol on ELF).
#   The script fails if nr_backend.obj references a vk*/volk*/Vma* symbol plume does not define.
# Env: PLUME_DIR XXHASH_DIR BACKEND_SRC OUT_ARCHIVE BUILD_DIR; XWIN_DIR LLVM_BIN.
set -euo pipefail
LLVM_BIN=${LLVM_BIN:-/usr/lib/llvm-20/bin}
XWIN_DIR=${XWIN_DIR:-$HOME/xwin}
CXX="$LLVM_BIN/clang++"
TARGET=x86_64-pc-windows-msvc
# -DNOMINMAX: windows.h min/max macros. -D_CRT_SECURE_NO_WARNINGS: getenv deprecation.
# CRT selection is the clang GNU-driver default for the msvc target (static libcmt), as in the WWE 13 script.
CXX_FLAGS=${CXX_FLAGS:-"-std=c++17 -O2 -g --target=$TARGET -march=x86-64-v3 -DNOMINMAX -D_CRT_SECURE_NO_WARNINGS"}
OBJCOPY="$LLVM_BIN/llvm-objcopy"; NM="$LLVM_BIN/llvm-nm"; AR="$LLVM_BIN/llvm-ar"
mkdir -p "$BUILD_DIR" "$(dirname "$OUT_ARCHIVE")"
INCLUDES=(-I"$PLUME_DIR" -I"$PLUME_DIR/contrib/volk" -I"$PLUME_DIR/contrib/Vulkan-Headers/include"
          -I"$PLUME_DIR/contrib/VulkanMemoryAllocator/include" -I"$(dirname "$BACKEND_SRC")" -I"$XXHASH_DIR"
          -I"$PLUME_DIR/contrib/D3D12MemoryAllocator/include")
WININCLUDES=()
for d in crt/include sdk/include/ucrt sdk/include/um sdk/include/shared sdk/include/winrt; do
  WININCLUDES+=(-Xclang -internal-isystem -Xclang "$XWIN_DIR/$d")
done
match() { case "$1" in vk*|volk*|Volk*|vma*|Vma*|*Vma*) return 0;; esac; return 1; }
P_RAW="$BUILD_DIR/plume_vulkan.obj"; B_RAW="$BUILD_DIR/nr_backend.obj"
# shellcheck disable=SC2086
$CXX $CXX_FLAGS "${INCLUDES[@]}" "${WININCLUDES[@]}" -c "$PLUME_DIR/plume_vulkan.cpp" -o "$P_RAW"
# shellcheck disable=SC2086
# BACKEND_DEFINES: -DWWE2K14_NO_TEST_TOOLS=1 from CMake when WWE2K14_TEST_TOOLS is OFF (public builds carry no test switches).
# shellcheck disable=SC2086
$CXX $CXX_FLAGS ${BACKEND_DEFINES:-} "${INCLUDES[@]}" "${WININCLUDES[@]}" -c "$BACKEND_SRC" -o "$B_RAW"
RENAMES="$BUILD_DIR/renames.txt"; : > "$RENAMES"
"$NM" --defined-only --extern-only "$P_RAW" | awk '{print $NF}' | sort -u > "$BUILD_DIR/plume_defined.txt"
while IFS= read -r s; do match "$s" && printf '%s __nr_plume_%s\n' "$s" "$s" >> "$RENAMES"; done < "$BUILD_DIR/plume_defined.txt"
"$NM" --undefined-only --extern-only "$B_RAW" | awk '{print $NF}' | sort -u > "$BUILD_DIR/backend_undef.txt"
bad=0
while IFS= read -r s; do
  if match "$s" && ! grep -qxF "$s" "$BUILD_DIR/plume_defined.txt"; then echo "[nr-backend-win] backend needs $s not defined by plume" >&2; bad=1; fi
done < "$BUILD_DIR/backend_undef.txt"
[[ $bad == 0 ]] || exit 1
"$OBJCOPY" --redefine-syms="$RENAMES" "$P_RAW" "$BUILD_DIR/plume_vulkan.local.obj"
"$OBJCOPY" --redefine-syms="$RENAMES" "$B_RAW" "$BUILD_DIR/nr_backend.local.obj"
# D3D12 (decision 0007; from the WWE '13 reference build-backend-win.sh:65-97): plume_d3d12.cpp references D3D12MA in
# a separate object, so defined AND undefined *D3D12MA* symbols are renamed in both objects.
d3d_objs=()
for src in "$PLUME_DIR/plume_d3d12.cpp" "$PLUME_DIR/contrib/D3D12MemoryAllocator/src/D3D12MemAlloc.cpp"; do
  raw="$BUILD_DIR/$(basename "$src").obj"; obj="$BUILD_DIR/$(basename "$src").local.obj"
  # shellcheck disable=SC2086
  $CXX $CXX_FLAGS -DNDEBUG "${INCLUDES[@]}" "${WININCLUDES[@]}" -c "$src" -o "$raw"
  ren="$BUILD_DIR/$(basename "$src").renames.txt"
  "$NM" --extern-only "$raw" | awk 'NF>=2 {print $NF}' | sort -u | { grep 'D3D12MA' || true; } | awk '{print $1" __nr_plume_"$1}' > "$ren"
  if [[ -s "$ren" ]]; then "$OBJCOPY" --redefine-syms="$ren" "$raw" "$obj"; else cp "$raw" "$obj"; fi
  d3d_objs+=("$obj")
done
rm -f "$OUT_ARCHIVE"
"$AR" rcs "$OUT_ARCHIVE" "$BUILD_DIR/plume_vulkan.local.obj" "$BUILD_DIR/nr_backend.local.obj" "${d3d_objs[@]}"
echo "[nr-backend-win] wrote $OUT_ARCHIVE ($(wc -l < "$RENAMES") symbols renamed)"
