#!/usr/bin/env python3
"""
crossref.py - one table of every function across the builds: its name and its address in
each build (PC SP, PC MP, Xbox 360 SP, Xbox 360 MP), from each build's names.tsv and the
match tables between them (name_pc.py --matches, name_x360.py --matches).

Usage:
    python3 tools/waw_decomp/crossref.py --out crossref.tsv \
        --build pc_sp pc_sp/names.tsv --build pc_mp pc_mp/names.tsv \
        --build x360_sp x360_sp/names.tsv --build x360_mp x360_mp/names.tsv \
        --link pc_sp pc_mp pc_sp/other_match.tsv \
        --link x360_sp pc_sp x360_sp/pc_match.tsv --link x360_mp pc_mp x360_mp/pc_match.tsv

A match table's first two columns are the two builds' addresses, in the order given to --link.
Output columns: name, one address column per build (empty where the function wasn't paired),
other_names (names that disagree between builds, as build=name) and conflicts (when the pairs
put two functions of one build in the same row, the others, as build=address: one of the
pairs is wrong). Rows are the functions
that are named in at least one build or paired between at least two, ordered by the first
build's address.
"""
import argparse
import collections
import csv
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from naming_common import read_tsv  # noqa: E402


class Union:
    def __init__(self):
        self.up = {}

    def find(self, x):
        self.up.setdefault(x, x)
        while self.up[x] != x:
            self.up[x] = self.up[self.up[x]]
            x = self.up[x]
        return x

    def join(self, a, b):
        ra, rb = self.find(a), self.find(b)
        if ra != rb:
            self.up[rb] = ra


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build", nargs=2, action="append", metavar=("LABEL", "NAMES"), required=True)
    ap.add_argument("--link", nargs=3, action="append", metavar=("A", "B", "MATCHES"), default=[])
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    labels = [b for b, _ in args.build]
    names = {}
    for label, path in args.build:
        names[label] = {int(r["addr"], 16): r["name"] for r in read_tsv(path) if r["kind"] == "function"}
    u = Union()
    for label in labels:
        for a in names[label]:
            u.find((label, a))
    for la, lb, path in args.link:
        with open(path, newline="", encoding="utf-8") as f:
            rows = csv.reader(f, delimiter="\t")
            next(rows)
            for r in rows:
                u.join((la, int(r[0], 16)), (lb, int(r[1], 16)))
    groups = collections.defaultdict(dict)
    extra = collections.defaultdict(list)
    for node in sorted(u.up):
        label, a = node
        root = u.find(node)
        g = groups[root]
        # two functions of one build in a row: one of the pairs is wrong; keep the first
        if label not in g:
            g[label] = a
        else:
            extra[root].append(f"{label}={a:08X}")
    out = []
    for root, g in groups.items():
        named = [(lb, names[lb][g[lb]]) for lb in labels if lb in g and g[lb] in names[lb]]
        if not named and len(g) < 2:
            continue
        name = named[0][1] if named else ""
        others = sorted({f"{lb}={n}" for lb, n in named if n != name})
        key = next((labels.index(lb), g[lb]) for lb in labels if lb in g)
        out.append((key, [name] + [f"{g[lb]:08X}" if lb in g else "" for lb in labels] + [" ".join(others),
                                                                                          " ".join(extra[root])]))
    out.sort()
    with open(args.out, "w", encoding="utf-8") as o:
        o.write("\t".join(["name"] + labels + ["other_names", "conflicts"]) + "\n")
        for _, row in out:
            o.write("\t".join(row) + "\n")
    full = sum(1 for _, r in out if all(r[1:1 + len(labels)]))
    print(f"{len(out)} functions; {full} found in all {len(labels)} builds; "
          f"{sum(1 for _, r in out if r[0])} named; {sum(1 for _, r in out if r[-1])} with conflicting pairs")


if __name__ == "__main__":
    sys.exit(main())
