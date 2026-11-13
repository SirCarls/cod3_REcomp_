#!/usr/bin/env python3
"""[acct2-decomp] Checks tools/cod_convert/apkf.py. Read-only; prints counts only; exit 1 on any
failure.

  apkfcheck.py --selftest           synthetic chunks only (no game data)
  apkfcheck.py ARCHIVE [ARCHIVE...]  every APKF chunk of each archive's unnamed section:
    1. parse + build gives the same bytes, and the tables are attached to the items
    2. grow: one item's object gets 16 more bytes; build, parse again: every item, pointer
       (by target item and offset) and import is the same, the later items have moved
    3. add: a copy of one item is appended to its type, with its pointers to itself moved to
       the copy; build, parse again: the old items are unchanged, the copy is there
"""
import collections, os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import apkf

c = collections.Counter()


def semantic(a):
    """Everything a rebuild must keep, with items named by their order and pointer fields
    blanked in the bytes (their value is the target, compared separately)."""
    flat = [it for _, _, it in a.items()]
    idx = {id(it): n for n, it in enumerate(flat)}
    out = []
    for k, t in enumerate(a.types):
        for i, it in enumerate(t.items):
            parts = [bytearray(it.obj), bytearray(it.gpu) if it.gpu is not None else None]
            for p in it.pointers:
                parts[p.area][p.field:p.field + 4] = b"\0\0\0\0"
            ptrs = [(p.area, p.field, None if p.target.item is None else idx[id(p.target.item)],
                     p.target.area, p.target.offset) for p in it.pointers]
            imps = [(m.area, m.field, m.type, a.name(m.name_off)) for m in it.imports]
            out.append((k, i, a.name(it), bytes(parts[0]), None if parts[1] is None else bytes(parts[1]), ptrs, imps))
    return out


def grow_and_add(a, label):
    before = semantic(a)
    flat = [(t, k, it) for k, t in enumerate(a.types) for it in t.items]
    # grow the first item that holds a pointer (or the first item at all)
    pick = next((x for x in flat if x[2].pointers), flat[0] if flat else None)
    if pick is None:
        c["no items (skipped)"] += 1
        return
    t, k, it = pick
    n0 = len(it.obj)
    a.replace(it, obj=it.obj + bytes(16))
    b = apkf.parse(apkf.build(a))
    after = semantic(b)
    exp = []
    for e in before:
        if e[2] == a.name(it) and e[0] == k and len(e[3]) == n0 and e[1] == t.items.index(it):
            e = (e[0], e[1], e[2], e[3] + bytes(16), e[4], e[5], e[6])
        exp.append(e)
    ok = b.relocatable and after == exp
    c["grow: same items, pointers and imports" if ok else "GROW CHANGED SOMETHING"] += 1
    if not ok:
        print("  grow failed:", label)
    # add a copy of the grown item to its type
    a2 = b
    t2 = a2.types[k]
    src = t2.items[t.items.index(it)]
    new = a2.add_item(k, a2.name(src) + "_copy", src.obj, src.gpu)
    new.pointers = [apkf.Pointer(p.field, apkf.Target(new if p.target.item is src else p.target.item,
                                                      p.target.area, p.target.offset), p.area)
                    for p in src.pointers]
    new.imports = [apkf.Import(m.field, m.type, m.name_off, m.area) for m in src.imports]
    before2 = semantic(a2)
    d = apkf.parse(apkf.build(a2))
    after2 = semantic(d)
    ok2 = d.relocatable and after2 == before2 and len(after2) == len(after) + 1
    c["add: old items unchanged, the copy is there" if ok2 else "ADD CHANGED SOMETHING"] += 1
    if not ok2:
        print("  add failed:", label)


def check_chunk(data, label):
    try:
        a = apkf.parse(data)
    except apkf.ApkfError as e:
        c["PARSE ERROR"] += 1
        print("  parse failed:", label, e)
        return
    same = apkf.build(a) == data
    c["rebuilt byte for byte" if same else "REBUILT DIFFERS"] += 1
    c["tables attached to the items" if a.relocatable else "tables kept as read"] += 1
    if not same:
        print("  differs:", label)
        return
    if a.relocatable:
        try:
            grow_and_add(a, label)
        except apkf.ApkfError as e:
            c["GROW/ADD ERROR"] += 1
            print("  grow/add error:", label, e)


def selftest():
    a = apkf.new(2)
    tex = a.add_type(0x58455400, 3, 2, [0x10, 0x1000])      # two parts
    mat = a.add_type(0x4D415400, 1, 1, [0x10])               # one part
    t1 = a.add_item(tex, "brick$wall_c", bytes(range(0x60)), bytes(0x2000))
    t2 = a.add_item(tex, "brick$wall_n", bytes(0x60), b"\x11" * 0x1000)
    m = a.add_item(mat, "brick_wall", bytes(0x40))
    nm = a.add_name("diffuse")
    m.pointers = [apkf.Pointer(0x00, apkf.Target.name(m.name_off)),
                  apkf.Pointer(0x08, apkf.Target(m, 0, 0x20)),
                  apkf.Pointer(0x10, apkf.Target(t1, 0, 0)),
                  apkf.Pointer(0x14, apkf.Target(t2, 1, 0x100)),
                  apkf.Pointer(0x0C, apkf.Target.name(nm))]
    m.imports = [apkf.Import(0x18, 0x46582020, nm)]
    raw = apkf.build(a)
    b = apkf.parse(raw)
    c["selftest: new chunk parses, tables attached" if b.relocatable and semantic(b) == semantic(a) else "SELFTEST NEW CHUNK"] += 1
    c["selftest: rebuilt byte for byte" if apkf.build(b) == raw else "SELFTEST REBUILD"] += 1
    be = lambda o: struct.unpack_from(">I", raw, o)[0]
    c["selftest: header words" if raw[:4] == b"APKF" and be(4) == 0x04010000 and be(0x10) == 2 else "SELFTEST HEADER"] += 1
    grow_and_add(b, "selftest")
    # names: hash of the full name, text cut to 27
    long = "a_very_long_texture_name_that_is_cut_c"
    rec = apkf.name_record(long)
    c["selftest: name record" if struct.unpack(">I", rec[:4])[0] == apkf.name_hash(long) and rec[4:].rstrip(b"\0") == long.encode()[:27] and len(rec) % 4 == 0 else "SELFTEST NAME"] += 1
    # A for the directory
    c["selftest: directory A" if apkf.directory_a(".xetex", t1) == 0x2000 + 0x10010 and apkf.directory_a("xemesh", m) is None and apkf.directory_a("dbb", m) == 0x40 else "SELFTEST A"] += 1
    # the directory entry of a new item
    import kapf
    e = apkf.directory_entry(b, b.types[tex].items[1], 3, "brick$wall_n.xetex", "C:\\cod\\obj\\x.xetex")
    c["selftest: directory entry" if (e.a, e.b, e.c) == (0x1000 + 0x10010, 3, tex << 16 | 1) else "SELFTEST ENTRY"] += 1
    # a pointer beyond a shrunk target is refused
    b2 = apkf.parse(raw)
    mm = b2.types[mat].items[0]
    try:
        b2.replace(mm, obj=bytes(0x10))
        c["SELFTEST SHRINK NOT REFUSED"] += 1
    except apkf.ApkfError:
        c["selftest: a shrink that cuts its own pointer fields is refused"] += 1
    b3 = apkf.parse(raw)
    tt = b3.types[tex].items[1]
    b3.replace(tt, gpu=bytes(0x80))          # m points 0x100 into it
    try:
        apkf.build(b3)
        c["SELFTEST DANGLING NOT REFUSED"] += 1
    except apkf.ApkfError:
        c["selftest: a pointer past a shrunk target is refused at build"] += 1


if __name__ == "__main__":
    if sys.argv[1:] == ["--selftest"]:
        selftest()
    else:
        import kapf
        for p in sys.argv[1:]:
            try:
                arc = kapf.read(p)
            except kapf.KapfError as e:
                c["ARCHIVE READ ERROR"] += 1
                print("  read failed:", p, e)
                continue
            c["archives"] += 1
            for s in arc.sections:
                if s.name:
                    continue
                for n, ch in enumerate(s.chunks):
                    if ch.data[:4] != b"APKF":
                        continue
                    c["APKF chunks"] += 1
                    check_chunk(ch.data, f"{os.path.basename(os.path.dirname(p))}/{os.path.basename(p)}#{n}")
    for k, v in sorted(c.items()):
        print(f"  {v:7d}  {k}")
    bad = [k for k in c if k.split(":")[0].split()[0].isupper() and k.split()[0] not in ("APKF",)]
    sys.exit(1 if bad else 0)
