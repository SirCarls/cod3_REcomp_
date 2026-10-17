#!/usr/bin/env python3
"""
same_code.py - find functions that are the same machine code in two or more Xbox 360 builds
(different games of the family, or different builds of one game), and carry names across.

Games built with the same Xbox 360 development kit and middleware share library code that was
compiled once (the C runtime, Direct3D, XAudio, the Demonware network library, Bink ...) and the
engine code that didn't change between them. Such a function has exactly the same instructions
in both executables except for the addresses inside them, so two functions whose instructions
match once addresses are taken out are the same function, without any guessing from strings or
call graphs.

What is taken out ("masked"): branch and call offsets (relative, so they change when the code
moves), the high half of an address built with lis, and the low half added to a register that a
lis loaded (addi/ori/load/store on that register). Everything else must match: registers, opcodes,
structure offsets, constants. A function is only paired when its masked code is unique in both
builds (identical copies inside one build pair with nothing) and at least --min-size bytes long.

Usage:
    python3 tools/x360_family/same_code.py --build LABEL RAW_DIR IMAGE [NAMES] ... --out-dir DIR
        [--min-size 24]

RAW_DIR is a WawExport.java export (functions.tsv), IMAGE the xex2pe.py image, NAMES a names.tsv
(addr, name, kind, pass, evidence). Writes DIR/pairs_<A>_<B>.tsv for every two builds:
a, b, size, a_name, b_name; and DIR/summary.tsv.
"""
import argparse
import collections
import csv
import hashlib
import itertools
import os
import re
import struct
import sys

GENERIC = re.compile(r"^(FUN_|thunk_|LAB_|DAT_|switch|table_|Dvar_Register_[0-9A-F]{8}$|sub_[0-9A-F]{8}$)")


def read_tsv(path):
    with open(path, encoding="utf-8", newline="") as f:
        return list(csv.DictReader(f, delimiter="\t", quoting=csv.QUOTE_NONE))


class Image:
    def __init__(self, path, base=0x82000000):
        with open(path, "rb") as f:
            self.img = f.read()
        self.base = base

    def words(self, a, n):
        o = a - self.base
        if o < 0 or o + n > len(self.img):
            return None
        return struct.unpack_from(">%dI" % (n // 4), self.img, o)


def masked(words):
    """Instructions with the addresses taken out (see the module comment)."""
    hi = set()  # registers holding the high half of an address (lis)
    out = []
    for w in words:
        op = w >> 26
        rd, ra = (w >> 21) & 31, (w >> 16) & 31
        if op == 18:                       # b / bl
            out.append((op << 26) | (w & 3))
            continue
        if op == 16:                       # bc
            out.append((w & 0xFFFF0000) | (w & 3))
            continue
        if op == 15 and ra == 0:           # lis rD, hi
            out.append(w & 0xFFFF0000)
            hi.add(rd)
            continue
        if op in (14, 24, 25) or 32 <= op <= 55 or op in (58, 62):
            base = ra if op not in (24, 25) else rd   # ori/oris: rA = rS | imm, the source is rS
            if base in hi and base != 0:
                out.append(w & 0xFFFF0000)
            else:
                out.append(w)
            # the destination of addi/loads no longer holds a lis value (unless it is the base)
            dest = rd if op not in (24, 25) else ra
            if op < 36 or op in (40, 41, 42, 43, 48, 49, 50, 51, 58):  # writes rD (loads, addi)
                if dest != base:
                    hi.discard(dest)
            continue
        if op == 31:
            hi.discard(rd)
            hi.discard(ra)
        out.append(w)
    return out


class Build:
    def __init__(self, label, raw, image, names):
        self.label = label
        self.img = Image(image)
        self.funcs = {}
        for r in read_tsv(os.path.join(raw, "functions.tsv")):
            a, size = int(r["entry"], 16), int(r["size"])
            if r.get("thunk") == "1" or size <= 0 or size % 4 or size > 1 << 20:
                continue
            self.funcs[a] = size
        self.names = {}
        if names:
            for r in read_tsv(names):
                if r["kind"] == "function" and not GENERIC.match(r["name"]):
                    self.names[int(r["addr"], 16)] = (r["name"], r["pass"])
        self.hash = {}
        self.by_hash = collections.defaultdict(list)
        for a, size in self.funcs.items():
            w = self.img.words(a, size)
            if w is None:
                continue
            h = hashlib.sha1(struct.pack(">%dI" % len(w), *masked(w))).digest() + struct.pack(">I", size)
            self.hash[a] = h
            self.by_hash[h].append(a)

    def unique(self, min_size):
        return {h: v[0] for h, v in self.by_hash.items() if len(v) == 1 and self.funcs[v[0]] >= min_size}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build", nargs="+", action="append", required=True, metavar="LABEL RAW IMAGE [NAMES]")
    ap.add_argument("--out-dir", required=True)
    ap.add_argument("--min-size", type=int, default=24)
    args = ap.parse_args()
    builds = []
    for b in args.build:
        if len(b) not in (3, 4):
            sys.exit("--build LABEL RAW_DIR IMAGE [NAMES]")
        builds.append(Build(b[0], b[1], b[2], b[3] if len(b) == 4 else None))
        print(f"{b[0]}: {len(builds[-1].funcs)} functions, {len(builds[-1].unique(args.min_size))} with unique code")
    os.makedirs(args.out_dir, exist_ok=True)
    summary = open(os.path.join(args.out_dir, "summary.tsv"), "w")
    summary.write("a\tb\tpairs\tnamed_in_a_only\tnamed_in_b_only\tsame_name\tother_name\n")
    for A, B in itertools.combinations(builds, 2):
        ua, ub = A.unique(args.min_size), B.unique(args.min_size)
        common = sorted(set(ua) & set(ub), key=lambda h: ua[h])
        c = collections.Counter()
        with open(os.path.join(args.out_dir, f"pairs_{A.label}_{B.label}.tsv"), "w") as o:
            o.write("a\tb\tsize\ta_name\tb_name\n")
            for h in common:
                a, b = ua[h], ub[h]
                na, nb = A.names.get(a, ("", ""))[0], B.names.get(b, ("", ""))[0]
                c["a_only" if na and not nb else "b_only" if nb and not na else
                  "same" if na and na == nb else "other" if na and nb else "none"] += 1
                o.write(f"{a:08X}\t{b:08X}\t{A.funcs[a]}\t{na}\t{nb}\n")
        summary.write(f"{A.label}\t{B.label}\t{len(common)}\t{c['a_only']}\t{c['b_only']}\t{c['same']}\t{c['other']}\n")
        print(f"{A.label} - {B.label}: {len(common)} functions with the same code; named only in {A.label} {c['a_only']},"
              f" only in {B.label} {c['b_only']}, same name {c['same']}, different names {c['other']}")


if __name__ == "__main__":
    main()
