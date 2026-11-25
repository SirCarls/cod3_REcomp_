#!/usr/bin/env python3
"""
tu_strings.py - for the functions tu_diff.py lists as changed or added, the
text strings each one uses that the other side doesn't. New messages, setting
names and asset names are the quickest way to tell what an update fixed.

Usage:
    python3 tools/waw_decomp/tu_strings.py disc.pe patched.pe functions.tsv [--min-similarity 0.98]

Analysis only: the output quotes the game's own strings, so keep it private.
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ppc_words import gpr_destinations, jump_table_addresses  # noqa: E402

LOAD_ADDRESS = 0x82000000


def c_string(image, address):
    off = address - LOAD_ADDRESS
    if off < 0 or off >= len(image):
        return None
    end = image.find(b"\0", off, off + 200)
    if end - off < 4:
        return None
    raw = image[off:end]
    if all(32 <= b < 127 for b in raw):
        return raw.decode("ascii")
    return None


def strings_used(image, start, size):
    """Strings whose address the function builds with lis + addi/ori. A `lis` counts only until its register is
    written again, and a switch's table after a `bctr` is skipped (ppc_words.py says why)."""
    high = {}
    found = set()
    base = start - LOAD_ADDRESS
    words = [struct.unpack_from(">I", image, base + i)[0] for i in range(0, size, 4)]
    table = jump_table_addresses(words, start, size)
    for i, word in enumerate(words):
        if start + 4 * i in table:
            continue
        op, rt, ra = word >> 26, (word >> 21) & 31, (word >> 16) & 31
        if op == 15 and ra == 0:  # lis
            high[rt] = (word & 0xFFFF) << 16
            continue
        if op == 14 and ra in high:  # addi
            low = word & 0xFFFF
            low = low - 0x10000 if low & 0x8000 else low
            s = c_string(image, (high[ra] + low) & 0xFFFFFFFF)
            if s:
                found.add(s)
        elif op == 24 and rt in high:  # ori: the source is the RS field (bits 6-10), the destination RA
            s = c_string(image, high[rt] | (word & 0xFFFF))
            if s:
                found.add(s)
        for register in gpr_destinations(word):
            high.pop(register, None)
    return found


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("disc")
    ap.add_argument("patched")
    ap.add_argument("functions")
    ap.add_argument("--min-similarity", type=float, default=0.98)
    args = ap.parse_args()
    disc = open(args.disc, "rb").read()
    patched = open(args.patched, "rb").read()
    rows = [line.rstrip("\n").split("\t") for line in open(args.functions, encoding="utf-8")][1:]
    for status, a_addr, a_size, b_addr, b_size, similarity, name in rows:
        if status not in ("changed", "added", "removed"):
            continue
        if status == "changed" and similarity and float(similarity) >= args.min_similarity:
            continue
        old = strings_used(disc, int(a_addr, 16), int(a_size, 16)) if status != "added" else set()
        new = strings_used(patched, int(b_addr, 16), int(b_size, 16)) if status != "removed" else set()
        gained, lost = sorted(new - old), sorted(old - new)
        if not gained and not lost:
            continue
        label = name or (a_addr if status != "added" else "new@" + b_addr)
        print(f"{status} {label} ({similarity or '-'})")
        for s in gained[:8]:
            print(f"   + {s}")
        for s in lost[:8]:
            print(f"   - {s}")


if __name__ == "__main__":
    main()
