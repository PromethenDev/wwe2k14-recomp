#!/usr/bin/env python3
"""Merge saved pipeline lists (userdata/cache/pipelines.bin, format "W12P" v1) into one
pipelines_seed.w12p for the kit. The game pre-builds every listed pipeline in the background at
start (pso_recorder.cpp ReplayBootCache), so a first session in a new folder does not stall on
shader builds. Entries hold only render state and shader content hashes; entries whose shaders a
session never creates stay pending and cost nothing.

usage: merge-pso-seed.py OUT.w12p IN.bin [IN.bin ...]
"""
import struct
import sys

MAGIC, VERSION, MAX_ENTRIES = 0x50323157, 1, 50000


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__, file=sys.stderr)
        return 2
    out, inputs = sys.argv[1], sys.argv[2:]
    entry_size = None
    entries: dict[bytes, None] = {}
    for path in inputs:
        data = open(path, "rb").read()
        magic, version, size, count = struct.unpack_from("<4I", data)
        if magic != MAGIC or version != VERSION or len(data) != 16 + count * size:
            print(f"skip {path}: not a W12P v1 list or truncated", file=sys.stderr)
            continue
        if entry_size is None:
            entry_size = size
        elif size != entry_size:
            print(f"skip {path}: entry size {size} != {entry_size} (other build)", file=sys.stderr)
            continue
        for i in range(count):
            entries.setdefault(data[16 + i * size:16 + (i + 1) * size])
    if entry_size is None or not entries:
        print("no usable input", file=sys.stderr)
        return 1
    if len(entries) > MAX_ENTRIES:
        print(f"{len(entries)} entries > {MAX_ENTRIES}", file=sys.stderr)
        return 1
    with open(out, "wb") as f:
        f.write(struct.pack("<4I", MAGIC, VERSION, entry_size, len(entries)))
        f.write(b"".join(entries))
    print(f"{out}: {len(entries)} pipelines (entry size {entry_size}) from {len(inputs)} lists")
    return 0


if __name__ == "__main__":
    sys.exit(main())
