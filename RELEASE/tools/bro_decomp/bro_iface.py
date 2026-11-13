#!/usr/bin/env python3
"""bro_iface.py - Call of Duty 2: Big Red One's level interface against Call of Duty 3's.

Big Red One's missions call the engine through a table of function pointers that the engine
fills at start-up: several functions load a pointer global (0x0095F49C in the campaign) and
store function addresses at offsets from it (`mov reg, [global]` then `mov [reg+off], func`).
Call of Duty 3's level DLLs use a table that grew from it (G_InitLevelInterface, 1,004 slots).
This tool, run on the owner's PC by `brojobs.py py bro_iface.py ...`:

  1. finds Big Red One's table and its filled slots in the program (image.pe from xbe2pe.py);
  2. counts the calls through each slot (`call dword ptr [slot]`) per section;
  3. pairs the program's functions with Call of Duty 3's campaign functions
     (tools/waw_decomp/match_builds.py: strings, named globals, the call graph);
  4. lines the two tables up: slots whose functions are paired, or that have the same name on
     both sides, are anchors (the longest chain of them in order on both sides);
  5. inside each gap between two anchors, compares every slot's function on one side with every
     slot's function on the other: what they call (through the functions paired in step 3, two
     calls deep) and the strings and named globals they use, each weighted by how rare it is
     among the slots' functions. The best order-keeping set of pairs is taken where each pair is
     clearly the best for both slots; those split the gap, and the smaller gaps are compared
     again. A gap that ends up with the same number of slots on both sides pairs in order,
     unless two slots that would pair use enough to compare and share nothing (a builtin CoD3
     dropped where it added another); the rest are left open with the range of Call of Duty 3
     slots they fall in;
  6. a check: each anchor is left out in turn, its gap is placed again by step 5, and the
     result is compared with the anchor (counted apart for same-name anchors, whose evidence
     step 5 doesn't use, and function-pair anchors).

Usage (paths inside the sandbox; @OUT is brojobs.py's output folder):
    python3 bro_iface.py --image <work>/builds/bro_sp/image.pe --raw <work>/builds/bro_sp/raw \\
        --names dev/bro-decomp/bro_sp/names.tsv --cod3 dev/waw-decomp/cod3 \\
        --cod3-iface dev/bro-decomp/cod3_level_iface.tsv --out @OUT/bro_sp

Writes level_iface.tsv, matches_cod3.tsv and iface_map.tsv into --out. Reads the program as
data only (byte patterns, no disassembler needed) and needs only Python's standard library.
"""
import argparse
import collections
import csv
import math
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "waw_decomp"))
from match_builds import Build, Matcher  # noqa: E402

# mov reg, [abs32]: A1 (eax) or 8B /r with mod=00 rm=101
LOAD_REG = {0x05: "eax", 0x0D: "ecx", 0x15: "edx", 0x1D: "ebx", 0x35: "esi", 0x3D: "edi"}
REG_RM = {0: "eax", 1: "ecx", 2: "edx", 3: "ebx", 6: "esi", 7: "edi"}


class Image:
    """A PE file from xbe2pe.py: sections at their addresses, read through their raw data."""

    def __init__(self, path):
        self.d = d = open(path, "rb").read()
        pe = struct.unpack_from("<I", d, 0x3C)[0]
        nsec = struct.unpack_from("<H", d, pe + 6)[0]
        opt = struct.unpack_from("<H", d, pe + 20)[0]
        base = struct.unpack_from("<I", d, pe + 24 + 28)[0]
        so = pe + 24 + opt
        self.secs = []
        for i in range(nsec):
            name = d[so + 40 * i:so + 40 * i + 8].rstrip(b"\0").decode("latin-1")
            vs, va, rs, ro = struct.unpack_from("<IIII", d, so + 40 * i + 8)
            self.secs.append((name, base + va, vs, ro, rs))

    def section(self, name):
        for s in self.secs:
            if s[0] == name:
                return s
        raise KeyError(name)

    def section_of(self, va):
        for name, sva, vs, ro, rs in self.secs:
            if sva <= va < sva + max(vs, rs):
                return name
        return None

    def data(self, name):
        n, va, vs, ro, rs = self.section(name)
        return va, self.d[ro:ro + rs]


def find_interface(img):
    """(pointer global, {offset: function}) of the engine table: the global most often loaded
    into a register right before a store of a .text address through that register."""
    tva, tb = img.data(".text")
    tlo, thi = tva, tva + len(tb)
    stores = []  # (instruction address, register, offset, value)
    loads = []   # (instruction address, register, global)
    for i in range(len(tb) - 10):
        b = tb[i]
        if b == 0xA1:
            loads.append((tva + i, "eax", struct.unpack_from("<I", tb, i + 1)[0]))
        elif b == 0x8B and tb[i + 1] in LOAD_REG:
            loads.append((tva + i, LOAD_REG[tb[i + 1]], struct.unpack_from("<I", tb, i + 2)[0]))
        elif b == 0xC7:
            m = tb[i + 1]
            mod, reg, rm = m >> 6, (m >> 3) & 7, m & 7
            if reg != 0 or rm not in REG_RM:
                continue
            if mod == 2:
                off, val = struct.unpack_from("<II", tb, i + 2)
            elif mod == 1:
                off, val = tb[i + 2], struct.unpack_from("<I", tb, i + 3)[0]
            elif mod == 0 and rm != 5:
                off, val = 0, struct.unpack_from("<I", tb, i + 2)[0]
            else:
                continue
            if tlo <= val < thi:
                stores.append((tva + i, REG_RM[rm], off, val))
    # pair each store with the closest load of its register before it (within 32 bytes)
    by_reg = collections.defaultdict(list)
    for a, r, g in loads:
        by_reg[r].append((a, g))
    votes = collections.Counter()
    paired = []
    import bisect
    keys = {r: [a for a, _ in v] for r, v in by_reg.items()}
    for a, r, off, val in stores:
        ks = keys.get(r)
        if not ks:
            continue
        j = bisect.bisect_left(ks, a) - 1
        if j >= 0 and a - ks[j] <= 32:
            g = by_reg[r][j][1]
            votes[g] += 1
            paired.append((g, off, val))
    if not votes:
        sys.exit("bro_iface: no interface filler found")
    g = votes.most_common(1)[0][0]
    slots = {}
    for gg, off, val in paired:
        if gg == g:
            slots[off] = val
    return g, slots


def table_calls(img, slots):
    """The table's address (from the calls through it) and the calls per slot and section."""
    calls = collections.defaultdict(collections.Counter)
    for name, sva, vs, ro, rs in img.secs:
        if name in (".rdata", ".data", ".data1", ".xbeh", "XON_RD", ".XTLID", "$$XTIMAGE", "$$XSIMAGE",
                    "DOLBY", "dspimage"):
            continue
        b = img.d[ro:ro + rs]
        i = b.find(b"\xff\x15")
        while i >= 0:
            if i + 6 <= len(b):
                calls[struct.unpack_from("<I", b, i + 2)[0]][name] += 1
            i = b.find(b"\xff\x15", i + 1)
    # the base: the address that makes the most distinct called slots land on filled offsets
    # (counted once each: weighting by calls lets a few busy slots pull the base off)
    cand = collections.Counter()
    for s in calls:
        if img.section_of(s) in (".data", None):
            for off in slots:
                if s - off >= 0:
                    cand[s - off] += 1
    best = max(cand.values())
    base = min(b for b, n in cand.items() if n == best)
    return base, {off: calls.get(base + off, collections.Counter()) for off in slots}


def read_names(path):
    funcs, slot_names = {}, {}
    if not path or not os.path.exists(path):
        return funcs, slot_names
    with open(path, encoding="utf-8") as fh:
        next(fh)
        for line in fh:
            c = line.rstrip("\n").split("\t")
            if len(c) < 3:
                continue
            a = int(c[0], 16)
            if c[2] == "function":
                funcs[a] = c[1]
            elif c[1].startswith("pScr_"):
                slot_names[a] = c[1][5:]
    return funcs, slot_names


def key(n):
    n = re.sub(r"^(LIBCMT::|LIBCMT__|_)", "", n or "")
    n = re.sub(r"_thunk$", "", n)
    n = re.sub(r"^(Broc_|Scr_|GScr_)", "", n)
    return n.lower()


# a gap's pair is taken when its score is at least MIN_SCORE, rests on at least MIN_SHARED
# shared things, and beats every other score in its row and column by MARGIN
MIN_SCORE, MIN_SHARED, MARGIN = 0.2, 2, 1.25
FAN_OUT = 50  # a callee with more callers than this is too common to stand for anything two calls deep


class Scorer:
    """How alike a Big Red One slot function and a Call of Duty 3 slot function are: the paired
    functions they reach (their callees and their callees' callees) and the strings
    and named globals they and their callees use, each weighted by how rare it is among all the
    slot functions of both tables (log(N / count))."""

    def __init__(self, A, B, pairs, bro_fns, c3_fns):
        self.A, self.B = A, B
        self.ab = {x: y for x, (y, _) in pairs.items()}
        self.paired_b = set(self.ab.values())
        self.fa = {f: self._feats(A, f, self.ab.get, lambda x: x in self.ab) for f in set(bro_fns)}
        self.fb = {g: self._feats(B, g, lambda y: y, lambda y: y in self.paired_b) for g in set(c3_fns)}
        df = collections.Counter()
        for fs in list(self.fa.values()) + list(self.fb.values()):
            df.update(fs)
        n = len(self.fa) + len(self.fb)
        self.w = {k: math.log(n / c) for k, c in df.items()}

    @staticmethod
    def _feats(build, f, to_b, is_paired):
        d1 = set(build.callees.get(f, ()))
        d2 = set()
        for x in d1:
            if len(build.callers.get(x, ())) <= FAN_OUT:
                d2 |= build.callees.get(x, set())
        # the function's own pair is left out: a paired slot function is an anchor already, and
        # the check below places anchors again without it
        feats = {("c", to_b(x)) for x in (d1 | d2) - {f} if is_paired(x)}
        for x in {f} | d1:
            feats |= {("s", t) for t in build.toks.get(x, ())}
        return frozenset(feats)

    def score(self, f, g):
        a, b = self.fa.get(f, frozenset()), self.fb.get(g, frozenset())
        shared = a & b
        if not shared:
            return 0.0, 0
        total = sum(self.w[k] for k in a | b)
        return (sum(self.w[k] for k in shared) / total if total else 0.0), len(shared)

    def differ(self, f, g):
        """True when both functions use enough that the comparison can tell, and they share
        (almost) nothing: then two slots at the same place in a gap are not the same builtin."""
        a, b = self.fa.get(f, frozenset()), self.fb.get(g, frozenset())
        return len(a) >= 3 and len(b) >= 3 and self.score(f, g)[0] < 0.05

    def best_pairs(self, gb, gc, bro, c3):
        """[(bro offset, cod3 offset, score, shared)] in order: the order-keeping set of pairs
        with the most total score, keeping only pairs clearly best in their row and column."""
        S = {}
        for i, o in enumerate(gb):
            for j, co in enumerate(gc):
                s, k = self.score(bro[o], c3[co])
                if s > 0:
                    S[i, j] = (s, k)
        if not S:
            return []
        row_best, col_best = collections.defaultdict(list), collections.defaultdict(list)
        for (i, j), (s, _) in S.items():
            row_best[i].append(s)
            col_best[j].append(s)

        def clear(i, j):
            s, k = S[i, j]
            if s < MIN_SCORE or k < MIN_SHARED:
                return False
            others_r = sorted(row_best[i], reverse=True)
            others_c = sorted(col_best[j], reverse=True)
            second_r = others_r[1] if len(others_r) > 1 else 0.0
            second_c = others_c[1] if len(others_c) > 1 else 0.0
            return s == others_r[0] and s == others_c[0] and s >= MARGIN * second_r and s >= MARGIN * second_c

        ok = {ij: S[ij] for ij in S if clear(*ij)}
        if not ok:
            return []
        # the order-keeping subset with the most total score (the pairs are few: a plain O(n^2) chain)
        items = sorted(ok.items())
        best = [v[0] for _, v in items]
        prev = [-1] * len(items)
        for x in range(len(items)):
            for y in range(x):
                if items[y][0][0] < items[x][0][0] and items[y][0][1] < items[x][0][1] \
                        and best[y] + items[x][1][0] > best[x]:
                    best[x], prev[x] = best[y] + items[x][1][0], y
        x = max(range(len(items)), key=lambda t: best[t])
        out = []
        while x >= 0:
            (i, j), (s, k) = items[x]
            out.append((gb[i], gc[j], s, k))
            x = prev[x]
        return out[::-1]


def place(gb, gc, c0, c1, bro, c3, scorer):
    """{bro offset: (cod3 offset or None, how)} for the slots of one gap: gb and gc are the slot
    offsets strictly between two anchors, c0 and c1 the anchors' CoD3 offsets."""
    rows = {}
    found = scorer.best_pairs(gb, gc, bro, c3) if scorer and gb and gc else []
    if found:
        lo_b, lo_c = -1, c0
        for o, co, s, k in found + [(None, c1, 0, 0)]:
            sub_b = [x for x in gb if x > lo_b and (o is None or x < o)]
            sub_c = [y for y in gc if lo_c < y < co]
            rows.update(place(sub_b, sub_c, lo_c, co, bro, c3, scorer))
            if o is not None:
                rows[o] = (co, f"callees and strings ({s:.2f}, {k} shared)")
                lo_b, lo_c = o, co
        return rows
    differ = bool(scorer) and any(scorer.differ(bro[o], c3[co]) for o, co in zip(gb, gc))
    if gb and len(gb) == len(gc) and not differ:
        for o, co in zip(gb, gc):
            rows[o] = (co, "order between anchors")
    else:
        why = "; in order, their functions differ" if gb and len(gb) == len(gc) else ""
        for o in gb:
            rows[o] = (None, f"open: CoD3 0x{max(c0, 0):X}-0x{c1:X} ({len(gc)} slots there, {len(gb)} here{why})")
    return rows


def align(bro, bname, c3, cname, pairs, scorer=None):
    """({bro offset: (cod3 offset or None, how)}, the anchors left out of the chain, the
    leave-one-out check as a Counter of (anchor kind: name / pair, result: same / elsewhere /
    open)). The check is strictest on the same-name anchors: their names are evidence the
    comparison doesn't use, while a function pair's callees were partly paired through it."""
    c3_by_fn = collections.defaultdict(list)
    for o, f in c3.items():
        c3_by_fn[f].append(o)
    anchors = {}
    for o, f in bro.items():
        if f in pairs:
            cs = c3_by_fn.get(pairs[f][0], [])
            if len(cs) == 1:
                anchors[o] = (cs[0], "function pair (" + pairs[f][1] + ")")
    bk, ck = collections.defaultdict(list), collections.defaultdict(list)
    for o, n in bname.items():
        if n:
            bk[key(n)].append(o)
    for o, n in cname.items():
        if n and not n.startswith("FUN_"):
            ck[key(n)].append(o)
    for k in set(bk) & set(ck):
        if len(bk[k]) == 1 and len(ck[k]) == 1 and bk[k][0] not in anchors:
            anchors[bk[k][0]] = (ck[k][0], "same name")
    items = sorted(anchors.items())
    best, prev = [1] * len(items), [-1] * len(items)
    for i in range(len(items)):
        for j in range(i):
            if items[j][1][0] < items[i][1][0] and best[j] + 1 > best[i]:
                best[i], prev[i] = best[j] + 1, j
    chain = []
    i = max(range(len(items)), key=lambda x: best[x]) if items else -1
    while i >= 0:
        chain.append(items[i])
        i = prev[i]
    chain.reverse()
    rows = {o: a for o, a in chain}
    bs, cs = sorted(bro), sorted(c3)
    bounds = [(-4, -4)] + [(o, co) for o, (co, _) in chain] + [(max(bs) + 4, max(cs) + 4)]

    def gap(b0, c0, b1, c1):
        return [o for o in bs if b0 < o < b1], [o for o in cs if c0 < o < c1]

    for (b0, c0), (b1, c1) in zip(bounds, bounds[1:]):
        gb, gc = gap(b0, c0, b1, c1)
        rows.update(place(gb, gc, c0, c1, bro, c3, scorer))
    check = collections.Counter()
    for n in range(1, len(bounds) - 1):
        (b0, c0), (o, co), (b1, c1) = bounds[n - 1], bounds[n], bounds[n + 1]
        gb, gc = gap(b0, c0, b1, c1)
        got = place(gb, gc, c0, c1, bro, c3, scorer).get(o, (None, ""))[0]
        kind = "name" if rows[o][1] == "same name" else "pair"
        check[kind, "same" if got == co else "open" if got is None else "elsewhere"] += 1
    return rows, sorted(set(anchors) - {o for o, _ in chain}), check


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--image", required=True)
    ap.add_argument("--raw", required=True, help="Ghidra's tables of the program (WawExport.java)")
    ap.add_argument("--names", help="its names table (dev/bro-decomp/<build>/names.tsv)")
    ap.add_argument("--cod3", required=True, help="Call of Duty 3's tables and names (dev/waw-decomp/cod3)")
    ap.add_argument("--cod3-iface", required=True, help="Call of Duty 3's table (dev/bro-decomp/cod3_level_iface.tsv)")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)

    img = Image(a.image)
    g, bro = find_interface(img)
    base, calls = table_calls(img, bro)
    funcs, slot_names = read_names(a.names)
    bname = {o: funcs.get(f) or slot_names.get(base + o, "") for o, f in bro.items()}
    with open(os.path.join(a.out, "level_iface.tsv"), "w", encoding="utf-8") as fh:
        fh.write(f"# pointer global 0x{g:08X}, table 0x{base:08X}\n")
        fh.write("offset\tslot\tfunction\tname\tcall_sites\tcalled_from\n")
        for o in sorted(bro):
            c = calls[o]
            fh.write(f"0x{o:04X}\t0x{base + o:08X}\t0x{bro[o]:08X}\t{bname[o]}\t{sum(c.values())}\t"
                     + ",".join(f"{k}:{v}" for k, v in c.most_common()) + "\n")
    print(f"bro_iface: pointer global 0x{g:08X}, table 0x{base:08X}, {len(bro)} filled slots, "
          f"{sum(sum(c.values()) for c in calls.values())} calls through them")

    c3 = {}
    with open(a.cod3_iface, encoding="utf-8") as fh:
        for line in fh:
            if line.startswith(("#", "offset")):
                continue
            o, f = line.split()[:2]
            c3[int(o, 16)] = int(f, 16)
    cnames = {}
    with open(os.path.join(a.cod3, "functions.tsv"), encoding="utf-8") as fh:
        for r in csv.DictReader(fh, delimiter="\t"):
            cnames[int(r["entry"], 16)] = r["name"]

    A = Build(a.raw, a.names if a.names and os.path.exists(a.names) else None)
    B = Build(a.cod3, os.path.join(a.cod3, "names.tsv"))
    m = Matcher(A, B)
    m.run(log=lambda s: None)
    pairs = {x: (m.ab[x], m.how[x][0]) for x in m.ab}
    with open(os.path.join(a.out, "matches_cod3.tsv"), "w", encoding="utf-8") as fh:
        fh.write("bro\tcod3\tbro_name\tcod3_name\thow\tvotes\n")
        for x in sorted(m.ab):
            fh.write(f"{x:08X}\t{m.ab[x]:08X}\t{A.names.get(x, '')}\t{B.names.get(m.ab[x], '')}\t"
                     f"{m.how[x][0]}\t{m.how[x][1]}\n")
    print(f"bro_iface: {len(pairs)} functions paired with Call of Duty 3's")

    cname = {o: cnames.get(f, "") for o, f in c3.items()}
    scorer = Scorer(A, B, pairs, bro.values(), c3.values())
    rows, dropped, check = align(bro, bname, c3, cname, pairs, scorer)
    kinds = collections.Counter()
    sites = collections.Counter()
    with open(os.path.join(a.out, "iface_map.tsv"), "w", encoding="utf-8") as fh:
        fh.write("bro_offset\tbro_function\tbro_name\tcall_sites\tcod3_offset\tcod3_function\tcod3_name\tevidence\n")
        for o in sorted(bro):
            co, how = rows.get(o, (None, ""))
            kind = how.split(" (")[0].split(":")[0]
            kinds[kind] += 1
            sites[kind] += sum(calls[o].values())
            fh.write("\t".join([f"0x{o:04X}", f"0x{bro[o]:08X}", bname[o], str(sum(calls[o].values())),
                                f"0x{co:04X}" if co is not None else "",
                                f"0x{c3[co]:08X}" if co is not None else "",
                                cname.get(co, "") if co is not None else "", how]) + "\n")
    total = sum(sites.values()) or 1
    print("bro_iface: slots " + ", ".join(f"{k} {v}" for k, v in kinds.most_common())
          + "; call sites " + ", ".join(f"{k} {100 * v / total:.0f}%" for k, v in sites.most_common())
          + f"; {len(dropped)} anchors out of order left out")
    for kind, label in (("name", "same-name"), ("pair", "function-pair")):
        n = sum(v for (k, _), v in check.items() if k == kind)
        print(f"bro_iface: check: {n} {label} anchors left out one at a time and placed again: "
              f"{check[kind, 'same']} at the same CoD3 slot, {check[kind, 'elsewhere']} elsewhere, "
              f"{check[kind, 'open']} left open")


if __name__ == "__main__":
    main()
