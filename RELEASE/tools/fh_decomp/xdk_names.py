#!/usr/bin/env python3
"""xdk_names.py - names for an original-Xbox program's XDK library functions, from another program built
with the same XDK.

Microsoft's XDK libraries (Direct3D 8, DirectSound, XAPI, XGraphics, the online and network libraries, the
C runtime) are linked into every Xbox game, most of them in sections of their own (D3D, DSOUND, XGRPH, XPP,
XONLINE, XNET, ...). Two games linked with the same XDK version carry much of the same library machine code
at other addresses. Given one program with a names table (A) and one without (B), this tool pairs B's
functions with A's by their bytes, with the addresses in them masked, and writes B's names table.

The bytes are masked the way tools/bro_decomp/bro_mp_names.py masks them (a call's or jump's 32-bit
displacement, and any 32-bit word that falls inside either program). Here the two programs are read
straight from their XBE files, and each side's function starts are its own disassembler's, so a function
may be cut differently on the two sides:

  1. each of A's functions (its trailing padding, int3 and nop, left off) is looked for at every function
     start in B: the same masked bytes for A's whole length. A's function pairs with that start when it is
     found at exactly one start in B and nothing else of A is found there. A function shorter than
     LONG_BYTES (64) pairs only with a start of B of the same length; a longer one may pair with a start
     whose next start comes sooner or later (the two sides cut the function differently);
  2. what the pairs point at: in two pieces of the same masked code the masked words line up one for one,
     so A's call targets and globals pair with B's at the same places. A function that pairs with one
     start every time is taken when its own masked bytes are found there too, and a global that pairs
     with one address every time is named. Step 2 repeats until nothing new is found.

[acct2-bro] 2026-11-08: written for Call of Duty: Finest Hour (B, XDK 1.0.5849) with Call of Duty 2: Big
Red One's campaign (A, the same XDK; its names table is dev/bro-decomp/bro_sp/names.tsv). Reads the
programs as data only (no disassembler, nothing is run) and needs only Python's standard library.

Usage:
    python3 xdk_names.py --a-xbe A.xbe --a-functions A_FUNCS [--a-functions MORE] --a-names A_NAMES.tsv \\
        --b-xbe B.xbe --b-functions B_FUNCS --out OUT_DIR

    A_FUNCS, B_FUNCS  a program's function starts: xboxrecomp's generated recomp_dispatch.c (its table of
                      "{ 0x...u, ... }" rows), a tab-separated table with an "entry" or "addr" column, or
                      one hexadecimal address per line. A's names table's functions are starts of A too.
    A_NAMES.tsv       tab-separated: addr, name, kind ("function", or anything else for a global)

    --names library   (the default) only names of XDK and runtime code are given to B: a pair named after A's
                      own game code is two short pieces of code that only look alike, and goes to dropped.tsv

Writes into OUT_DIR:
    names.tsv    B's names: addr, name, kind, section, how, evidence
    matches.tsv  every function pair, named or not: b, a, a_name, section, how, bytes
    dropped.tsv  pairs left out of names.tsv because the name is A's game code: addr, a_name, section, how, a, bytes
    summary.txt  both programs' XDK library versions; per section of B: function starts, pairs, names;
                 names per library (the part of a name before "::")
"""
import argparse
import collections
import csv
import os
import re
import struct
import sys

MIN_BYTES = 12        # shorter functions (return 0, empty stubs) are too common to pair by their bytes
LONG_BYTES = 64       # a pair whose two sides end differently needs at least this many bytes in common
MAX_BYTES = 0x10000
PAD = (0xCC, 0x90)    # int3 and nop: the padding between functions
# Sections the XBE marks executable that hold only data
DATA_SECTIONS = {".rdata", ".data", ".data1", ".xbeh", "XON_RD", ".XTLID", "$$XTIMAGE", "$$XSIMAGE", "DOLBY",
                 "dspimage", "BINKDATA"}
GENERIC_NAME = re.compile(r"^(FUN_|sub_|LAB_|DAT_|PTR_|thunk_FUN_|switchD|caseD|FID_conflict:|UNK_|s_|u_)")


def u32(d, o):
    return struct.unpack_from("<I", d, o)[0]


class Xbe:
    """An XBE read as data: its sections at their addresses, its XDK library versions."""

    def __init__(self, path):
        with open(path, "rb") as fh:
            self.d = fh.read()
        d = self.d
        if d[:4] != b"XBEH":
            sys.exit(f"xdk_names: {path} is not an XBE (no XBEH at its start)")
        self.path = path
        self.base = u32(d, 0x104)
        count, headers = u32(d, 0x11C), u32(d, 0x120)
        self.secs = []      # (name, va, vsize, raw, rsize, flags)
        for i in range(count):
            o = headers - self.base + i * 0x38
            flags, va, vsize, raw, rsize, name_va = struct.unpack_from("<6I", d, o)
            self.secs.append((self.cstr(name_va), va, vsize, raw, rsize, flags))
        self.lo = self.base
        self.hi = max(s[1] + s[2] for s in self.secs)
        self.code = [s for s in self.secs if s[5] & 4 and s[0] not in DATA_SECTIONS]
        self.libs = []
        n, at = u32(d, 0x160), u32(d, 0x164)
        for i in range(n):
            o = at - self.base + i * 16
            name = d[o:o + 8].split(b"\0")[0].decode("ascii", "replace")
            major, minor, build, flags = struct.unpack_from("<4H", d, o + 8)
            self.libs.append(f"{name} {major}.{minor}.{build} (flags {flags:#06x})")

    def cstr(self, va):
        o = va - self.base
        if not 0 <= o < len(self.d):
            return "?"
        end = self.d.find(b"\0", o, o + 64)
        return self.d[o:end if end >= 0 else o + 64].decode("ascii", "replace")

    def section(self, va):
        """The code section holding va, or None."""
        for s in self.code:
            if s[1] <= va < s[1] + s[2]:
                return s
        return None

    def end_of(self, s):
        """Where the section's bytes in the file end (past that, the Xbox fills it with zeros)."""
        return s[1] + min(s[2], s[4])

    def read(self, va, n):
        s = self.section(va)
        if not s:
            return b""
        end = min(va + n, self.end_of(s))
        if end <= va:
            return b""
        o = s[3] + va - s[1]
        return self.d[o:o + end - va]


class Program:
    """One program: its XBE, its function starts, each start's length (to the next start, padding left off)."""

    def __init__(self, xbe_path, starts):
        self.x = Xbe(xbe_path)
        self.starts = sorted(a for a in set(starts) if self.x.section(a))
        self.startset = set(self.starts)
        self.length = {}
        for i, a in enumerate(self.starts):
            s = self.x.section(a)
            nxt = self.starts[i + 1] if i + 1 < len(self.starts) else None
            end = self.x.end_of(s)
            if nxt is not None and nxt < end:
                end = nxt
            b = self.x.read(a, min(end - a, MAX_BYTES))
            n = len(b)
            while n and b[n - 1] in PAD:
                n -= 1
            self.length[a] = n
        self.lo, self.hi = self.x.lo, self.x.hi

    def masked(self, va, n, lo, hi):
        """(the n bytes at va with the addresses in them masked, the masked values in order as (kind, value)),
        or (None, None) when fewer than n bytes are there."""
        b = self.x.read(va, n)
        if len(b) < n:
            return None, None
        out, mask, refs = bytearray(b), bytearray(n), []
        i = 0
        while i < n:
            if b[i] in (0xE8, 0xE9) and i + 5 <= n:
                t = (va + i + 5 + struct.unpack_from("<i", b, i + 1)[0]) & 0xFFFFFFFF
                if self.lo <= t < self.hi:
                    out[i + 1:i + 5] = b"\0\0\0\0"
                    mask[i + 1:i + 5] = b"\1\1\1\1"
                    refs.append(("call", t))
                    i += 5
                    continue
            if i + 4 <= n:
                v = struct.unpack_from("<I", b, i)[0]
                if lo <= v < hi:
                    out[i:i + 4] = b"\0\0\0\0"
                    mask[i:i + 4] = b"\1\1\1\1"
                    refs.append(("word", v))
                    i += 4
                    continue
            i += 1
        return bytes(out) + b"|" + bytes(mask), refs


def read_starts(path):
    """Function starts from recomp_dispatch.c, a table with an entry/addr column, or a list of addresses."""
    with open(path, encoding="utf-8", errors="replace") as fh:
        text = fh.read()
    if path.lower().endswith(".c"):
        return {int(m.group(1), 16) for m in re.finditer(r"\{\s*0x([0-9A-Fa-f]+)u?\s*,", text)}
    rows = [r for r in text.splitlines() if r.strip() and not r.startswith("#")]
    if rows and "\t" in rows[0]:
        head = rows[0].split("\t")
        for want in ("entry", "addr", "address"):
            if want in head:
                col = head.index(want)
                out = set()
                for r in rows[1:]:
                    f = r.split("\t")
                    if len(f) > col:
                        try:
                            out.add(int(f[col], 16))
                        except ValueError:
                            pass
                return out
    out = set()
    for r in rows:
        try:
            out.add(int(r.split()[0], 16))
        except ValueError:
            pass
    return out


def read_names(path):
    """({address: name} of A's functions, of A's globals), generic names left out."""
    funcs, globs = {}, {}
    csv.field_size_limit(1 << 24)
    with open(path, encoding="utf-8", errors="replace", newline="") as fh:
        for r in csv.DictReader(fh, delimiter="\t", quoting=csv.QUOTE_NONE):
            try:
                a = int(r.get("addr") or "", 16)
            except ValueError:
                continue
            n = (r.get("name") or "").strip()
            if not n or GENERIC_NAME.match(n):
                continue
            (funcs if (r.get("kind") or "function") == "function" else globs)[a] = n
    return funcs, globs


def pair_by_bytes(A, B, lo, hi):
    """Step 1: {a: (b, how)} for A's functions found at exactly one start of B, alone there.

    A function shorter than LONG_BYTES is looked up whole among B's starts of the same length; a longer one
    by its first LONG_BYTES bytes among all of B's starts, then compared for its whole length."""
    short, long_ = {}, collections.defaultdict(list)
    for b in B.starts:
        n = B.length[b]
        if MIN_BYTES <= n < LONG_BYTES:
            k, _ = B.masked(b, n, lo, hi)
            if k is not None:
                short.setdefault(k, []).append(b)
        k, _ = B.masked(b, LONG_BYTES, lo, hi)
        if k is not None:
            long_[k].append(b)
    found, at_b = {}, collections.defaultdict(list)
    for a in A.starts:
        n = A.length[a]
        if n < MIN_BYTES:
            continue
        if n < LONG_BYTES:
            k, _ = A.masked(a, n, lo, hi)
            hits = list(short.get(k, ()))
        else:
            k, _ = A.masked(a, LONG_BYTES, lo, hi)
            cands = long_.get(k)
            if not cands:
                continue
            full, _ = A.masked(a, n, lo, hi)
            hits = [b for b in cands if B.masked(b, n, lo, hi)[0] == full]
        if hits:
            found[a] = hits
            for b in hits:
                at_b[b].append(a)
    pairs = {}
    for a, hits in found.items():
        if len(hits) != 1 or len(at_b[hits[0]]) != 1:
            continue
        b = hits[0]
        pairs[a] = (b, "bytes" if B.length.get(b) == A.length[a] else "bytes, cut differently")
    return pairs


def pair_by_refs(A, B, pairs, lo, hi):
    """Step 2: what the pairs call and use. Adds function pairs to `pairs`; returns {a global: (b global,
    votes)}."""
    while True:
        votes_a = collections.defaultdict(collections.Counter)
        votes_b = collections.defaultdict(collections.Counter)
        for a, (b, _) in pairs.items():
            n = A.length[a]
            _, ra = A.masked(a, n, lo, hi)
            _, rb = B.masked(b, n, lo, hi)
            if ra is None or rb is None or len(ra) != len(rb):
                continue
            for (ka, x), (kb, y) in zip(ra, rb):
                if ka != kb:
                    continue
                votes_a[x][y] += 1
                votes_b[y][x] += 1
        taken = {b for b, _ in pairs.values()}
        new = 0
        globals_ = {}
        for x, c in votes_a.items():
            if len(c) != 1:
                continue
            (y, v), = c.items()
            if len(votes_b[y]) != 1:
                continue
            if x in A.startset and y in B.startset:
                if x in pairs or y in taken:
                    continue
                n = A.length[x]
                if n == 0:
                    continue
                ma, _ = A.masked(x, n, lo, hi)
                mb, _ = B.masked(y, n, lo, hi)
                if ma is not None and ma == mb:
                    pairs[x] = (y, f"used at the same place in paired code ({v})")
                    taken.add(y)
                    new += 1
            elif x not in A.startset and y not in B.startset:
                globals_[x] = (y, v)
        if not new:
            return globals_


def library(name):
    return name.split("::")[0] if "::" in name else "(no library prefix)"


def is_library_name(name):
    """A name of XDK or C/C++ runtime code: "LIB::Function", a C or decorated C++ name ("_memmove",
    "?_UnwindNestedFrames@@...", "sscanf"), the C++ library under A's names ("std_...", "eh_..."), or the startup
    code ("entry"). A's own game code has none of these, and a game's code can't be the other game's, so a pair
    whose name is none of these is two short pieces of code that only look alike."""
    return ("::" in name or name[:1] in ("_", "?") or name.startswith(("std_", "eh_")) or name == "entry"
            or re.fullmatch(r"[a-z][a-z0-9_]*", name) is not None)     # the C library's own names: srand, sscanf


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--a-xbe", required=True, help="the program with names")
    ap.add_argument("--a-functions", required=True, action="append", help="its function starts (repeatable)")
    ap.add_argument("--a-names", required=True, help="its names table (addr, name, kind)")
    ap.add_argument("--b-xbe", required=True, help="the program to name")
    ap.add_argument("--b-functions", required=True, action="append", help="its function starts (repeatable)")
    ap.add_argument("--out", required=True, help="the folder to write names.tsv, matches.tsv, summary.txt into")
    ap.add_argument("--names", choices=("library", "all"), default="library",
                    help="library (the default): only names of XDK and runtime code (is_library_name); all: every "
                         "paired name")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)

    a_funcs, a_globs = read_names(a.a_names)
    a_starts = set(a_funcs)
    for p in a.a_functions:
        a_starts |= read_starts(p)
    b_starts = set()
    for p in a.b_functions:
        b_starts |= read_starts(p)
    A, B = Program(a.a_xbe, a_starts), Program(a.b_xbe, b_starts)
    lo, hi = min(A.lo, B.lo), max(A.hi, B.hi)
    print(f"xdk_names: A {len(A.starts)} function starts in code, B {len(B.starts)}")

    pairs = pair_by_bytes(A, B, lo, hi)
    n_bytes = len(pairs)
    globals_ = pair_by_refs(A, B, pairs, lo, hi)
    print(f"xdk_names: {n_bytes} pairs by their bytes, {len(pairs) - n_bytes} more by what they call; "
          f"{len(globals_)} globals paired")

    rows, dropped = {}, []
    for x, (y, how) in sorted(pairs.items(), key=lambda kv: kv[1][0]):
        n = a_funcs.get(x)
        if not n:
            continue
        s = B.x.section(y)
        if a.names == "library" and not is_library_name(n):
            dropped.append((y, n, s[0] if s else "?", how, x, A.length[x]))
            continue
        rows[y] = (n, "function", s[0] if s else "?", how, f"{n} at {x:08X} in A ({A.length[x]} bytes)")
    for x, (y, v) in sorted(globals_.items(), key=lambda kv: kv[1][0]):
        n = a_globs.get(x) or a_funcs.get(x)
        if n and y not in rows and (a.names == "all" or is_library_name(n)):
            rows[y] = (n, "global", "", f"used at the same place in paired code ({v})", f"{n} at {x:08X} in A")
    with open(os.path.join(a.out, "dropped.tsv"), "w", encoding="utf-8", newline="\n") as fh:
        fh.write("addr\ta_name\tsection\thow\ta\tbytes\n")
        for y, n, sec, how, x, ln in dropped:
            fh.write(f"{y:08X}\t{n}\t{sec}\t{how}\t{x:08X}\t{ln}\n")
    print(f"xdk_names: {len(dropped)} pairs whose name is A's own game code left out (dropped.tsv)")
    with open(os.path.join(a.out, "names.tsv"), "w", encoding="utf-8", newline="\n") as fh:
        fh.write("addr\tname\tkind\tsection\thow\tevidence\n")
        for y in sorted(rows):
            fh.write(f"{y:08X}\t" + "\t".join(rows[y]) + "\n")
    with open(os.path.join(a.out, "matches.tsv"), "w", encoding="utf-8", newline="\n") as fh:
        fh.write("b\ta\ta_name\tsection\thow\tbytes\n")
        for x, (y, how) in sorted(pairs.items(), key=lambda kv: kv[1][0]):
            s = B.x.section(y)
            fh.write(f"{y:08X}\t{x:08X}\t{a_funcs.get(x, '')}\t{s[0] if s else '?'}\t{how}\t{A.length[x]}\n")

    lines = [f"A {A.x.path}", "  " + "; ".join(A.x.libs), f"B {B.x.path}", "  " + "; ".join(B.x.libs), "",
             "B's code sections: function starts, paired, named"]
    paired_b = collections.Counter()
    named_b = collections.Counter()
    for x, (y, _) in pairs.items():
        s = B.x.section(y)
        paired_b[s[0] if s else "?"] += 1
        if x in a_funcs:
            named_b[s[0] if s else "?"] += 1
    starts_b = collections.Counter()
    for y in B.starts:
        starts_b[B.x.section(y)[0]] += 1
    seen = []
    for s in B.x.code:
        if s[0] not in seen:
            seen.append(s[0])
    for name in seen:
        lines.append(f"  {name:10s} {starts_b[name]:6d} {paired_b[name]:6d} {named_b[name]:6d}")
    lines.append(f"  {'all':10s} {len(B.starts):6d} {sum(paired_b.values()):6d} {sum(named_b.values()):6d}")
    lines += ["", "B's function names per library"]
    for lib, n in collections.Counter(library(r[0]) for r in rows.values() if r[1] == "function").most_common():
        lines.append(f"  {lib:24s} {n:6d}")
    lines.append(f"  globals named: {sum(1 for r in rows.values() if r[1] == 'global')}")
    with open(os.path.join(a.out, "summary.txt"), "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(lines) + "\n")
    print("\n".join(lines))


if __name__ == "__main__":
    main()
