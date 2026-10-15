#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
SDK="$ROOT/../tools/rexglue"
REXGLUE_BIN="$SDK/out/linux-amd64/RelWithDebInfo/rexglue"
XEX="$ROOT/game/default.xex"
LOG="$ROOT/logs/codegen-$(date +%Y%m%d-%H%M%S).log"

test -x "$REXGLUE_BIN" || { echo "Run ./tools/setup.sh first." >&2; exit 1; }
test -f "$XEX" || { echo "Place or symlink your owned XEX at $XEX." >&2; exit 1; }
mkdir -p "$ROOT/logs" "$ROOT/out"
export REX_DUMP_PATCHED_IMAGE="$ROOT/out/tu-image.bin"
"$REXGLUE_BIN" codegen "$ROOT/wwe2k14_config.toml" 2>&1 | tee "$LOG"
