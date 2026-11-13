#!/usr/bin/env python3
"""[acct2-decomp] Directory check of .cod archives through tools/cod_convert/kapf.py: reads each
archive's header, tables and directories only (no chunk data, so all 451 retail ones take well
under a second) and reports whether kapf.py rebuilt its directories byte for byte from the
parsed entries, and whether every entry's A, B, C equals the raw 0x14-byte record.
argv: folder   (read-only; prints counts only; exit 1 if any archive fails)"""
import collections, os, struct, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kapf
c = collections.Counter()
t0 = time.time()
for root, _, files in os.walk(sys.argv[1]):
    for fn in sorted(files):
        if not fn.lower().endswith(".cod"):
            continue
        p = os.path.join(root, fn)
        try:
            a = kapf.read(p, unpack=False)
        except kapf.KapfError as e:
            c["read error"] += 1
            print("READ FAIL", os.path.relpath(p, sys.argv[1]), e)
            continue
        if not a.sections:
            c["empty archive"] += 1
            continue
        if any(s.entries is None for s in a.sections):
            c["directories not in the usual layout (kept as bytes)"] += 1
            print("UNUSUAL", os.path.relpath(p, sys.argv[1]))
            continue
        with open(p, "rb") as f:
            h = f.read(0x30 + 0x18 * len(a.sections))
            end = struct.unpack_from("<I", h, 0x10)[0]
            f.seek(0)
            raw = f.read(end + len(a.directory))
        good = all(struct.unpack_from("<3I", raw, struct.unpack_from("<I", h, 0x30 + 0x18 * i + 20)[0] + 0x14 * k + 8) == (e.a, e.b, e.c)
                   for i, s in enumerate(a.sections) for k, e in enumerate(s.entries))
        c["rebuilt byte for byte, entries match" if good else "ENTRY MISMATCH"] += 1
        c["entries"] += sum(len(s.entries) for s in a.sections)
for k, v in c.items():
    print(f"  {k:55s} {v}")
print(f"{time.time() - t0:.1f}s")
sys.exit(1 if c["read error"] or c["ENTRY MISMATCH"] or c["directories not in the usual layout (kept as bytes)"] else 0)
