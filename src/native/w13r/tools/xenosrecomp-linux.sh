#!/usr/bin/env bash
# Linux cross-build: reblue-XenosRecomp built for Linux (build-linux/, -DREBLUE_RECOMP), with dxc-bin's libdxcompiler.so.
# usage: xenosrecomp-linux.sh <reblue-XenosRecomp dir> <XenosRecomp arguments...>
X=$1; shift
LD_LIBRARY_PATH="$X/thirdparty/dxc-bin/lib/x64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" exec "$X/build-linux/XenosRecomp/XenosRecomp" "$@"
