#!/usr/bin/env python3
"""
xiso_get.py - copy only the program files (*.xex, *.dll) out of an Xbox 360
disc image, without unpacking the whole disc.

Used to get World at War's Xbox 360 executables for decompiling. The disc
image is only read, never changed.

Usage (from the project folder, Linux or Windows):
    python3 tools/waw_decomp/xiso_get.py "<path to the game>.iso" dev/decomp-input/waw360
    python3 tools/waw_decomp/xiso_get.py "<path to the game>.iso" --list

Options:
    --pattern GLOB   extra file name pattern to copy (repeatable), e.g. --pattern "*.ini"
"""
import argparse
import fnmatch
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))  # tools/ (for extract_xiso)

from extract_xiso import CHUNK, SECTOR, find_partition, human, walk  # noqa: E402

DEFAULT_PATTERNS = ["*.xex", "*.dll"]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("iso")
    ap.add_argument("out_dir", nargs="?", default=os.path.join("dev", "decomp-input", "waw360"))
    ap.add_argument("--list", action="store_true", help="only list the files that would be copied")
    ap.add_argument("--pattern", action="append", default=[], help="extra file name pattern to copy")
    args = ap.parse_args()
    patterns = DEFAULT_PATTERNS + args.pattern

    iso = os.path.abspath(args.iso)
    out_root = os.path.abspath(args.out_dir)
    if out_root == os.path.dirname(iso) or out_root.startswith(iso):
        sys.exit("error: choose an output folder that is not the disc image's own folder")

    with open(iso, "rb") as f:
        kind, base, root_sector, root_size = find_partition(f)
        print(f"[xiso_get] layout {kind}, game partition at 0x{base:X}")
        wanted = [
            (rel, start, size)
            for rel, is_dir, start, size in walk(f, base, root_sector, root_size)
            if not is_dir and any(fnmatch.fnmatch(os.path.basename(rel).lower(), p.lower()) for p in patterns)
        ]
        if not wanted:
            sys.exit("error: no matching files on this disc")
        for rel, _, size in wanted:
            print(f"{size:>12}  {rel}")
        if args.list:
            return

        os.makedirs(out_root, exist_ok=True)
        for rel, start, size in wanted:
            dst = os.path.abspath(os.path.join(out_root, rel))
            if not dst.startswith(out_root + os.sep):
                sys.exit(f"error: unsafe path {rel!r}")
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            if os.path.exists(dst) and os.path.getsize(dst) == size:
                print(f"[xiso_get] already there: {rel}")
                continue
            f.seek(base + start * SECTOR)
            with open(dst, "wb") as o:
                left = size
                while left:
                    buf = f.read(min(CHUNK, left))
                    if not buf:
                        sys.exit(f"error: image truncated while reading {rel}")
                    o.write(buf)
                    left -= len(buf)
            print(f"[xiso_get] copied {rel} ({human(size)})")
    print(f"[xiso_get] done -> {out_root}")


if __name__ == "__main__":
    main()
