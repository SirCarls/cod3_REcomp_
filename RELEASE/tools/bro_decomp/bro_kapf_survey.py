#!/usr/bin/env python3
"""bro_kapf_survey.py - what Call of Duty 2: Big Red One's .cod archives (KAPF 2.02) hold.

Reads archives as data only and needs only Python's standard library. For each archive given
(files, or folders searched for *.cod), it lists the sections and chunks and the first four bytes
of every chunk. For the first chunk of each kind (by those four bytes) it also writes a hex dump of
its start and the printable strings in its first 64 KB, so the kind's layout can be worked out.

KAPF 2.02 (claude-5d's notes in engine/crates/kapf/src/lib.rs): "KAPF", the version 2.02 as a
float, u32 header size (0x1C), u32 section count; section records of 0x18 bytes (char name[12],
u16 chunk count, u16 unknown, u32 chunk table offset, u32 unknown); chunk entries of 0x1C bytes
(u32 file offset, u32 data size, u32 slot size, u32 flags, reserved). Chunks are stored as is.

Usage:
    python3 bro_kapf_survey.py --out <folder> <archive or folder>...
Writes survey.tsv (archive, section, chunk, offset, size, magic) and kinds.txt into --out.
"""
import argparse
import collections
import os
import re
import struct
import sys

VERSION_BRO = 0x400147AE
DUMP_BYTES = 0x200
STRING_SCAN = 0x10000
MAX_STRINGS = 120


def chunks(path):
    with open(path, "rb") as fh:
        d = fh.read()
    if d[:4] != b"KAPF":
        return d, None
    version, hsize, nsec = struct.unpack_from("<III", d, 4)
    out = []
    for i in range(nsec):
        rec = hsize + 0x18 * i
        name = d[rec:rec + 12].split(b"\0")[0].decode("latin-1")
        count, _unk, table, _unk2 = struct.unpack_from("<HHII", d, rec + 12)
        entry = 0x1C if version == VERSION_BRO else 0x20
        for c in range(count):
            off, size, slot, flags = struct.unpack_from("<IIII", d, table + entry * c)
            out.append((name, c, off, size, flags))
    return d, (version, out)


def magic(b):
    m = b[:4]
    return m.decode("latin-1") if all(32 <= x < 127 for x in m) else m.hex()


def hexdump(b, base=0):
    lines = []
    for i in range(0, len(b), 16):
        row = b[i:i + 16]
        text = "".join(chr(x) if 32 <= x < 127 else "." for x in row)
        lines.append(f"{base + i:06X}  {row.hex(' '):<48}  {text}")
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True)
    ap.add_argument("paths", nargs="+")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    files = []
    for p in a.paths:
        if os.path.isdir(p):
            for root, _dirs, names in os.walk(p):
                files += [os.path.join(root, n) for n in sorted(names) if n.lower().endswith(".cod")]
        else:
            files.append(p)
    kinds = collections.Counter()
    sizes = collections.Counter()
    first = {}
    with open(os.path.join(a.out, "survey.tsv"), "w", encoding="utf-8") as fh:
        fh.write("archive\tsection\tchunk\toffset\tsize\tflags\tmagic\n")
        for f in files:
            d, info = chunks(f)
            rel = os.path.basename(os.path.dirname(f)) + "/" + os.path.basename(f)
            if info is None:
                print(f"bro_kapf_survey: {rel}: not a KAPF archive", file=sys.stderr)
                continue
            for name, c, off, size, flags in info[1]:
                b = d[off:off + size]
                k = (name or "(main)", magic(b))
                kinds[k] += 1
                sizes[k] += size
                first.setdefault(k, (rel, c, off, b))
                fh.write(f"{rel}\t{name}\t{c}\t0x{off:X}\t{size}\t0x{flags:X}\t{magic(b)}\n")
    with open(os.path.join(a.out, "kinds.txt"), "w", encoding="utf-8") as fh:
        fh.write(f"{len(files)} archives\n\n")
        for k, n in kinds.most_common():
            fh.write(f"{k[0]:<8} {k[1]:<10} {n:6} chunks {sizes[k] / 1e6:10.1f} MB\n")
        for k, (rel, c, off, b) in first.items():
            fh.write(f"\n===== {k[0]} {k[1]}: first in {rel} chunk {c} (file offset 0x{off:X}, {len(b)} bytes)\n")
            fh.write(hexdump(b[:DUMP_BYTES]) + "\n")
            strs = [(m.start(), m.group().decode("latin-1"))
                    for m in re.finditer(rb"[\x20-\x7E]{5,}", b[:STRING_SCAN])]
            fh.write(f"-- {len(strs)} strings in the first 64 KB; the first {min(len(strs), MAX_STRINGS)}:\n")
            for o, s in strs[:MAX_STRINGS]:
                fh.write(f"{o:06X}  {s[:100]}\n")
    print(f"bro_kapf_survey: {len(files)} archives, "
          + ", ".join(f"{k[0]}/{k[1]} {n}" for k, n in kinds.most_common()))


if __name__ == "__main__":
    main()
