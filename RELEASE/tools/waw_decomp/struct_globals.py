#!/usr/bin/env python3
"""
struct_globals.py - find where a game structure lives in an executable, and check a table of typed
globals against each other.

The code reads and writes a global structure's fields at fixed addresses (base + field offset), so
every address the code references votes for "base = address - offset" for each field offset of the
structure. The true base collects votes from most of its fields; a base one field away usually
collects fewer, because the fields are not evenly spaced. Field offsets come from a C header whose
members carry "//OFS: 0x.. SIZE: 0x.." comments (the style of the T4SP server plugin's headers,
turned into one file by t4sp_types.py; private use only).

Usage:
    python3 tools/waw_decomp/struct_globals.py vote --header <types.h> --raw <export dir> \\
        (--struct NAME ... | --all) [--lo 0x900000] [--hi 0x6000000] [--min-hits 10] [--min-margin 0.15]
    python3 tools/waw_decomp/struct_globals.py check --header <types.h> --table <globals.tsv> \\
        [--names <names.tsv>]

vote: for each structure, the best bases, how many of its fields start at a referenced address
there, and the best count one or two fields away ("next door"). --all lists every structure whose
best base has at least --min-hits fields and beats next door by --min-margin (a share of its hits).
A high count alone means little in dense data (pointer tables match any structure of ints); the
margin and the functions that use the fields (WawExport globals.tsv) decide.

check: a table (addr, kind, name, type, evidence; type "S" or "S[N]") of globals typed with the
header's structures: each one's extent (from the last field's offset and size) must not overlap
another's, the names table (--names) must give its address the same name, and no other named
global may sit inside it, except labels of its own fields ("level.time" inside "level").

Needs: nothing outside the standard library.
"""
import argparse
import bisect
import collections
import csv
import re
import sys

FIELD_RE = re.compile(r"//OFS: 0x([0-9A-Fa-f]+) SIZE: 0x([0-9A-Fa-f]+)")
STRUCT_RE = re.compile(r"\n\s*(struct|union) (?:__declspec\(align\(\d+\)\) )?(\w+)\s*\n\s*\{(.*?)\n\s*\};", re.S)


def read_tsv(path):
    with open(path, encoding="utf-8", newline="") as f:
        return list(csv.DictReader(f, delimiter="\t"))


def struct_fields(header):
    """{struct name: [(offset, size), ...]} for the top-level members of every structure. A union's
    members all get offset 0 (the header numbers them as if they followed each other)."""
    with open(header, encoding="utf-8", errors="replace") as f:
        text = f.read()
    out = {}
    for m in STRUCT_RE.finditer(text):
        union = m.group(1) == "union"
        depth, fields = 0, []
        for line in m.group(3).split("\n"):
            if depth == 0:
                fm = FIELD_RE.search(line)
                if fm:
                    fields.append((0 if union else int(fm.group(1), 16), int(fm.group(2), 16)))
            depth += line.count("{") - line.count("}")
        if fields:
            out[m.group(2)] = fields
    return out


def extent(fields):
    """Size up to the end of the last member, rounded up to 4 bytes."""
    end = max(o + s for o, s in fields)
    return (end + 3) & ~3


def vote(fields, refs):
    offsets = sorted(set(o for o, _ in fields))
    votes = collections.Counter()
    for a in refs:
        for o in offsets:
            if o > a:
                break
            votes[a - o] += 1
    ranked = []
    for base, n in votes.most_common(5):
        # "next door": the same structure one or two fields earlier or later
        near = [base + d for d in (-8, -4, 4, 8)]
        ranked.append((base, n, max(votes[b] for b in near)))
    return len(offsets), ranked


def cmd_vote(args):
    structs = struct_fields(args.header)
    lo, hi = int(args.lo, 16), int(args.hi, 16)
    refs = sorted(a for a in (int(r["addr"], 16) for r in read_tsv(f"{args.raw}/globals.tsv")) if lo <= a < hi)
    names = args.struct or sorted(structs)
    for name in names:
        if name not in structs:
            print(f"{name}: not in the header")
            continue
        fields = structs[name]
        n, ranked = vote(fields, refs)
        if not ranked:
            continue
        base, hits, nextdoor = ranked[0]
        if args.all and (n < 8 or hits < args.min_hits or hits - nextdoor < args.min_margin * hits):
            continue
        print(f"{name}: {n} fields, extent 0x{extent(fields):X}")
        for base, hits, nextdoor in ranked[:1 if args.all else 5]:
            print(f"  base {base:08X}: {hits} of {n} fields at referenced addresses (next door {nextdoor})")


def parse_type(spec):
    m = re.match(r"^\s*(\w+)\s*(?:\[(\d+)\])?\s*$", spec)
    return (m.group(1), int(m.group(2) or 1)) if m else (None, 0)


def cmd_check(args):
    structs = struct_fields(args.header)
    rows = read_tsv(args.table)
    spans, bad = [], 0
    for r in rows:
        t, count = parse_type(r["type"])
        if t not in structs:
            continue
        a = int(r["addr"], 16)
        spans.append((a, a + extent(structs[t]) * count, r["name"], r["type"]))
    spans.sort()
    for (a1, e1, n1, t1), (a2, e2, n2, t2) in zip(spans, spans[1:]):
        if a2 < e1:
            print(f"overlap: {n1} {t1} {a1:08X}-{e1:08X} and {n2} {t2} at {a2:08X}")
            bad += 1
    if args.names:
        named = sorted((int(r["addr"], 16), r["name"]) for r in read_tsv(args.names) if r["kind"] == "global")
        keys = [a for a, _ in named]
        at = dict(named)
        for a, e, n, t in spans:
            if at.get(a, n) != n:
                print(f"{n} {t} at {a:08X}: the names table calls this address {at[a]}")
                bad += 1
            for i in range(bisect.bisect_right(keys, a), bisect.bisect_left(keys, e)):
                other = named[i][1]
                if not other.startswith(n + "."):
                    print(f"inside {n} {t} {a:08X}-{e:08X}: {other} at {named[i][0]:08X}")
                    bad += 1
    print(f"{len(spans)} typed globals checked, {bad} problems")
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    v = sub.add_parser("vote")
    v.add_argument("--header", required=True)
    v.add_argument("--raw", required=True)
    v.add_argument("--struct", nargs="+")
    v.add_argument("--all", action="store_true")
    v.add_argument("--lo", default="0x900000", help="lowest address to count (default: past .rdata)")
    v.add_argument("--hi", default="0x6000000")
    v.add_argument("--min-hits", type=int, default=10)
    v.add_argument("--min-margin", type=float, default=0.15)
    c = sub.add_parser("check")
    c.add_argument("--header", required=True)
    c.add_argument("--table", required=True)
    c.add_argument("--names")
    args = ap.parse_args()
    if args.cmd == "vote":
        cmd_vote(args)
        return 0
    return cmd_check(args)


if __name__ == "__main__":
    sys.exit(main())
