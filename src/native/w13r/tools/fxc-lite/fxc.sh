#!/usr/bin/env bash
# fxc for the Linux cross-build: fxc-lite.exe (tools/fxc-lite/fxc-lite.cpp, built by CMake) with Microsoft's own
# D3DCompiler_47.dll (copied from a Windows PC, never committed) under Wine. usage: fxc.sh <dir with both> <fxc args>
D=$1; shift
export WINEPREFIX=${FXC_WINEPREFIX:-$HOME/.wine-fxc} WINEDEBUG=-all WINEDLLOVERRIDES="d3dcompiler_47=n"
exec wine "$D/fxc-lite.exe" "$@"
