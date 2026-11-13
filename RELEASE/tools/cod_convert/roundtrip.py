#!/usr/bin/env python3
"""[acct2-decomp] Round trip of retail .cod archives through tools/cod_convert/kapf.py:
read (unpacking NCH blocks), write again with every chunk stored, read back, and check that
  - an archive whose chunks were all stored comes out byte for byte the same file;
  - for packed ones, every chunk's unpacked bytes, the directory and every field are the same.
argv: out_dir, archive [archive ...]   (writes only into out_dir, deletes what it wrote)"""
import hashlib, os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kapf
out = sys.argv[1]
os.makedirs(out, exist_ok=True)
bad = 0
for p in sys.argv[2:]:
    t0 = time.time()
    try:
        a = kapf.read(p)
    except kapf.KapfError as e:
        print(f"READ FAIL {p}: {e}"); bad += 1; continue
    tmp = os.path.join(out, "rt.cod")
    kapf.write(a, tmp)
    b = kapf.read(tmp)
    problems = []
    if [s.name for s in a.sections] != [s.name for s in b.sections]:
        problems.append("section names")
    for sa, sb in zip(a.sections, b.sections):
        if (sa.unknown14, sa.unknown20, len(sa.chunks)) != (sb.unknown14, sb.unknown20, len(sb.chunks)):
            problems.append(f"section {sa.name!r} fields")
        for k, (ca, cb) in enumerate(zip(sa.chunks, sb.chunks)):
            if ca.data != cb.data or (ca.slot, ca.flags) != (cb.slot, cb.flags):
                problems.append(f"section {sa.name!r} chunk {k}")
    if (a.directory, a.dir_end, a.builder) != (b.directory, b.dir_end, b.builder):
        problems.append("directory / header")
    packed = any(c.stored for s in a.sections for c in s.chunks)
    same_file = None
    if not packed:
        same_file = hashlib.md5(open(p, "rb").read()).digest() == hashlib.md5(open(tmp, "rb").read()).digest()
        if not same_file:
            problems.append("file not byte-identical")
    os.remove(tmp)
    n = sum(len(s.chunks) for s in a.sections)
    print(f"{'ok  ' if not problems else 'FAIL'} {p} ({n} chunks, {'packed' if packed else 'stored, file identical' if same_file else 'stored'}, {time.time() - t0:.1f}s)"
          + ("" if not problems else ": " + "; ".join(problems[:5])))
    bad += bool(problems)
sys.exit(1 if bad else 0)
