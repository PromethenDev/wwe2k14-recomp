#!/usr/bin/env bash
set -euo pipefail

# Ensure the SDK-managed generated/rexglue.cmake exists.
#
# generated/rexglue.cmake is rendered by 'rexglue init' and is required by the
# top-level include() in CMakeLists.txt. It is SDK-managed, so never hand-write
# it: render it into a temporary project (exactly like tools/setup.sh does) and
# copy it in. This repository is never force-initialized because that would
# overwrite its hand-maintained files.

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
SDK="$ROOT/../tools/rexglue"
REXGLUE_BIN="$SDK/out/linux-amd64/RelWithDebInfo/rexglue"

if [ -f "$ROOT/generated/rexglue.cmake" ]; then
  exit 0
fi

test -x "$REXGLUE_BIN" || {
  echo "Missing ReXGlue binary: $REXGLUE_BIN (run ./tools/setup.sh first)." >&2
  exit 1
}

TMP=$(mktemp -d)
TMP_DEST="$ROOT/generated/.rexglue.cmake.tmp.$$"
trap 'rm -rf "$TMP"; rm -f "$TMP_DEST"' EXIT
"$REXGLUE_BIN" init --app_name wwe2k14 --app_root "$TMP/wwe2k14"
mkdir -p "$ROOT/generated"
# Atomic install: copy beside the destination, then rename over it, so an
# interrupted copy cannot leave a partial file that the existence check accepts.
cp "$TMP/wwe2k14/generated/rexglue.cmake" "$TMP_DEST"
mv -f "$TMP_DEST" "$ROOT/generated/rexglue.cmake"
echo "Rendered generated/rexglue.cmake via rexglue init."
