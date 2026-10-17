#!/usr/bin/env python3
"""
rebuild_diff.py - what changed between two builds of the same Xbox 360 program when they are NOT
the same set of functions (a later rebuild: the Japanese CoD3 against the USA/Europe one).

The linker keeps the source order, so the two .pdata function lists are lined up as sequences of
"masked code" fingerprints (tools/x360_family/same_code.py's masking: addresses taken out):
equal runs are unchanged functions; inside a run that differs, functions are paired by how alike
their instructions are (difflib ratio on the masked words, at least --min-ratio); what is left is
new (only in B) or gone (only in A). Names come from A's names (symbols.toml-style names.tsv).

Usage:
    python3 tools/x360_family/rebuild_diff.py --a a.pe --b b.pe [--a-names names.tsv] --out pairs.tsv
        [--min-ratio 0.6]

pairs.tsv: a, b, a_size, b_size, status (same / changed / gone / new), ratio, a_name.
"""
import argparse
import collections
import csv
import difflib
import hashlib
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from build_diff import Image  # noqa: E402
from same_code import GENERIC, masked  # noqa: E402


def fingerprints(img):
    out = []
    for a, size in img.functions():
        w = img.words(a, size)
        m = masked(w)
        out.append((a, size, hashlib.sha1(struct.pack(">%dI" % len(m), *m)).digest(), m))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--a", required=True)
    ap.add_argument("--b", required=True)
    ap.add_argument("--a-names")
    ap.add_argument("--out", required=True)
    ap.add_argument("--min-ratio", type=float, default=0.6)
    args = ap.parse_args()
    A, B = fingerprints(Image(args.a)), fingerprints(Image(args.b))
    names = {}
    if args.a_names:
        with open(args.a_names, encoding="utf-8", newline="") as f:
            for r in csv.DictReader(f, delimiter="\t", quoting=csv.QUOTE_NONE):
                if r["kind"] == "function" and not GENERIC.match(r["name"]):
                    names[int(r["addr"], 16)] = r["name"]
    sm = difflib.SequenceMatcher(a=[x[2] for x in A], b=[x[2] for x in B], autojunk=False)
    rows = []
    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        if tag == "equal":
            rows.extend((A[i1 + k], B[j1 + k], "same", 1.0) for k in range(i2 - i1))
            continue
        left, right = list(range(i1, i2)), list(range(j1, j2))
        # pair the most alike first, keeping the order (a pair may not cross an earlier one)
        cands = []
        for i in left:
            for j in right:
                la, lb = A[i][1], B[j][1]
                if min(la, lb) / max(la, lb) < 0.5:
                    continue
                r = difflib.SequenceMatcher(a=A[i][3], b=B[j][3], autojunk=False).ratio()
                if r >= args.min_ratio:
                    cands.append((r, i, j))
        cands.sort(reverse=True)
        used_a, used_b, pairs = set(), set(), []
        for r, i, j in cands:
            if i in used_a or j in used_b:
                continue
            if any((i < pi) != (j < pj) for pi, pj, _ in pairs):
                continue
            used_a.add(i)
            used_b.add(j)
            pairs.append((i, j, r))
        for i, j, r in pairs:
            rows.append((A[i], B[j], "same" if r == 1.0 else "changed", r))
        rows.extend((A[i], None, "gone", 0.0) for i in left if i not in used_a)
        rows.extend((None, B[j], "new", 0.0) for j in right if j not in used_b)
    rows.sort(key=lambda r: (r[0] or r[1])[0] if r[0] else r[1][0] + 0x10000000)
    c = collections.Counter(r[2] for r in rows)
    named = collections.Counter(r[2] for r in rows if r[0] and r[0][0] in names)
    with open(args.out, "w") as o:
        o.write("a\tb\ta_size\tb_size\tstatus\tratio\ta_name\n")
        for a, b, st, r in rows:
            o.write(f"{a[0]:08X}\t" if a else "\t")
            o.write(f"{b[0]:08X}\t" if b else "\t")
            o.write(f"{a[1] if a else ''}\t{b[1] if b else ''}\t{st}\t{r:.3f}\t{names.get(a[0], '') if a else ''}\n")
    print(f"A {len(A)} functions, B {len(B)}: same {c['same']}, changed {c['changed']}, gone {c['gone']}, new {c['new']}"
          f" (named in A: same {named['same']}, changed {named['changed']}, gone {named['gone']})")


if __name__ == "__main__":
    main()
