#!/usr/bin/env bash
# Linux cross-build: run reblue-XenosRecomp's Linux DXC with its libdxcompiler.so (cmake/native_renderer.cmake).
# usage: dxc-linux.sh <dxc-bin dir> <dxc arguments...>
D=$1; shift
LD_LIBRARY_PATH="$D/lib/x64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" exec "$D/bin/x64/dxc-linux" "$@"
