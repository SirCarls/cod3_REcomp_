#!/usr/bin/env python3
"""bro_mp_names.py - names for Call of Duty 2: Big Red One's multiplayer program, from its campaign.

The campaign (default.xbe) and multiplayer (codmp_xboxf.xbe) programs are built from one engine
source with the same compiler and XDK, so much of the engine and all of the XDK libraries are the
same machine code in both, at other addresses. The campaign has names (dev/bro-decomp/bro_sp/
names.tsv); multiplayer has only its kernel imports. This tool, run on the owner's PC by
`brojobs.py py bro_mp_names.py ...`, carries the names over:

  1. byte pairs: each function's bytes, with the addresses in them masked (the 32-bit
     displacement of a call or jump, and any 32-bit word that falls inside either program), are
     hashed; a hash that exactly one function has on each side pairs the two;
  2. what the byte pairs point at: in two functions with the same masked bytes, the masked words
     line up one for one, so the campaign's call targets and globals pair with the multiplayer
     ones at the same places. A function or global that pairs with one address every time is
     taken (functions only when their own masked bytes match too). Steps 1-2 repeat;
  3. the rest from tools/waw_decomp/match_builds.py, seeded with those pairs and the globals'
     names: strings, named globals and the call graph;
  4. a check: the pairs that strings alone give (no byte pairs, no call graph) against the
     byte pairs.

Usage (paths inside the sandbox; @OUT is brojobs.py's output folder):
    python3 bro_mp_names.py --sp-image <work>/builds/bro_sp/image.pe --sp-raw <work>/builds/bro_sp/raw \\
        --sp-names dev/bro-decomp/bro_sp/names.tsv \\
        --mp-image <work>/builds/bro_mp/image.pe --mp-raw <work>/builds/bro_mp/raw --out @OUT/bro_mp

Writes into --out:
    names.tsv       the multiplayer names table (addr, name, kind, pass, evidence) for
                    `brojobs.py names bro_mp --names <that file>`
    matches_sp.tsv  every pair: mp, sp, sp_name, how (bytes, bytes-ref, name, strings, calls, data), votes
Reads the programs as data only (no disassembler) and needs only Python's standard library.
"""
import argparse
import collections
import csv
import hashlib
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "waw_decomp"))
from bro_iface import Image  # noqa: E402
from match_builds import GENERIC_NAME, Build, Matcher  # noqa: E402

MIN_BYTES = 12        # shorter functions (return 0, empty stubs) are too common to pair by bytes
MAX_BYTES = 0x10000
DATA_SECTIONS = {".rdata", ".data", ".data1", ".xbeh", "XON_RD", ".XTLID", "$$XTIMAGE", "$$XSIMAGE",
                 "DOLBY", "dspimage"}
HOW_TEXT = {"bytes": "the same machine code", "bytes-ref": "called or used at the same place in the same code",
            "name": "the same name", "strings": "the same strings", "calls": "the call graph",
            "data": "the same place in a table of function pointers"}


class Program:
    """One program: its image, its functions (entry -> byte length) and their masked bytes."""

    def __init__(self, image, raw):
        self.img = Image(image)
        self.lo = min(s[1] for s in self.img.secs)
        self.hi = max(s[1] + max(s[2], s[4]) for s in self.img.secs)
        self.code = [(va, va + rs, ro) for name, va, vs, ro, rs in self.img.secs if name not in DATA_SECTIONS]
        self.funcs = {}
        with open(os.path.join(raw, "functions.tsv"), encoding="utf-8") as fh:
            for r in csv.DictReader(fh, delimiter="\t"):
                self.funcs[int(r["entry"], 16)] = int(r["size"])
        entries = sorted(self.funcs)
        for a, b in zip(entries, entries[1:] + [None]):
            n = min(self.funcs[a], (b - a) if b else MAX_BYTES, MAX_BYTES)
            self.funcs[a] = n
        self.sig = {}   # entry -> hash of the masked bytes
        self.refs = {}  # entry -> [masked value, in order]

    def read(self, va, n):
        for lo, hi, ro in self.code:
            if lo <= va < hi:
                return self.img.d[ro + va - lo:ro + min(va + n, hi) - lo]
        return b""

    def mask_all(self, lo, hi):
        """Hash every function, masking 32-bit words in [lo, hi) and call/jump displacements."""
        for a, n in self.funcs.items():
            b = self.read(a, n)
            if len(b) < MIN_BYTES:
                continue
            out, mask, refs = bytearray(), bytearray(), []
            i = 0
            while i < len(b):
                if b[i] in (0xE8, 0xE9) and i + 5 <= len(b):
                    t = (a + i + 5 + struct.unpack_from("<i", b, i + 1)[0]) & 0xFFFFFFFF
                    if self.lo <= t < self.hi:
                        out += bytes([b[i]]) + b"\0\0\0\0"
                        mask += b"\0\1\1\1\1"
                        refs.append(t)
                        i += 5
                        continue
                if i + 4 <= len(b):
                    v = struct.unpack_from("<I", b, i)[0]
                    if lo <= v < hi:
                        out += b"\0\0\0\0"
                        mask += b"\1\1\1\1"
                        refs.append(v)
                        i += 4
                        continue
                out.append(b[i])
                mask.append(0)
                i += 1
            self.sig[a] = hashlib.sha1(bytes(out) + b"|" + bytes(mask)).hexdigest()
            self.refs[a] = refs


def byte_pairs(sp, mp):
    """{sp function: mp function} for masked hashes held once on each side, then what they point
    at, until nothing new is found. Returns (function pairs, how, global pairs)."""
    by_s, by_m = collections.defaultdict(list), collections.defaultdict(list)
    for a, h in sp.sig.items():
        by_s[h].append(a)
    for a, h in mp.sig.items():
        by_m[h].append(a)
    pairs, how = {}, {}
    for h, la in by_s.items():
        lb = by_m.get(h)
        if len(la) == 1 and lb and len(lb) == 1:
            pairs[la[0]] = lb[0]
            how[la[0]] = ("bytes", 1)
    globals_ = {}
    while True:
        votes_s = collections.defaultdict(collections.Counter)
        votes_m = collections.defaultdict(collections.Counter)
        for a, b in pairs.items():
            ra, rb = sp.refs.get(a, []), mp.refs.get(b, [])
            if len(ra) != len(rb):
                continue
            for x, y in zip(ra, rb):
                votes_s[x][y] += 1
                votes_m[y][x] += 1
        taken = set(pairs.values())
        new = 0
        for x, c in votes_s.items():
            if len(c) != 1:
                continue
            (y, n), = c.items()
            if len(votes_m[y]) != 1:
                continue
            if x in sp.funcs and y in mp.funcs:
                if x not in pairs and y not in taken and sp.sig.get(x) and sp.sig.get(x) == mp.sig.get(y):
                    pairs[x] = y
                    how[x] = ("bytes-ref", n)
                    taken.add(y)
                    new += 1
            elif x not in sp.funcs and y not in mp.funcs:
                globals_[x] = (y, n)
        if not new:
            return pairs, how, globals_


def read_sp_names(path, raw):
    """({address: name} of the campaign's functions, of its globals): the names table and the
    names Ghidra already had (kernel imports)."""
    funcs, globs = {}, {}
    with open(os.path.join(raw, "globals.tsv"), encoding="utf-8") as fh:
        for r in csv.DictReader(fh, delimiter="\t"):
            if r["source"] not in ("DEFAULT", "ANALYSIS") and not GENERIC_NAME.match(r["name"]):
                globs[int(r["addr"], 16)] = r["name"]
    if path and os.path.exists(path):
        with open(path, encoding="utf-8") as fh:
            for r in csv.DictReader(fh, delimiter="\t"):
                (funcs if r["kind"] == "function" else globs)[int(r["addr"], 16)] = r["name"]
    return funcs, globs


def named_already(raw):
    """The addresses that already have a name in a program's Ghidra tables (its kernel imports):
    the names table leaves them as they are."""
    out = set()
    for table, col in (("functions.tsv", "entry"), ("globals.tsv", "addr")):
        with open(os.path.join(raw, table), encoding="utf-8") as fh:
            for r in csv.DictReader(fh, delimiter="\t"):
                if r["source"] not in ("DEFAULT", "ANALYSIS") and not GENERIC_NAME.match(r["name"]):
                    out.add(int(r[col], 16))
    return out


def usable(n):
    return bool(n) and not GENERIC_NAME.match(n) and not n.startswith("FID_conflict:")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--sp-image", required=True, help="the campaign's image.pe (xbe2pe.py)")
    ap.add_argument("--sp-raw", required=True, help="the campaign's Ghidra tables (WawExport.java)")
    ap.add_argument("--sp-names", help="the campaign's names table (dev/bro-decomp/bro_sp/names.tsv)")
    ap.add_argument("--mp-image", required=True)
    ap.add_argument("--mp-raw", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)

    sp, mp = Program(a.sp_image, a.sp_raw), Program(a.mp_image, a.mp_raw)
    lo, hi = min(sp.lo, mp.lo), max(sp.hi, mp.hi)
    sp.mask_all(lo, hi)
    mp.mask_all(lo, hi)
    pairs, how, gpairs = byte_pairs(sp, mp)
    c = collections.Counter(h for h, _ in how.values())
    print(f"bro_mp_names: {len(sp.sig)} / {len(mp.sig)} functions hashed; byte pairs: "
          + ", ".join(f"{k} {v}" for k, v in c.most_common()) + f"; {len(gpairs)} globals paired")

    sp_fn, sp_gl = read_sp_names(a.sp_names, a.sp_raw)
    mp_gnames = {y: sp_gl[x] for x, (y, _) in gpairs.items() if usable(sp_gl.get(x))}

    A = Build(a.sp_raw, a.sp_names if a.sp_names and os.path.exists(a.sp_names) else None, image=a.sp_image)
    B = Build(a.mp_raw, global_names=mp_gnames, image=a.mp_image)

    # the check: strings alone, without the byte pairs (nor the global names they gave)
    m0 = Matcher(A, Build(a.mp_raw))
    m0.string_seeds(m0.string_votes())
    agree = sum(1 for x, y in m0.ab.items() if pairs.get(x) == y)
    differ = sum(1 for x, y in m0.ab.items() if x in pairs and pairs[x] != y)
    print(f"bro_mp_names: check: strings alone pair {len(m0.ab)}; {agree + differ} of them are byte pairs too, "
          f"{agree} the same ({100 * agree / max(1, agree + differ):.1f}%), {differ} different")

    m = Matcher(A, B)
    for x in sorted(pairs):
        m.take(x, pairs[x], *how[x])
    m.run(log=lambda s: None)
    c = collections.Counter(h for h, _ in m.how.values())
    print(f"bro_mp_names: {len(m.ab)} of {len(mp.funcs)} multiplayer functions paired: "
          + ", ".join(f"{k} {v}" for k, v in c.most_common()))

    keep = named_already(a.mp_raw)
    rows, n_fn, n_gl = {}, 0, 0
    for x in sorted(m.ab):
        y = m.ab[x]
        n = sp_fn.get(x) or A.names.get(x)
        h, v = m.how[x]
        if usable(n) and y not in keep:
            rows[y] = (n, "function", f"{n} at {x:08X} in the campaign: {HOW_TEXT.get(h, h)} ({v})")
            n_fn += 1
    for x, (y, v) in sorted(gpairs.items()):
        n = sp_gl.get(x)
        if usable(n) and y not in rows and y not in keep:
            rows[y] = (n, "global", f"{n} at {x:08X} in the campaign: used at the same place in the same code ({v})")
            n_gl += 1
    with open(os.path.join(a.out, "names.tsv"), "w", encoding="utf-8") as fh:
        fh.write("addr\tname\tkind\tpass\tevidence\n")
        for y in sorted(rows):
            n, kind, ev = rows[y]
            fh.write(f"{y:08X}\t{n}\t{kind}\tbro-sp\t{ev}\n")
    with open(os.path.join(a.out, "matches_sp.tsv"), "w", encoding="utf-8") as fh:
        fh.write("mp\tsp\tsp_name\thow\tvotes\n")
        for x in sorted(m.ab, key=lambda k: m.ab[k]):
            fh.write(f"{m.ab[x]:08X}\t{x:08X}\t{sp_fn.get(x) or A.names.get(x, '')}\t{m.how[x][0]}\t{m.how[x][1]}\n")
    print(f"bro_mp_names: names.tsv: {n_fn} function names, {n_gl} global names")


if __name__ == "__main__":
    main()
