#!/usr/bin/env python3
"""
carry_addresses.py - find, for addresses known in one Xbox 360 build, the same places in another
build of the same game (Call of Duty 3's campaign default.xex -> its multiplayer codmp_xenonf.xex,
or World at War SP -> MP). Functions come from a match_builds.py pair table; addresses inside a
function and global variables come from lining up the instructions of each paired function.

How globals are found: PowerPC code forms an address in two instructions (`lis r, hi` then
`addi`/`ori`/a load or store with the low half). In two builds of the same source a paired
function usually has the same instructions, only with other address halves. The two
instruction lists are aligned (with the address halves and branch offsets masked); wherever both
sides form an address at aligned instructions, that is one vote that the first build's address
is the second build's. Every paired function votes; the answer is the address most votes agree
on.

Usage:
    python3 tools/waw_decomp/carry_addresses.py \
        --a-image <a image.pe> --a-raw <a WawExport dir> --b-image <b image.pe> --b-raw <b WawExport dir> \
        --pairs <match_builds.py table> [--pairs-reversed] \
        (--want <tsv: name, addr> | --scan-src <source dir> [--names-toml <names.toml>]
         [--skip-file <regex>] [--skip-line <regex>]) \
        --out <tsv> [--globals-out <tsv>]

--pairs: a table whose a_addr column is build A and b_addr build B; --pairs-reversed when it is
the other way round. Functions the table misses are paired when their code (address halves
masked) is the same and unique in both builds. --scan-src collects every 0x82xxxxxx/0x83xxxxxx
constant in .h/.cpp files (named by the identifier on its line), every `sub_XXXXXXXX` and every
REX_HOOK_RAW/REX_EXTERN name (through --names-toml, the codegen's address -> name table);
--skip-file and --skip-line leave out files and lines that are about the other build.

Output columns: name, a_addr, kind (function, in-function, global, string, vtable, unknown),
b_addr, how, evidence. `how`: pair (the pair table, or "same code"), calls (aligned calls reach
it), aligned (instruction alignment inside a paired function), votes (globals: votes for this
address / all votes for it), string (the same string, once in B), rtti (the vtable of the same
class, by its RTTI name; its slots may differ), near (a global next to a voted one, at the same
distance: check).
"""
import argparse
import bisect
import collections
import csv
import difflib
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from name_x360 import Image  # noqa: E402
from naming_common import read_tsv  # noqa: E402

LOADS = {32, 33, 34, 35, 40, 41, 42, 43, 46, 48, 49, 50, 51}   # lwz lwzu lbz lbzu lhz lhzu lha lhau lmw lfs lfsu lfd lfdu
STORES = {36, 37, 38, 39, 44, 45, 47, 52, 53, 54, 55}          # stw stwu stb stbu sth sthu stmw stfs stfsu stfd stfdu
DS_FORM = {58, 62}                                             # ld/lwa, std
X_STORES = {151, 183, 215, 247, 407, 439, 149, 181, 663, 695, 727, 759, 918, 662, 150, 214}
WRITES_RA = {20, 21, 23, 24, 25, 26, 27, 28, 29, 30}           # rlwimi rlwinm rlwnm ori oris xori xoris andi. andis. rld*


def sext16(v):
    return v - 0x10000 if v & 0x8000 else v


def decode(img, start, size):
    """One (masked word, address formed or None, call target or None) per instruction."""
    hi, full = {}, {}
    out = []
    for i in range(size // 4):
        a = start + 4 * i
        w = img.u32(a)
        if w is None:
            break
        op, rd, ra, imm = w >> 26, (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
        masked, ea, call = w, None, None
        if op == 15 and ra == 0:                       # lis rd, hi
            hi[rd] = (imm << 16) & 0xFFFFFFFF
            full.pop(rd, None)
            masked = w & 0xFFFF0000
        elif op == 14 and (ra in hi or ra in full):    # addi rd, ra, lo
            ea = ((hi[ra] if ra in hi else full[ra]) + sext16(imm)) & 0xFFFFFFFF
            hi.pop(rd, None)
            full[rd] = ea
            masked = w & 0xFFFF0000
        elif op == 24 and rd in hi:                    # ori ra, rs, lo  (rs = rd field)
            ea = hi[rd] | imm
            hi.pop(ra, None)
            full[ra] = ea
            masked = w & 0xFFFF0000
        elif op in LOADS or op in STORES or op in DS_FORM:
            off = sext16(imm & 0xFFFC) if op in DS_FORM else sext16(imm)
            if ra in hi:
                ea = (hi[ra] + off) & 0xFFFFFFFF
                masked = w & 0xFFFF0000
            elif ra in full:
                ea = (full[ra] + off) & 0xFFFFFFFF
            if op in LOADS or (op == 58):
                hi.pop(rd, None)
                full.pop(rd, None)
        elif op == 18:                                 # b / bl
            li = w & 0x03FFFFFC
            if li & 0x02000000:
                li -= 0x04000000
            target = (li if w & 2 else a + li) & 0xFFFFFFFF
            if w & 1:
                call = target
            masked = w & 0xFC000003
        elif op == 16:                                 # bc: mask the displacement
            masked = w & 0xFFFF0003
        else:
            # Anything else that may write a register forgets what that register held. X-form
            # (opcode 31) and logical/rotate instructions write rA, the others rD; when unsure,
            # forget both (both builds are read the same way, so this only costs votes).
            if op == 31:
                if ((w >> 1) & 0x3FF) not in X_STORES:
                    for r in (rd, ra):
                        hi.pop(r, None)
                        full.pop(r, None)
            elif op in WRITES_RA:
                hi.pop(ra, None)
                full.pop(ra, None)
            elif op not in (10, 11, 19):              # compares and branch-unit ops write no GPR
                hi.pop(rd, None)
                full.pop(rd, None)
        out.append((masked, ea, call))
    return out


def functions(raw):
    out = {}
    for r in read_tsv(os.path.join(raw, "functions.tsv")):
        out[int(r["entry"], 16)] = int(r["size"])
    return out


def containing(starts, sizes, a):
    """The function whose body holds address a, from a sorted list of starts."""
    i = bisect.bisect_right(starts, a) - 1
    if i >= 0 and a < starts[i] + sizes[starts[i]]:
        return starts[i]
    return None


def cstring(img, a, maxlen=256):
    """The zero-terminated string at a (printable, 2+ characters), or None."""
    o = a - img.base
    if not 0 <= o < len(img.img):
        return None
    end = img.img.find(b"\0", o, o + maxlen)
    if end < 0:
        return None
    text = img.img[o:end]
    if len(text) < 2 or any(not (32 <= c < 127 or c in (9, 10, 13)) for c in text):
        return None
    return text


def rtti_class(img, vtable):
    """MSVC RTTI: the word before a vtable points to its complete object locator, whose fourth word
    points to the type descriptor; its name (".?AVName@@") starts 8 bytes in."""
    col = img.u32(vtable - 4)
    if col is None or not img.is_data(col):
        return None
    td = img.u32(col + 12)
    if td is None or not img.is_data(td):
        return None
    name = cstring(img, td + 8)
    return name if name and name.startswith(b".?A") else None


class Finder:
    """Find strings and RTTI vtables in an image's data."""

    def __init__(self, img):
        self.img = img
        self.data = []
        for n, lo, hi, ch in img.secs:
            if not ch & 0x20000000:
                o = lo - img.base
                self.data.append((lo, img.img[o:o + (hi - lo)]))

    def string(self, text):
        hits = []
        for lo, blob in self.data:
            i = blob.find(text + b"\0")
            while i >= 0:
                if i == 0 or blob[i - 1] == 0:
                    hits.append(lo + i)
                i = blob.find(text + b"\0", i + 1)
        return hits

    def words(self, value):
        needle = value.to_bytes(4, "big")
        hits = []
        for lo, blob in self.data:
            i = blob.find(needle)
            while i >= 0:
                if i % 4 == 0:
                    hits.append(lo + i)
                i = blob.find(needle, i + 1)
        return hits

    def vtables(self, class_name):
        out = []
        for td_name in self.string(class_name):
            td = td_name - 8
            for ref in self.words(td):
                col = ref - 12
                out += [w + 4 for w in self.words(col)]
        return out


def scan_sources(src, names_toml, skip_file=None, skip_line=None):
    by_name = {}
    if names_toml:
        for line in open(names_toml, encoding="utf-8"):
            m = re.match(r'\s*0x([0-9A-Fa-f]+)\s*=\s*"([^"]+)"', line)
            if m:
                by_name[m.group(2)] = int(m.group(1), 16)
    want = {}
    for root, _, files in os.walk(src):
        for fn in sorted(files):
            if not fn.endswith((".h", ".cpp", ".inc")):
                continue
            rel = os.path.relpath(os.path.join(root, fn), src)
            if skip_file and re.search(skip_file, fn):
                continue
            for line in open(os.path.join(root, fn), encoding="utf-8", errors="replace"):
                if skip_line and re.search(skip_line, line):
                    continue   # an address of the other build, mentioned next to this one
                for m in re.finditer(r"\b(k\w+|\w+)\s*=\s*0x(8[23][0-9A-Fa-f]{6})\b", line):
                    want.setdefault(int(m.group(2), 16), f"{m.group(1)} ({rel})")
                for m in re.finditer(r"\bsub_(8[23][0-9A-Fa-f]{6})\b", line):
                    want.setdefault(int(m.group(1), 16), f"sub_{m.group(1).upper()} ({rel})")
                for m in re.finditer(r"REX_(?:HOOK_RAW|EXTERN)\((?:__imp__)?(\w+)\)", line):
                    n = m.group(1)
                    a = by_name.get(n)
                    if a is None and n.startswith("sub_"):
                        a = int(n[4:], 16)
                    if a is not None:
                        want.setdefault(a, f"{n} ({rel})")
                for m in re.finditer(r"0x(8[23][0-9A-Fa-f]{6})\b", line):
                    want.setdefault(int(m.group(1), 16), f"0x{m.group(1).upper()} ({rel})")
    return want


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--a-image", required=True)
    ap.add_argument("--a-raw", required=True)
    ap.add_argument("--b-image", required=True)
    ap.add_argument("--b-raw", required=True)
    ap.add_argument("--pairs", required=True)
    ap.add_argument("--pairs-reversed", action="store_true")
    ap.add_argument("--want")
    ap.add_argument("--scan-src")
    ap.add_argument("--names-toml")
    ap.add_argument("--skip-file", help="regex: source files to leave out (the other build's own code)")
    ap.add_argument("--skip-line", help="regex: source lines to leave out (they name the other build's addresses)")
    ap.add_argument("--out", required=True)
    ap.add_argument("--globals-out")
    args = ap.parse_args()

    A, B = Image(args.a_image, 0x82000000), Image(args.b_image, 0x82000000)
    fa, fb = functions(args.a_raw), functions(args.b_raw)
    pairs = {}
    with open(args.pairs, newline="", encoding="utf-8") as f:
        for r in csv.DictReader(f, delimiter="\t"):
            x, y = int(r["a_addr"], 16), int(r["b_addr"], 16)
            if args.pairs_reversed:
                x, y = y, x
            pairs[x] = (y, r.get("how", ""), r.get("votes", ""))

    # functions the pair table misses but whose code is the same (addresses masked), once per build
    paired_b = {y for y, _, _ in pairs.values()}
    def code_key(img, f, size):
        return tuple(d[0] for d in decode(img, f, size))
    keys_b = collections.defaultdict(list)
    for y, size in fb.items():
        if y not in paired_b and size >= 16:
            keys_b[code_key(B, y, size)].append(y)
    keys_a = collections.defaultdict(list)
    for x, size in fa.items():
        if x not in pairs and size >= 16:
            keys_a[code_key(A, x, size)].append(x)
    for k, xs in keys_a.items():
        ys = keys_b.get(k, [])
        if len(xs) == 1 and len(ys) == 1:
            pairs[xs[0]] = (ys[0], "same code", "")

    gvotes = collections.defaultdict(collections.Counter)
    cvotes = collections.defaultdict(collections.Counter)
    align = {}       # a function -> (list of (i, j) aligned instruction indices, share of equal words)
    for x, (y, _, _) in pairs.items():
        if x not in fa or y not in fb:
            continue
        da, db = decode(A, x, fa[x]), decode(B, y, fb[y])
        sm = difflib.SequenceMatcher(None, [d[0] for d in da], [d[0] for d in db], autojunk=False)
        idx = []
        for blk in sm.get_matching_blocks():
            for k in range(blk.size):
                i, j = blk.a + k, blk.b + k
                idx.append((i, j))
                if da[i][1] is not None and db[j][1] is not None:
                    gvotes[da[i][1]][db[j][1]] += 1
                if da[i][2] is not None and db[j][2] is not None:
                    cvotes[da[i][2]][db[j][2]] += 1
        align[x] = (dict(idx), len(idx) / max(1, len(da), len(db)))

    gmap = {}
    for ga, c in gvotes.items():
        gb, n = c.most_common(1)[0]
        total = sum(c.values())
        if n >= 2 and n * 2 > total or n == total:
            gmap[ga] = (gb, n, total)
    if args.globals_out:
        with open(args.globals_out, "w", encoding="utf-8") as o:
            o.write("a_addr\tb_addr\tvotes\tall_votes\n")
            for ga in sorted(gmap):
                gb, n, total = gmap[ga]
                o.write(f"{ga:08X}\t{gb:08X}\t{n}\t{total}\n")

    want = {}
    if args.want:
        for r in read_tsv(args.want):
            want[int(r["addr"], 16)] = r["name"]
    if args.scan_src:
        want.update(scan_sources(args.scan_src, args.names_toml, args.skip_file, args.skip_line))

    fin_b = Finder(B)
    starts = sorted(fa)
    voted = sorted(gmap)
    rows = []
    stats = collections.Counter()
    for a in sorted(want):
        name = want[a]
        if a in fa:
            kind = "function"
            if a in pairs:
                b, how, votes = pairs[a]
                same = align.get(a, ({}, 0))[1]
                cv = cvotes.get(a)
                ev = f"pair ({how}, {votes} votes); {round(100 * same)}% of the instructions line up"
                if cv:
                    cb, cn = cv.most_common(1)[0]
                    ev += f"; calls to it line up {cn} times" + ("" if cb == b else f" (with {cb:08X}!)")
                rows.append((name, a, kind, b, "pair", ev))
            elif a in cvotes:
                b, n = cvotes[a].most_common(1)[0]
                rows.append((name, a, kind, b, "calls", f"not in the pair table; {n} aligned calls reach it"))
            else:
                rows.append((name, a, kind, None, "", "no pair"))
        elif A.is_code(a):
            kind = "in-function"
            f = containing(starts, fa, a)
            if f is not None and f in pairs and f in align:
                i = (a - f) // 4
                j = align[f][0].get(i)
                if j is not None:
                    b = pairs[f][0] + 4 * j
                    rows.append((name, a, kind, b, "aligned",
                                 f"+0x{a - f:X} in {f:08X} -> +0x{4 * j:X} in {pairs[f][0]:08X}; "
                                 f"{round(100 * align[f][1])}% of the function lines up"))
                else:
                    rows.append((name, a, kind, None, "", f"+0x{a - f:X} in {f:08X}: that instruction doesn't line up"))
            else:
                rows.append((name, a, kind, None, "", "its function has no pair"))
        else:
            kind = "global"
            text = cstring(A, a) if A.is_data(a) else None
            cls = rtti_class(A, a) if A.is_data(a) else None
            if a in gmap:
                b, n, total = gmap[a]
                rows.append((name, a, kind, b, "votes", f"{n} of {total} aligned uses"))
            elif cls and len(fin_b.vtables(cls)) == 1:
                rows.append((name, a, "vtable", fin_b.vtables(cls)[0], "rtti",
                             f"the vtable of {cls.decode()} (RTTI); its slots may differ between the builds"))
            elif text and len(fin_b.string(text)) == 1:
                rows.append((name, a, "string", fin_b.string(text)[0], "string", f"the same string: {text.decode()[:60]!r}"))
            else:
                k = bisect.bisect_right(voted, a) - 1
                if k >= 0 and a - voted[k] < 0x400 and gmap[voted[k]][1] >= 3:
                    base = voted[k]
                    b = gmap[base][0] + (a - base)
                    rows.append((name, a, kind, b, "near",
                                 f"0x{a - base:X} after {base:08X} (-> {gmap[base][0]:08X}, {gmap[base][1]} votes): check"))
                else:
                    rows.append((name, a, kind if A.is_data(a) else "unknown", None, "", "no aligned use"))
        stats[(rows[-1][2], bool(rows[-1][3] is not None))] += 1
    with open(args.out, "w", encoding="utf-8") as o:
        o.write("name\ta_addr\tkind\tb_addr\thow\tevidence\n")
        for name, a, kind, b, how, ev in rows:
            o.write(f"{name}\t{a:08X}\t{kind}\t{'' if b is None else f'{b:08X}'}\t{how}\t{ev}\n")
    print(f"{len(gmap)} globals carried; " + ", ".join(f"{k} {'found' if f else 'not found'}: {v}"
                                                   for (k, f), v in sorted(stats.items())))


if __name__ == "__main__":
    sys.exit(main())
