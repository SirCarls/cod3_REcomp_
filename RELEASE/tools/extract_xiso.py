#!/usr/bin/env python3
"""
extract_xiso.py - dump the game partition of an Xbox 360 (or original Xbox)
disc image to a folder.  Works on redump-style ISOs (XGD1/XGD2/XGD3, which
carry a video partition in front of the game data) and on trimmed XISOs.

Usage:
    python3 tools/extract_xiso.py "<path to game>.iso" assets
    python3 tools/extract_xiso.py "<path to game>.iso" --list

Re-running is safe: files that already exist with the right size are skipped.
"""
import argparse
import os
import struct
import sys
import time

SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"
# Byte offset of the XDVDFS game partition for each disc layout.
PARTITIONS = [
    ("XISO", 0x00000000),
    ("XGD2", 0x0FD90000),   # most 2006-2010 Xbox 360 titles (CoD3 is XGD2)
    ("XGD3", 0x02080000),
    ("XGD1", 0x18300000),
]
ATTR_DIRECTORY = 0x10
CHUNK = 8 * 1024 * 1024


def find_partition(f):
    for kind, off in PARTITIONS:
        f.seek(off + 32 * SECTOR)
        hdr = f.read(SECTOR)
        if len(hdr) == SECTOR and hdr[:20] == MAGIC and hdr[0x7EC:0x7EC + 20] == MAGIC:
            root_sector, root_size = struct.unpack_from("<II", hdr, 20)
            return kind, off, root_sector, root_size
    sys.exit("error: no XDVDFS volume found - is this an Xbox / Xbox 360 game image?")


def safe_name(name):
    name = name.replace("\\", "_").replace("/", "_")
    if name in ("", ".", ".."):
        raise ValueError(f"refusing unsafe file name {name!r}")
    return name


def read_dir(f, base, sector, size):
    """Walk one XDVDFS directory table (a binary tree) and yield its entries."""
    if size == 0:
        return
    f.seek(base + sector * SECTOR)
    data = f.read(size)
    stack, seen = [0], set()
    while stack:
        off = stack.pop()
        if off in seen or off + 14 > len(data):
            continue
        seen.add(off)
        left, right, start, fsize, attrs, nlen = struct.unpack_from("<HHIIBB", data, off)
        if left == 0xFFFF and right == 0xFFFF:
            continue  # sector padding
        name = data[off + 14:off + 14 + nlen].decode("latin-1")
        yield name, attrs, start, fsize
        for child in (left, right):
            if child and child != 0xFFFF and child * 4 < len(data):
                stack.append(child * 4)


def walk(f, base, sector, size, prefix=""):
    for name, attrs, start, fsize in sorted(read_dir(f, base, sector, size), key=lambda e: e[0].lower()):
        rel = prefix + safe_name(name)
        if attrs & ATTR_DIRECTORY:
            yield rel, True, start, fsize
            yield from walk(f, base, start, fsize, rel + "/")
        else:
            yield rel, False, start, fsize


def human(n):
    for unit in ("B", "KB", "MB", "GB"):
        if n < 1024:
            return f"{n:.1f} {unit}"
        n /= 1024
    return f"{n:.1f} TB"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("iso")
    ap.add_argument("out_dir", nargs="?", default="assets")
    ap.add_argument("--list", action="store_true", help="only list the files")
    args = ap.parse_args()

    with open(args.iso, "rb") as f:
        kind, base, root_sector, root_size = find_partition(f)
        print(f"[xiso] layout {kind}, game partition at 0x{base:X}")
        entries = list(walk(f, base, root_sector, root_size))
        files = [e for e in entries if not e[1]]
        total = sum(e[3] for e in files)
        print(f"[xiso] {len(files)} files, {human(total)}")

        if args.list:
            for rel, is_dir, start, size in entries:
                print(("<DIR> " if is_dir else f"{size:>12} ") + rel)
            return

        out_root = os.path.abspath(args.out_dir)
        os.makedirs(out_root, exist_ok=True)
        done, t0 = 0, time.time()
        # default.xex first so it is usable even if the full dump is interrupted
        files.sort(key=lambda e: (e[0].lower() != "default.xex", e[0].lower()))
        for rel, is_dir, start, size in entries:
            if is_dir:
                os.makedirs(os.path.join(out_root, rel), exist_ok=True)
        for rel, _, start, size in files:
            dst = os.path.join(out_root, rel)
            if not os.path.abspath(dst).startswith(out_root + os.sep):
                sys.exit(f"error: unsafe path {rel!r}")
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            if os.path.exists(dst) and os.path.getsize(dst) == size:
                done += size
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
            done += size
            rate = done / max(time.time() - t0, 1e-3)
            print(f"[xiso] {100 * done / max(total, 1):5.1f}%  {human(rate)}/s  {rel}", flush=True)
        print(f"[xiso] done -> {out_root}")


if __name__ == "__main__":
    main()
