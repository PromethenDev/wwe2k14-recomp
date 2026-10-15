#!/usr/bin/env python3
"""Replace the build machine's home-directory prefix in release binaries with a neutral path of the same length.

Compilers embed source paths (__FILE__ in log/assert messages, PDB/debug paths) into the game, launcher and FFmpeg
binaries. Those paths name the build account. This rewrites every occurrence of OLD with NEW byte for byte; because
both have the same length no offset or string length changes, so the code is untouched. It refuses to run when the
lengths differ and verifies that OLD (and the bare account name) no longer occurs afterwards.

Usage: tools/scrub-build-paths.py [--old <home>/] [--new <same-length neutral path>] [--account <user>] FILE...
(edits in place; defaults come from the current user's home directory)
"""
import argparse
import os
import sys


def main() -> int:
    ap = argparse.ArgumentParser()
    home = os.path.expanduser("~").rstrip("/\\") + "/"
    neutral = ("/build/src/" + "x" * len(home))[: len(home) - 1] + "/"
    ap.add_argument("--old", default=home)
    ap.add_argument("--new", default=neutral)
    ap.add_argument("--account", default=os.path.basename(home.rstrip("/")),
                    help="name that must not remain anywhere afterwards")
    ap.add_argument("files", nargs="+")
    a = ap.parse_args()
    old, new = a.old.encode(), a.new.encode()
    if len(old) != len(new):
        print(f"refusing: '{a.old}' and '{a.new}' differ in length", file=sys.stderr)
        return 2
    bad = 0
    for path in a.files:
        data = open(path, "rb").read()
        count = data.count(old)
        data = data.replace(old, new)
        left = data.count(a.account.encode())
        open(path, "wb").write(data)
        print(f"{path}: replaced {count}, '{a.account}' left {left}")
        bad += left
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
