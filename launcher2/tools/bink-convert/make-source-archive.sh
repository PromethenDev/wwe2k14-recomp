#!/bin/sh
set -eu

if [ "$#" -ne 1 ]; then
  echo "usage: $0 OUTPUT.tar.gz" >&2
  exit 2
fi

source_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
archive_root=$(CDPATH= cd -- "$source_dir/../.." && pwd)
output=$1
epoch=${SOURCE_DATE_EPOCH:-0}
case "$epoch" in
  ''|*[!0-9]*) echo "SOURCE_DATE_EPOCH must be a nonnegative integer" >&2; exit 2 ;;
esac

case "$(cd -- "$(dirname -- "$output")" && pwd)/$(basename -- "$output")" in
  "$source_dir"/*) echo "write the archive outside tools/bink-convert" >&2; exit 2 ;;
esac

tar --sort=name --format=gnu --mtime="@$epoch" --owner=0 --group=0 --numeric-owner \
  --mode='u+rwX,go+rX,go-w' --exclude='tools/bink-convert/target' --exclude='tools/bink-convert/target-win' \
  --exclude='tools/bink-convert/.git' -C "$archive_root" -cf - tools/bink-convert |
  gzip -n -9 > "$output"
