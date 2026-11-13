#!/usr/bin/env python3
"""[acct2-decomp] Survey of every .cod archive under a folder: the unknown header and section fields
next to values they might be (file size, sums of chunk sizes, counts), and the order of the
tables and chunks, so the writer can set them by rule. Prints rule hits per field.
argv: folder [--show N]  (read-only; output stays on the PC)"""
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kapf
root = sys.argv[1]
show = int(sys.argv[3]) if len(sys.argv) > 3 and sys.argv[2] == "--show" else 5
files = []
for dp, _, fs in os.walk(root):
    for fn in fs:
        if fn.lower().endswith(".cod"):
            files.append(os.path.join(dp, fn))
files.sort()
hits = {}
def rule(name, ok):
    a = hits.setdefault(name, [0, 0])
    a[0 if ok else 1] += 1
shown = 0
for p in files:
    try:
        a = kapf.read(p, unpack=False)
    except kapf.KapfError as e:
        print("skip", os.path.relpath(p, root), e)
        continue
    f = open(p, "rb").read(0x30 + 0x18 * len(a.sections))
    allc = [c for s in a.sections for c in s.chunks]
    unpacked = sum(c.size for c in allc)
    ondisk = sum(c.stored or c.size for c in allc)
    rule("directory ends before the first chunk", True)
    last = max(allc, key=lambda c: c.offset) if allc else None
    rule("chunks in table order", all(allc[i].offset < allc[i + 1].offset for i in range(len(allc) - 1)))
    rule("first chunk at 64K after tables", bool(allc) and allc[0].offset == (struct.unpack_from('<I', f, 0x10)[0] + 0xFFFF) // 0x10000 * 0x10000)
    rule("tables follow records", bool(a.sections) and struct.unpack_from("<I", f, 0x30 + 16)[0] == 0x30 + 0x18 * len(a.sections))
    for i, s in enumerate(a.sections):
        rule("s14 == chunk count", s.unknown14 == len(s.chunks))
    if shown < show:
        shown += 1
        print(f"{os.path.relpath(p, root)} size={a.size:#x} dir={len(a.directory):#x} dir_end={a.dir_end:#x} unpacked={unpacked:#x} ondisk={ondisk:#x} chunks={len(allc)} builder={a.builder}")
        for s in a.sections:
            print(f"   [{s.name or '-'}] chunks={len(s.chunks)} s14={s.unknown14} s20={s.unknown20:#x} "
                  + " ".join(f"{c.offset:#x}/{c.size:#x}/{c.stored:#x}/{c.slot:#x}/{c.flags:#x}" for c in s.chunks[:4]))
print(len(files), "archives")
for k, (y, n) in hits.items():
    print(f"  {k:40s} holds {y}, not {n}")
