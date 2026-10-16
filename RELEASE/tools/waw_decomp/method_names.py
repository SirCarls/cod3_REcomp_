#!/usr/bin/env python3
"""
method_names.py - name the C++ methods of a build that name themselves: assertions with the
compiler's function signature ("bool __cdecl bdUnicastConnection::handleData(...)") and
Demonware log lines that start with the method ("bdCore::init"). The same pass runs inside
name_pc.py and name_x360.py; this runs it alone on any WawExport.java export, for a build
whose names come from elsewhere (Call of Duty 3's symbol map).

Usage:
    python3 tools/waw_decomp/method_names.py --raw <export dir> --out <names.tsv> [--skip <names.tsv>]

--skip: a names table of the build; functions it already names are left out of the output, and
so are names it already uses.
"""
import argparse
import collections
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from naming_common import Namer, Tables, pass_methods, read_tsv  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--raw", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--skip")
    args = ap.parse_args()
    t = Tables(args.raw)
    namer = Namer()
    if args.skip:
        for r in read_tsv(args.skip):
            if r["kind"] == "function":
                namer.add(int(r["addr"], 16), r["name"], "function", "existing", "")
    stats = collections.Counter()
    pass_methods(namer, t, stats)
    with open(args.out, "w", encoding="utf-8") as o:
        o.write("addr\tname\tkind\tpass\tevidence\n")
        for addr in sorted(namer.by_addr):
            name, kind, pas, ev = namer.by_addr[addr]
            if pas == "method":
                o.write(f"{addr:08X}\t{name}\t{kind}\t{pas}\t{ev}\n")
    print(dict(stats))


if __name__ == "__main__":
    sys.exit(main())
