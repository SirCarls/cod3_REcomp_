#!/usr/bin/env python3
"""
merge_names.py - join two names.tsv files for one build: the first is trusted (for CoD4 on Xbox 360:
the names from KisakCOD, a reconstruction of the same game), the second only fills gaps (names
carried from another game of the family, World at War), and only from its stronger passes.

Kept from the second file: functions the first leaves unnamed, whose name the first doesn't already
use, from any pass except the call-graph ones (`pc-graph`, `kisak-graph`) and except `pc` names
backed by a single shared string (across games that is often a different function: CoD4's
BG_EvaluateTrajectory took World at War's Scr_GetInt from one string). Globals likewise fill gaps.

Usage:
    python3 tools/x360_family/merge_names.py first.tsv second.tsv --out merged.tsv [--tag waw]
"""
import argparse
import csv
import re


def read(path):
    with open(path, encoding="utf-8", newline="") as f:
        return list(csv.DictReader(f, delimiter="\t", quoting=csv.QUOTE_NONE))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("first")
    ap.add_argument("second")
    ap.add_argument("--out", required=True)
    ap.add_argument("--tag", default="other", help="appended to the pass of names taken from the second file")
    args = ap.parse_args()
    first, second = read(args.first), read(args.second)
    have = {(r["addr"], r["kind"]) for r in first}
    used = {r["name"] for r in first}
    out, added = list(first), 0
    for r in second:
        if (r["addr"], r["kind"]) in have or r["name"] in used:
            continue
        if r["kind"] == "note":
            continue
        if r["pass"].endswith("-graph"):
            continue
        if r["pass"] == "pc":
            m = re.match(r"(\d+) string", r["evidence"])
            if not m or int(m.group(1)) < 2:
                continue
        r = dict(r)
        r["pass"] = f"{r['pass']}-{args.tag}"
        out.append(r)
        used.add(r["name"])
        added += 1
    with open(args.out, "w", encoding="utf-8", newline="") as f:
        w = csv.DictWriter(f, fieldnames=["addr", "name", "kind", "pass", "evidence"], delimiter="\t",
                           quoting=csv.QUOTE_NONE, escapechar="\\", extrasaction="ignore")
        w.writeheader()
        for r in sorted(out, key=lambda r: (r["addr"], r["kind"])):
            w.writerow(r)
    print(f"{len(first)} kept, {added} added from {args.second}")


if __name__ == "__main__":
    main()
