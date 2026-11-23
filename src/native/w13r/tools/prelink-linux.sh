#!/usr/bin/env bash
# Linux cross-build: wwe13_prelink built for the Linux host (same sources as the Windows tool) against dxc-bin's
# libdxcompiler.so, then run. usage: prelink-linux.sh <port dir> <reblue-XenosRecomp dir> <miniz dir> <work dir>
#                                                    <shader_cache.cpp> <out linked_shader_cache.cpp>
set -e
P=$1 X=$2 MZ=$3 W=$4 CACHE=$5 OUT=$6
D=$X/thirdparty/dxc-bin
mkdir -p "$W"
clang-20 -O2 -w -c -I"$P/third_party/reblue_thirdparty/zstd" "$P/third_party/reblue_thirdparty/zstd/zstddeclib.c" -o "$W/zstd.o"
clang-20 -O2 -w -c -I"$MZ" "$MZ/miniz.c" -o "$W/miniz.o"
clang++-20 -std=c++20 -O2 -w -fms-extensions -I"$P/src/reblue" -I"$P/third_party/reblue_thirdparty/zstd" -I"$MZ" -I"$D/inc" \
  "$P/src/prelink/wwe13_prelink.cpp" "$P/src/reblue/gpu/shaders/dxc_link.cpp" "$CACHE" "$W/zstd.o" "$W/miniz.o" \
  -L"$D/lib/x64" -ldxcompiler -Wl,-rpath,"$D/lib/x64" -lpthread -o "$W/wwe13_prelink-linux"
# libdxcompiler.so dlopens libdxil.so (the validator that signs the DXIL); the RUNPATH above does not cover
# that dlopen, so without this every linked shader came out unsigned and D3D12 rejected all pipelines.
LD_LIBRARY_PATH="$D/lib/x64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" exec "$W/wwe13_prelink-linux" "$OUT"
