#!/usr/bin/env python3
"""fh_stand_in_spec.py - the list of Finest Hour's library functions that get phase 1 stand-ins, in the form
tools/bro_decomp/ngl_stubs.py reads.

Finest Hour's Direct3D (the D3D8 library) and DirectSound (DSOUND) are in sections of their own in its XBE. In
phase 1 of the port every function in those sections is a stand-in that logs its call and returns (as Big Red
One's port does for its graphics library and the Direct3D and DirectSound calls outside it), so the recompiled
game runs with no GPU and no sound chip emulated. ngl_stubs.py then reads each function's instructions for how
many argument bytes its "ret N" removes and which registers it takes arguments in, and writes the stand-in table.

[acct2-bro] 2026-11-08. Reads the program as data only (nothing is run); Python's standard library only.

Usage:
    python3 fh_stand_in_spec.py --xbe default.xbe --functions recomp_dispatch.c [--names names.tsv] \\
        [--sections D3D,DSOUND] --out OUT_DIR

    --functions  the function starts: xboxrecomp's generated recomp_dispatch.c, or a table (see xdk_names.py)
    --names      names for some of them: xdk_names.py's names.tsv (addr, name, kind, ...)
    --sections   the sections whose functions get stand-ins (default D3D,DSOUND). Not D3DX: its nine small
                 functions work on the CPU (one has the same code as a vector-length function in Big Red One) and
                 the game needs their results

Writes into OUT_DIR:
    functions.tsv  every function start in the program's code with its length to the next start (entry, size):
                   ngl_stubs.py's sizes, also for following a tail call out of a stand-in's function
    spec.tsv       one row per stand-in: addr, name, group, returns, params (ngl_stubs.py's spec form). A function
                   without a name is called SECTION_ADDRESS; "::" in a library name becomes "__".
"""
import argparse
import collections
import csv
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from xdk_names import Xbe, read_starts  # noqa: E402

GROUP = {"D3D": "direct3d", "D3DX": "d3dx", "DSOUND": "directsound"}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--xbe", required=True)
    ap.add_argument("--functions", required=True, action="append")
    ap.add_argument("--names")
    ap.add_argument("--sections", default="D3D,DSOUND")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)

    x = Xbe(a.xbe)
    starts = set()
    for p in a.functions:
        starts |= read_starts(p)
    starts = sorted(s for s in starts if x.section(s))
    names = {}
    if a.names:
        csv.field_size_limit(1 << 24)
        with open(a.names, encoding="utf-8", errors="replace", newline="") as fh:
            for r in csv.DictReader(fh, delimiter="\t", quoting=csv.QUOTE_NONE):
                if (r.get("kind") or "function") == "function":
                    try:
                        names[int(r["addr"], 16)] = r["name"].replace("::", "__")
                    except (KeyError, ValueError):
                        pass

    with open(os.path.join(a.out, "functions.tsv"), "w", encoding="utf-8", newline="\n") as fh:
        fh.write("entry\tsize\n")
        for i, s in enumerate(starts):
            sec = x.section(s)
            end = x.end_of(sec)
            if i + 1 < len(starts) and starts[i + 1] < end:
                end = starts[i + 1]
            fh.write(f"{s:08X}\t{max(0, end - s)}\n")

    wanted = [w.strip() for w in a.sections.split(",") if w.strip()]
    rows, used = [], collections.Counter()
    for s in starts:
        sec = x.section(s)[0]
        if sec not in wanted:
            continue
        n = names.get(s) or f"{sec}_{s:08X}"
        used[n] += 1
        if used[n] > 1:                     # one name on two starts: keep both, the second with its address
            n = f"{n}_{s:08X}"
        rows.append((s, n, GROUP.get(sec, sec.lower())))
    with open(os.path.join(a.out, "spec.tsv"), "w", encoding="utf-8", newline="\n") as fh:
        fh.write("addr\tname\tgroup\treturns\tparams\n")
        for s, n, g in rows:
            fh.write(f"{s:08X}\t{n}\t{g}\t\t\n")
    per = collections.Counter(g for _, _, g in rows)
    named = sum(1 for s, _, _ in rows if s in names)
    print(f"fh_stand_in_spec: {len(starts)} function starts; {len(rows)} stand-ins ({named} named): "
          + ", ".join(f"{g} {n}" for g, n in per.most_common()))


if __name__ == "__main__":
    main()
