#!/usr/bin/env bash
# tools/setup.sh: get, patch, build and install the ReXGlue SDK this project needs (Linux host).
#
# The SDK lives next to this repository at ../tools/rexglue (all build scripts expect that layout). On first run it is
# cloned from the public fork https://github.com/HollywoodAkeem/rexglue-sdk-yukes at the pinned base commit and the
# WWE 2K14 fixes in rexglue-patches/ are applied with `git am` - the exact SDK the release was built with. Then the SDK
# is configured once (LLVM 20, x86-64-v3, Vulkan, shared FFmpeg) and installed to ../tools/rexglue/out/install/linux-amd64;
# the codegen tool is ../tools/rexglue/out/linux-amd64/RelWithDebInfo/rexglue.
#
# Env: BUILD_JOBS (default 8), LLVM_BIN (default /usr/lib/llvm-20/bin), REXGLUE_URL (fork URL override).
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
SDK="$ROOT/../tools/rexglue"
BASE_COMMIT=987593c0fc6612850a2e1b247b2b03aa5281e663
REXGLUE_URL=${REXGLUE_URL:-https://github.com/HollywoodAkeem/rexglue-sdk-yukes.git}
LLVM_BIN=${LLVM_BIN:-/usr/lib/llvm-20/bin}
BUILD_JOBS=${BUILD_JOBS:-8}
export PATH="$LLVM_BIN:$PATH"

for command in git cmake ninja clang clang++ ld.lld; do
  command -v "$command" >/dev/null || { echo "Missing required command: $command" >&2; exit 1; }
done

if [ ! -d "$SDK/.git" ]; then
  echo "== Fetching ReXGlue ($REXGLUE_URL @ $BASE_COMMIT) into $SDK"
  mkdir -p "$(dirname "$SDK")"
  git clone --no-checkout "$REXGLUE_URL" "$SDK"
  git -C "$SDK" checkout -q -b wwe2k14-fixes "$BASE_COMMIT"
  echo "== Applying the WWE 2K14 patches from rexglue-patches/"
  git -C "$SDK" -c user.name="WWE2K14 Recomp" -c user.email="wwe2k14-recomp@users.noreply.github.com" \
    am -q --keep-cr "$ROOT"/rexglue-patches/*.patch
  git -C "$SDK" submodule update --init --recursive
else
  echo "== Using existing ReXGlue checkout: $SDK ($(git -C "$SDK" rev-parse --short HEAD))"
fi

BUILD_DIR="$SDK/out/build/linux-amd64-clang20"
if [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
  cmake -S "$SDK" -B "$BUILD_DIR" -G "Ninja Multi-Config" \
    -DCMAKE_C_COMPILER="$LLVM_BIN/clang" -DCMAKE_CXX_COMPILER="$LLVM_BIN/clang++" \
    -DCMAKE_C_FLAGS="-march=x86-64-v3" -DCMAKE_CXX_FLAGS="-march=x86-64-v3 -include immintrin.h" \
    -DCMAKE_CONFIGURATION_TYPES="Debug;Release;RelWithDebInfo" \
    -DCMAKE_INSTALL_PREFIX="$SDK/out/install/linux-amd64" \
    -DREXGLUE_BUILD_TESTS=OFF -DREXGLUE_ENABLE_FIDELITYFX=OFF -DREXGLUE_ENABLE_PERF_COUNTERS=OFF \
    -DREXGLUE_ENABLE_SANITIZERS=OFF -DREXGLUE_ENABLE_TRACY=OFF -DREXGLUE_FFMPEG_SHARED=ON \
    -DREXGLUE_PROFILE_GUEST_FUNCTIONS=OFF -DREXGLUE_USE_VULKAN=ON
fi
cmake --build "$BUILD_DIR" --config RelWithDebInfo --target install -j "$BUILD_JOBS"

REXGLUE_BIN="$SDK/out/linux-amd64/RelWithDebInfo/rexglue"
test -x "$REXGLUE_BIN" || { echo "ReXGlue binary was not produced: $REXGLUE_BIN" >&2; exit 1; }

# Render the SDK-managed generated/rexglue.cmake through a temporary project;
# see tools/ensure-rexglue-cmake.sh for why this repository is not force-inited.
"$ROOT/tools/ensure-rexglue-cmake.sh"

echo "ReXGlue fetched, patched, built and installed; project boilerplate prepared."
