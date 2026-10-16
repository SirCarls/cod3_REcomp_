#!/usr/bin/env python3
"""
script_api.py - list World at War's script API (the GSC builtin functions and
methods) from the { "name", handler, developer } tables the naming pass found.

For the Rust engine's script VM: every name the game's scripts can call, which
table it is in (function or method of player/actor/entity/...), whether it is
developer-only, and the address and C name of its handler in the decompile.

Usage:
    python3 tools/waw_decomp/script_api.py --exe CoDWaW.exe --names names.tsv --out script_api.tsv
"""
import argparse
import collections
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from name_pc import Exe  # noqa: E402
from naming_common import read_tsv  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", required=True)
    ap.add_argument("--names", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    exe = Exe(a.exe)
    names = {int(r["addr"], 16): r for r in read_tsv(a.names)}
    rows = []
    for addr, r in sorted(names.items()):
        if r["pass"] != "table" or r["kind"] != "global":
            continue
        m = re.match(r"(\d+) entries of \(name, handler\), stride (\d+)", r["evidence"])
        reader = r["evidence"].split("read by ")[1] if "read by " in r["evidence"] else ""
        if not m:
            continue
        count, stride = int(m.group(1)), int(m.group(2))
        handlers = []
        for k in range(count):
            e = addr + k * stride
            w = struct.unpack("<%dI" % (stride // 4), exe.bytes_at(e, stride))
            b = exe.bytes_at(w[0], 64)
            sname = b[:b.find(b"\0")].decode("latin-1")
            dev = w[2] if stride >= 12 else ""
            h = names.get(w[1])
            handlers.append((sname, w[1], h["name"] if h else "", dev))
        prefixes = collections.Counter(n.split("_")[0] for _, _, n, _ in handlers if "_" in n)
        kind = prefixes.most_common(1)[0][0] if prefixes else "?"
        for sname, h, cname, dev in handlers:
            rows.append((f"{addr:08X}", reader, kind, sname, f"{h:08X}", cname, str(dev)))
    with open(a.out, "w", encoding="utf-8") as o:
        o.write("table\tread_by\ttable_kind\tscript_name\thandler\tc_name\tdeveloper\n")
        for r in rows:
            o.write("\t".join(r) + "\n")
    kinds = collections.Counter(r[2] for r in rows)
    print(f"{len(rows)} entries in {len(set(r[0] for r in rows))} tables: " + ", ".join(f"{k} {v}" for k, v in kinds.most_common()))


if __name__ == "__main__":
    main()
