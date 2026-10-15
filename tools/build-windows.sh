#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
SDK_ROOT=$(cd "$ROOT/../tools/rexglue" && pwd)
XWIN_DIR=${XWIN_DIR:-"$HOME/xwin"}
LLVM_BIN=${LLVM_BIN:-/usr/lib/llvm-20/bin}
SDK_BUILD_DIR="$SDK_ROOT/out/build/win-amd64-cross"
SDK_INSTALL_DIR="$SDK_ROOT/out/install/win-amd64-cross"
GAME_BUILD_DIR="$ROOT/out/build/win-amd64-cross"
TOOLCHAIN_FILE="$SDK_ROOT/cmake/toolchains/win-amd64-xwin.cmake"

if [[ ! -d "$XWIN_DIR" || ! -f "$XWIN_DIR/sdk/include/um/windows.h" ]]; then
  printf 'Windows SDK xwin splat not found under XWIN_DIR=%s\n' "$XWIN_DIR" >&2
  exit 1
fi
if [[ ! -f "$ROOT/generated/sources.cmake" ]]; then
  printf 'Missing %s/generated/sources.cmake; refusing to regenerate generated code.\n' "$ROOT" >&2
  exit 1
fi
if [[ ! -f "$TOOLCHAIN_FILE" ]]; then
  printf 'Missing Windows toolchain file: %s\n' "$TOOLCHAIN_FILE" >&2
  exit 1
fi
for tool in clang clang++ lld-link llvm-ar llvm-ranlib llvm-rc llvm-mt; do
  if [[ ! -x "$LLVM_BIN/$tool" ]]; then
    printf 'Required LLVM tool not executable: %s/%s\n' "$LLVM_BIN" "$tool" >&2
    exit 1
  fi
done

export XWIN_DIR LLVM_BIN

# Budget about 3 GiB per compiler process, capped at the requested SDK/game
# parallelism. This gives the game a maximum of six concurrent generated TUs.
jobs_for_ram() {
  local cap=$1 available_kib=0 key value unit jobs
  if [[ -r /proc/meminfo ]]; then
    while read -r key value unit; do
      if [[ "$key" == MemAvailable: ]]; then
        available_kib=$value
        break
      fi
    done < /proc/meminfo
  fi
  jobs=$((available_kib / (3 * 1024 * 1024)))
  ((jobs < 1)) && jobs=1
  ((jobs > cap)) && jobs=$cap
  printf '%s' "$jobs"
}

SDK_JOBS=$(jobs_for_ram 10)
printf 'Configuring Windows SDK (XWIN_DIR=%s LLVM_BIN=%s, jobs=%s)\n' \
  "$XWIN_DIR" "$LLVM_BIN" "$SDK_JOBS"
cmake -S "$SDK_ROOT" -B "$SDK_BUILD_DIR" -G "Ninja Multi-Config" \
  -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
  -DXWIN_DIR="$XWIN_DIR" \
  -DLLVM_BIN="$LLVM_BIN" \
  -DCMAKE_CONFIGURATION_TYPES=RelWithDebInfo \
  -DREXGLUE_USE_D3D12=OFF \
  -DREXGLUE_USE_VULKAN=ON \
  -DCMAKE_INSTALL_PREFIX="$SDK_INSTALL_DIR"
cmake --build "$SDK_BUILD_DIR" --config RelWithDebInfo --parallel "$SDK_JOBS"
cmake --build "$SDK_BUILD_DIR" --config RelWithDebInfo --target install --parallel "$SDK_JOBS"

GAME_JOBS=$(jobs_for_ram 6)
printf 'Configuring and building Windows game (jobs=%s)\n' "$GAME_JOBS"
cmake --preset win-amd64-cross -S "$ROOT" \
  -DXWIN_DIR="$XWIN_DIR" \
  -DLLVM_BIN="$LLVM_BIN"
cmake --build --preset win-amd64-cross --parallel "$GAME_JOBS"
