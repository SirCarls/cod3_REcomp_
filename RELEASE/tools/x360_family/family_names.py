#!/usr/bin/env python3
"""
family_names.py - name candidates for one build (Call of Duty 3) from the functions other builds
share with it as identical code (same_code.py's pairs).

For every function of the target that has a twin with the same machine code in another build:
  - the names the twins carry there, and how many builds agree on one name;
  - a candidate when the target's own symbol map has no name yet;
  - a disagreement when it has one and the twins agree on another (one of them is wrong, or the
    target names the function in its own style: Cmd_X_f against Scr_X).
Names are compared without case, leading underscores and a trailing _2/_3.

Usage:
    python3 tools/x360_family/family_names.py --pairs-dir DIR --target cod3_sp \
        --target-names names.tsv [--target-names names_mp.toml] --out candidates.tsv

Only pairs files named pairs_<A>_<B>.tsv that include the target are read.
"""
import argparse
import collections
import csv
import glob
import os
import re


def norm(n):
    n = re.sub(r"^_+", "", n)
    n = re.sub(r"_\d+$", "", n)
    return n.lower()


def read_names(path):
    out = {}
    if path.endswith(".toml"):
        for line in open(path, encoding="utf-8"):
            m = re.match(r'^\s*(0x[0-9A-Fa-f]{8})\s*=\s*"([^"]+)"', line) or None
            if m:
                out[int(m.group(1), 16)] = m.group(2)
                continue
            m = re.match(r'^\s*"?([A-Za-z_][\w:]*)"?\s*=\s*(0x[0-9A-Fa-f]{8})', line)
            if m:
                out[int(m.group(2), 16)] = m.group(1)
        return out
    with open(path, encoding="utf-8", newline="") as f:
        for r in csv.DictReader(f, delimiter="\t", quoting=csv.QUOTE_NONE):
            if r["kind"] == "function":
                out[int(r["addr"], 16)] = r["name"]
    return out


GENERIC = re.compile(r"^(FUN_|thunk_|LAB_|DAT_|switch|table_|Dvar_Register_[0-9A-F]{8}$|sub_[0-9A-F]{8}$|vfunc)")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--pairs-dir", required=True)
    ap.add_argument("--target", required=True)
    ap.add_argument("--target-names", action="append", default=[])
    ap.add_argument("--out", required=True)
    args = ap.parse_args()
    own = {}
    for p in args.target_names:
        own.update({a: n for a, n in read_names(p).items() if not GENERIC.match(n)})
    twins = collections.defaultdict(list)  # target addr -> [(build, addr, name)]
    for path in sorted(glob.glob(os.path.join(args.pairs_dir, "pairs_*.tsv"))):
        base = os.path.basename(path)[6:-4]
        with open(path, encoding="utf-8", newline="") as f:
            rows = list(csv.DictReader(f, delimiter="\t", quoting=csv.QUOTE_NONE))
        # the labels are joined by "_" and contain "_" themselves: find the target's side
        if base.startswith(args.target + "_"):
            other, side = base[len(args.target) + 1:], "a"
        elif base.endswith("_" + args.target):
            other, side = base[:-len(args.target) - 1], "b"
        else:
            continue
        for r in rows:
            t = int(r[side], 16)
            o = r["b" if side == "a" else "a"]
            name = r["b_name" if side == "a" else "a_name"]
            twins[t].append((other, o, name))
    stats = collections.Counter()
    with open(args.out, "w") as out:
        out.write("addr\town_name\tcandidate\tagreeing\tstatus\ttwins\n")
        for t in sorted(twins):
            tw = twins[t]
            votes = collections.defaultdict(set)
            spelled = {}
            for b, o, n in tw:
                # the target's own other program (CoD3 MP for the campaign) got many of its names
                # from the target: not independent evidence
                if b.split("_")[0] == args.target.split("_")[0]:
                    continue
                if n and not GENERIC.match(n):
                    k = norm(n)
                    votes[k].add(b.split("_")[0])  # one vote per game (SP and MP of a game are not independent)
                    spelled.setdefault(k, n)
            best = max(votes.items(), key=lambda kv: len(kv[1]), default=(None, set()))
            cand = spelled.get(best[0], "")
            agree = len(best[1])
            mine = own.get(t, "")
            if not cand:
                status = "no-name"
            elif not mine:
                status = "candidate"
            elif norm(mine) == best[0]:
                status = "agrees"
            else:
                status = "differs"
            stats[status] += 1
            if status == "candidate":
                stats[f"candidate_{min(agree, 3)}"] += 1
            desc = "; ".join(f"{b}:{o}{'=' + n if n else ''}" for b, o, n in tw)
            out.write(f"{t:08X}\t{mine}\t{cand}\t{agree}\t{status}\t{desc}\n")
    print(f"{args.target}: {len(twins)} functions with an identical twin elsewhere; "
          f"own name agrees {stats['agrees']}, differs {stats['differs']}; candidates {stats['candidate']} "
          f"(1 game {stats['candidate_1']}, 2 games {stats['candidate_2']}, 3+ {stats['candidate_3']}); "
          f"twins without names {stats['no-name']}")


if __name__ == "__main__":
    main()
