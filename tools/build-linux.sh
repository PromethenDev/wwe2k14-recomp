#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
test -f "$ROOT/generated/sources.cmake" || { echo "Run ./tools/codegen.sh first." >&2; exit 1; }
"$ROOT/tools/ensure-rexglue-cmake.sh"
cmake --preset linux-amd64-relwithdebinfo -S "$ROOT"
cmake --build --preset linux-amd64-relwithdebinfo
