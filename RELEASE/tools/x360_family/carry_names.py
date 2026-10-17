#!/usr/bin/env python3
"""
carry_names.py - give a rebuild (the Japanese CoD3) the names of the build it was rebuilt from,
through rebuild_diff.py's pairs: names.tsv rows for every paired function whose source has a name,
and a note on changed functions (how alike they are) and on new ones.

Usage:
    python3 tools/x360_family/carry_names.py --pairs pairs.tsv --names names.tsv [--names names_mp.toml] \
        --label USA --out names_rebuild.tsv
"""
import argparse
import bisect
import csv
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))


def read_names(path):
    out = {}
    if path.endswith(".toml"):
        for line in open(path, encoding="utf-8"):
            m = re.match(r'^\s*(0x[0-9A-Fa-f]{8})\s*=\s*"([^"]+)"', line)
            if m:
                out[int(m.group(1), 16)] = (m.group(2), "toml")
        return out
    with open(path, encoding="utf-8", newline="") as f:
        for r in csv.DictReader(f, delimiter="\t", quoting=csv.QUOTE_NONE):
            if r["kind"] == "function" and not re.match(r"^(FUN_|sub_|thunk_)", r["name"]):
                out[int(r["addr"], 16)] = (r["name"], r["pass"])
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--pairs", required=True)
    ap.add_argument("--names", action="append", required=True, help="later files win")
    ap.add_argument("--label", default="source")
    ap.add_argument("--out", required=True)
    ap.add_argument("--a-image", help="with --b-image: also carry names of code that has no .pdata entry"
                                      " (leaf functions), from the paired function before it, instructions checked")
    ap.add_argument("--b-image")
    args = ap.parse_args()
    names = {}
    for p in args.names:
        names.update(read_names(p))
    used, rows = set(), []
    with open(args.pairs, encoding="utf-8", newline="") as f:
        pairs = list(csv.DictReader(f, delimiter="\t"))
    for r in pairs:
        if not r["b"]:
            continue
        b = int(r["b"], 16)
        if r["status"] == "new":
            rows.append((b, "", "note", "rebuild", f"new in this build (no {args.label} counterpart)"))
            continue
        a = int(r["a"], 16)
        n = names.get(a)
        how = "the same code" if r["status"] == "same" else f"changed (instructions {float(r['ratio']):.0%} alike)"
        if n and n[0] not in used:
            used.add(n[0])
            rows.append((b, n[0], "function", "rebuild", f"{args.label} {a:08X} {n[0]} ({n[1]}): {how}"))
        else:  # unnamed (or the name is taken): the comment still says where it came from
            rows.append((b, "", "note", "rebuild", f"{args.label} {a:08X}: {how}"))
    extra = 0
    if args.a_image and args.b_image:
        from build_diff import Image, mask
        A, B = Image(args.a_image), Image(args.b_image)
        same = sorted((int(r["a"], 16), int(r["b"], 16), int(r["a_size"])) for r in pairs
                      if r["status"] == "same" and r["a"] and r["b"])
        starts = [x[0] for x in same]
        paired_a = {int(r["a"], 16) for r in pairs if r["a"]}
        taken_b = {b for b, *_ in rows}
        for a, (n, src) in sorted(names.items()):
            if a in paired_a or n in used:
                continue
            i = bisect.bisect_right(starts, a) - 1
            if i < 0:
                continue
            pa, pb, size = same[i]
            if a < pa + size or (i + 1 < len(same) and same[i + 1][0] < a):
                continue
            b = pb + (a - pa)
            wa, wb = A.words(a, 16), B.words(b, 16)
            if wa is None or wb is None or [mask(w) for w in wa] != [mask(w) for w in wb] or b in taken_b:
                continue
            used.add(n)
            taken_b.add(b)
            extra += 1
            rows.append((b, n, "function", "rebuild",
                         f"{args.label} {a:08X} {n} ({src}): code without a .pdata entry after {pa:08X}, moved with it, 4 instructions checked"))
    with open(args.out, "w", encoding="utf-8") as o:
        o.write("addr\tname\tkind\tpass\tevidence\n")
        for b, n, k, p, e in sorted(rows):
            o.write(f"{b:08X}\t{n}\t{k}\t{p}\t{e}\n")
    print(f"{sum(1 for r in rows if r[2] == 'function')} names carried ({extra} on code without .pdata entries), "
          f"{sum(1 for r in rows if r[2] == 'note')} notes -> {args.out}")


if __name__ == "__main__":
    main()
