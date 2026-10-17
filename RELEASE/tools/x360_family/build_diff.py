#!/usr/bin/env python3
"""
build_diff.py - compare two builds of the same Xbox 360 program (for example the USA/Europe and the
German disc's codmp_xenonf.xex) and carry addresses from one to the other.

Both builds are first turned into plain images with tools/waw_decomp/xex2pe.py. The two programs
must have the same functions in the same order (a rebuild with small changes): the .pdata tables
are walked side by side, so every function's address in build B is known exactly, and a function
whose length changed is lined up instruction by instruction. Data is mapped from the pointers both
builds store at the same place in .data (a pointer that moved tells where its target moved to).

Usage:
    python3 tools/x360_family/build_diff.py --a a.pe --b b.pe [--out-dir DIR]
        [--addresses FILE]   one address per line (0x8.......), optional name after a tab or space,
                             or a names TOML (`name = 0x8...` / `0x8... = "name"`) or names.tsv
Writes (with --out-dir): functions.tsv (every function: a, b, sizes, same/moved/changed),
changed.txt (the changed functions, instructions lined up), addresses.tsv (each --addresses entry
in build B, and how it was found).

Needs capstone only for changed.txt (pip install capstone); everything else is plain Python.
"""
import argparse
import bisect
import collections
import difflib
import os
import re
import struct
import sys


class Image:
    def __init__(self, path, base=0x82000000):
        with open(path, "rb") as f:
            self.img = f.read()
        self.base = base
        pe, = struct.unpack_from("<I", self.img, 0x3C)
        n, = struct.unpack_from("<H", self.img, pe + 6)
        opt, = struct.unpack_from("<H", self.img, pe + 20)
        p = pe + 24 + opt
        self.secs = {}
        for _ in range(n):
            name = self.img[p:p + 8].rstrip(b"\0").decode("latin-1")
            vs, va = struct.unpack_from("<II", self.img, p + 8)
            ch, = struct.unpack_from("<I", self.img, p + 36)
            self.secs[name] = (base + va, vs, ch)
            p += 40

    def u32(self, a):
        o = a - self.base
        return struct.unpack_from(">I", self.img, o)[0] if 0 <= o <= len(self.img) - 4 else None

    def words(self, a, n):
        o = a - self.base
        return list(struct.unpack_from(">%dI" % (n // 4), self.img, o))

    def section_of(self, a):
        for name, (lo, size, ch) in self.secs.items():
            if lo <= a < lo + size:
                return name
        return None

    def functions(self):
        lo, size, _ = self.secs[".pdata"]
        out = []
        for i in range(0, size - 7, 8):
            b, w = struct.unpack_from(">II", self.img, lo - self.base + i)
            if b:
                out.append((b, ((w >> 8) & 0x3FFFFF) * 4))
        return out


def mask(w):
    """An instruction with the parts that hold addresses taken out: branch offsets, and the
    immediates of lis/addi/ori/loads/stores (absolute addresses are built from those)."""
    op = w >> 26
    if op == 18:
        return (op << 26) | (w & 3)
    if op == 16:
        return (w & 0xFFFF0000) | (w & 3)
    if op in (14, 15, 24, 25) or 32 <= op <= 55 or op in (58, 62):
        return w & 0xFFFF0000
    return w


def align_function(A, a0, B, b0):
    """Line up two versions of one function: a list of (a_offset, b_offset) for every
    instruction they share, from difflib on the masked words."""
    sm = difflib.SequenceMatcher(a=[mask(w) for w in A], b=[mask(w) for w in B], autojunk=False)
    pairs, ops = [], []
    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        if tag == "equal":
            pairs.extend((4 * (i1 + k), 4 * (j1 + k)) for k in range(i2 - i1))
        else:
            ops.append((tag, i1, i2, j1, j2))
    return pairs, ops


def data_map(a, b):
    """Where .rdata/.data/.text targets moved, from the pointers both builds keep at the same
    .data offsets (the .data layout itself is checked to be the same)."""
    lo, size, _ = a.secs[".data"]
    lo_b, size_b, _ = b.secs[".data"]
    moves, same, other = [], 0, 0
    if lo != lo_b or size != size_b:
        return None, "the .data sections differ in place or size"
    A = a.words(lo, size - size % 4)
    B = b.words(lo, size - size % 4)
    rd_lo, rd_size, _ = a.secs[".rdata"]
    for x, y in zip(A, B):
        if x == y:
            same += 1
            if rd_lo <= x < rd_lo + rd_size:
                moves.append((x, 0))  # a pointer that stayed: evidence of delta 0
        elif a.base <= x < a.base + len(a.img) and b.base <= y < b.base + len(b.img):
            moves.append((x, y - x))
        else:
            other += 1
    return (same, moves, other), None


def ranges(points):
    """[(addr, delta)] sorted -> [[first, last, delta, count]] runs of the same delta."""
    out = []
    for x, d in sorted(points):
        if out and out[-1][2] == d:
            out[-1][1] = x
            out[-1][3] += 1
        else:
            out.append([x, x, d, 1])
    return out


def read_addresses(path):
    out = []
    txt = open(path, encoding="utf-8", errors="replace").read()
    for line in txt.splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        m = re.match(r'^"?([A-Za-z_][\w:]*)"?\s*=\s*(0x[0-9A-Fa-f]{8})', line)        # name = 0x...
        if m:
            out.append((int(m.group(2), 16), m.group(1)))
            continue
        m = re.match(r'^"?(0x[0-9A-Fa-f]{8})"?\s*=\s*"([^"]+)"', line)               # 0x... = "name"
        if m:
            out.append((int(m.group(1), 16), m.group(2)))
            continue
        m = re.match(r"^(?:0x)?([0-9A-Fa-f]{8})(?:[\s\t]+(\S+))?", line)              # addr [name]
        if m and m.group(1).upper().startswith("8"):
            out.append((int(m.group(1), 16), m.group(2) or ""))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--a", required=True, help="build A's image (xex2pe.py output)")
    ap.add_argument("--b", required=True, help="build B's image")
    ap.add_argument("--out-dir")
    ap.add_argument("--addresses", action="append", default=[])
    args = ap.parse_args()

    a, b = Image(args.a), Image(args.b)
    print("sections (A -> B):")
    for name in a.secs:
        la, sa, _ = a.secs[name]
        lb, sb, _ = b.secs.get(name, (None, None, None))
        flag = "" if (la, sa) == (lb, sb) else "  <- differs"
        print(f"  {name:8} {la:08X}+{sa:#x} -> " + (f"{lb:08X}+{sb:#x}" if lb else "missing") + flag)

    fa, fb = a.functions(), b.functions()
    if len(fa) != len(fb):
        sys.exit(f"error: {len(fa)} functions in A, {len(fb)} in B: not two builds of the same program")
    rows, changed, deltas = [], [], collections.Counter()
    for (pa, la), (pb, lb) in zip(fa, fb):
        if la == lb and [mask(w) for w in a.words(pa, la)] == [mask(w) for w in b.words(pb, lb)]:
            st = "same" if pa == pb else "moved"
        else:
            st = "changed"
            changed.append((pa, la, pb, lb))
        deltas[pb - pa] += 1
        rows.append((pa, pb, la, lb, st))
    print(f"functions: {len(fa)}; same place {sum(r[4] == 'same' for r in rows)}, moved {sum(r[4] == 'moved' for r in rows)},"
          f" changed {len(changed)}")
    print("code moves: " + ", ".join(f"{d:+#x} x{n}" for d, n in sorted(deltas.items())))
    for pa, la, pb, lb in changed:
        print(f"  changed: {pa:08X} ({la} bytes) -> {pb:08X} ({lb} bytes)")

    dm, err = data_map(a, b)
    rd_runs = []
    if err:
        print("data:", err)
    else:
        same, moves, other = dm
        print(f".data: {same} words the same, {sum(1 for _, d in moves if d)} pointers moved, {other} other differences"
              + ("  (the layout is the same: .data addresses carry over unchanged)" if other == 0 else ""))
        rd_lo, rd_size, _ = a.secs[".rdata"]
        rd_runs = ranges([(x, d) for x, d in moves if rd_lo <= x < rd_lo + rd_size])
        for f, l, d, n in rd_runs:
            print(f"  .rdata {f:08X}-{l:08X}: {d:+#x} ({n} pointers)")

    starts = [r[0] for r in rows]

    def carry(x):
        sec = a.section_of(x)
        if sec == ".data" and dm and dm[2] == 0:
            return x, "same .data layout"
        if sec == ".text" or (sec and a.secs[sec][2] & 0x20000000):
            i = bisect.bisect_right(starts, x) - 1
            if i < 0:
                return None, "before the first function"
            pa, pb, la, lb, st = rows[i]
            if x >= pa + la:
                # code with no .pdata entry of its own (a chunk, a thunk): it moved with its
                # neighbours when both agree, checked on the instructions around it
                d = pb - pa
                if i + 1 < len(rows) and rows[i + 1][1] - rows[i + 1][0] != d:
                    return None, f"between {pa:08X} and {rows[i + 1][0]:08X}, which moved differently"
                wa = [mask(w) for w in a.words(x & ~3, 16)]
                wb = [mask(w) for w in b.words((x + d) & ~3, 16)]
                if wa != wb:
                    return None, f"after function {pa:08X}: the instructions there differ"
                return x + d, f"code after function {pa:08X} (no .pdata entry), moved with it ({d:+#x}), 4 instructions checked"
            if st != "changed":
                return pb + (x - pa), f"function {pa:08X} -> {pb:08X} ({st})"
            pairs, _ = align_function(a.words(pa, la), pa, b.words(pb, lb), pb)
            off = dict(pairs).get(x - pa)
            if off is None:
                return None, f"inside changed function {pa:08X}: this instruction has no counterpart"
            return pb + off, f"changed function {pa:08X} -> {pb:08X}, instructions lined up"
        if sec == ".rdata":
            # the runs of moved/unmoved pointers say where blocks of .rdata went; an address
            # between two runs takes the delta of the one whose contents agree there
            prev = nxt = None
            for f, l, d, n in rd_runs:
                if f <= x <= l:
                    return x + d, f".rdata run {f:08X}-{l:08X} ({n} pointers agree)"
                if l < x:
                    prev = (f, l, d, n)
                elif f > x and nxt is None:
                    nxt = (f, l, d, n)

            def score(d):
                wa, wb = a.words(x & ~3, 32), b.words((x + d) & ~3, 32)
                return sum(p == q for p, q in zip(wa, wb)) if wa and wb and any(wa) else 0
            cands = sorted({run[2] for run in (prev, nxt) if run} | {0}, key=score, reverse=True)
            if cands and score(cands[0]) >= 4 and (len(cands) == 1 or score(cands[0]) > score(cands[1])):
                d = cands[0]
                return x + d, f"between .rdata runs; {score(d)} of 8 words the same at {d:+#x}"
            return None, ".rdata: between runs and the contents agree with neither delta"
        return None, f"section {sec}: not mapped"

    if args.out_dir:
        os.makedirs(args.out_dir, exist_ok=True)
        with open(os.path.join(args.out_dir, "functions.tsv"), "w") as o:
            o.write("a\tb\ta_size\tb_size\tstatus\n")
            for pa, pb, la, lb, st in rows:
                o.write(f"{pa:08X}\t{pb:08X}\t{la}\t{lb}\t{st}\n")
        try:
            import capstone
            md = capstone.Cs(capstone.CS_ARCH_PPC, capstone.CS_MODE_32 | capstone.CS_MODE_BIG_ENDIAN)
        except ImportError:
            md = None
        with open(os.path.join(args.out_dir, "changed.txt"), "w") as o:
            for pa, la, pb, lb in changed:
                A, B = a.words(pa, la), b.words(pb, lb)
                _, ops = align_function(A, pa, B, pb)
                o.write(f"== {pa:08X} ({la} bytes) -> {pb:08X} ({lb} bytes)\n")

                def text(img, addr, w):
                    if md is None:
                        return f"{w:08X}"
                    ins = list(md.disasm(struct.pack(">I", w), addr))
                    return f"{ins[0].mnemonic} {ins[0].op_str}" if ins else f".long {w:#010x}"
                for tag, i1, i2, j1, j2 in ops:
                    o.write(f"  {tag}\n")
                    for k in range(i1, i2):
                        o.write(f"    - A {pa + 4 * k:08X}  {text(a, pa + 4 * k, A[k])}\n")
                    for k in range(j1, j2):
                        o.write(f"    + B {pb + 4 * k:08X}  {text(b, pb + 4 * k, B[k])}\n")
    if args.addresses:
        items = []
        for p in args.addresses:
            items.extend(read_addresses(p))
        found = 0
        out = open(os.path.join(args.out_dir, "addresses.tsv"), "w") if args.out_dir else None
        if out:
            out.write("a\tb\tname\thow\n")
        for x, name in items:
            y, how = carry(x)
            found += y is not None
            line = f"{x:08X}\t{y:08X}\t{name}\t{how}" if y is not None else f"{x:08X}\t\t{name}\t{how}"
            if out:
                out.write(line + "\n")
        print(f"addresses: {found} of {len(items)} carried to B")


if __name__ == "__main__":
    main()
