"""[acct2-decomp] Made-up Call of Duty 3 archives for the tests: no game data. A KAPF file with
named sections and the unnamed (streamed) one, its chunks APKF banks of made-up textures and other
items, each chunk packed (one NCH block of an LZO1X literal run) or stored, laid out as the retail
archives are (c3w/cod_archive.py has the format)."""
import struct

TEX = 0x00584554            # textures (".xetex"), as in the game
TXT = 0x00545854            # made up: an item of one part (a language list, say)
NO_PART = 0xFF000000


def up(x: int, a: int) -> int:
    return (x + a - 1) // a * a if a > 1 else x


def lzo_literals(data: bytes) -> bytes:
    """An LZO1X stream of one literal run, then the end marker."""
    n = len(data)
    if not n:
        raise ValueError("nothing to pack")
    if n <= 238:
        head = bytes([17 + n])
    else:
        if n < 19:
            raise ValueError(n)
        z = (n - 19) // 255
        head = bytes(1 + z) + bytes([n - 18 - 255 * z])
    return head + data + b"\x11\x00\x00"


def nch(chunk: bytes) -> bytes:
    """A chunk as one NCH block (chunks under 512 KB)."""
    packed = lzo_literals(chunk)
    return b"NCH\0" + struct.pack(">7I", len(packed), 0, len(chunk), 0, 0, 0x20 + len(packed), 1) + packed


def texture(fill: int, size: int = 0x200, width: int = 64, fmt: int = 0x12, addr: int = 0) -> tuple:
    """(object, GPU data) of a made-up texture: the fetch constant at 0x48 with its GPU address."""
    obj = bytearray(0x60)
    obj[0:4] = b"tex!"
    struct.pack_into(">6I", obj, 0x48, 0x2, fmt | (addr << 12), (width - 1) | ((width - 1) << 13), 0, 0,
                     0x200 | (addr << 12))
    gpu = bytes((fill + i) & 0xFF for i in range(size))
    return bytes(obj), gpu


def apkf(types) -> bytes:
    """An APKF bank of two areas. types: [(type code, object alignment, GPU alignment or 0 for
    one part, [(name, object, GPU data or None)])]."""
    areas = 2
    rec = (areas + 5) * 4
    pos = 0x1C + rec * len(types) + 4           # the item tables, after the zero word
    tbls = []
    for _code, _a0, a1, items in types:
        tbls.append(pos)
        pos += len(items) * ((2 if a1 else 1) + 1) * 4
    pool_start, pool, name_at = pos, bytearray(), []
    for _code, _a0, _a1, items in types:
        row = []
        for name, _obj, _gpu in items:
            row.append(pool_start + len(pool))
            r = bytes(4) + name.encode("latin-1")[:27] + b"\0"
            pool += r + bytes(-len(r) % 4)
        name_at.append(row)
    area_name = pool_start + len(pool)
    pool += bytes(4) + b"area\0\0\0\0"
    dir_end = pool_start + len(pool)
    desc = dir_end + 0x14
    img = up(desc + 0x18 * areas, 0x10)
    body0 = bytearray()
    for _code, a0, _a1, items in types:
        for _name, obj, _gpu in items:
            body0 += bytes(up(len(body0), a0) - len(body0)) + obj
    phys = up(img + len(body0), 0x1000)
    body1 = bytearray()
    for _code, _a0, a1, items in types:
        if a1:
            for _name, _obj, gpu in items:
                body1 += bytes(up(len(body1), a1) - len(body1)) + gpu
    out = bytearray(phys + len(body1))
    out[0:4] = b"APKF"
    struct.pack_into(">6I", out, 4, 0x04010000, 0x04000000, 0xFFFFFFFF, areas, dir_end, 0)
    for k, (code, a0, a1, items) in enumerate(types):
        o = 0x1C + rec * k
        struct.pack_into(">7I", out, o, code, 1, 2 if a1 else 1, (tbls[k] - (o + 12)) & 0xFFFFFFFF, len(items),
                         a0, (1 << 24 | a1) if a1 else NO_PART)
        for i, (_name, obj, gpu) in enumerate(items):
            e = tbls[k] + i * ((2 if a1 else 1) + 1) * 4
            struct.pack_into(">I", out, e, (name_at[k][i] - e) & 0xFFFFFFFF)
            struct.pack_into(">I", out, e + 4, len(obj))
            if a1:
                struct.pack_into(">I", out, e + 8, len(gpu))
    out[pool_start:dir_end] = pool
    for k, (base, size, align) in enumerate(((img, len(body0), 0x10), (phys, len(body1), 0x1000))):
        d = desc + 0x18 * k
        struct.pack_into(">6I", out, d, (area_name - d) & 0xFFFFFFFF, 0, align, 0, size,
                         (base - (d + 20)) & 0xFFFFFFFF)
    out[img:img + len(body0)] = body0
    out[phys:phys + len(body1)] = body1
    out += bytes(-len(out) % 4) + b"\xff" * 8        # no pointers, no imports
    return bytes(out)


def kapf(sections, align: int = 0x10000) -> bytes:
    """A .cod archive. sections: [(name, [(chunk bytes, packed)], [(directory name, A, B, C)])],
    the streamed section's name ''. Chunks laid out in order, each at the last end rounded up to
    `align`, zero filled, as is the end."""
    nsec = len(sections)
    tables = 0x30 + 0x18 * nsec
    nch_ = sum(len(c) for _, c, _ in sections)
    end = tables + 0x20 * nch_
    nent = sum(len(e) for _, _, e in sections)
    names_at = end + 0x14 * nent
    names = bytearray()
    for _, _, ents in sections:
        for name, *_ in ents:
            names += name.encode("latin-1") + b"\0"
    u14 = up(names_at + len(names), 4)
    source = b"c:/cod/assets/made_up\0"
    u18 = up(u14 + len(source), 4)
    head = bytearray(u18)
    head[0:4] = b"KAPF"
    struct.pack_into("<6I", head, 4, 0x4003D70A, 0x30, nsec, end, u14, u18)
    head[0x1C:0x1C + 9] = b"made up\0\0"
    head[names_at:names_at + len(names)] = names
    head[u14:u14 + len(source)] = source
    datas, ci, di, ni = [], 0, 0, names_at
    for k, (name, chunks, ents) in enumerate(sections):
        r = 0x30 + 0x18 * k
        head[r:r + 12] = name.encode("latin-1")[:11].ljust(12, b"\0")
        struct.pack_into("<2H2I", head, r + 12, len(chunks), len(ents), tables + 0x20 * ci, end + 0x14 * di)
        for data, packed in chunks:
            datas.append((tables + 0x20 * ci, data, nch(data) if packed else data, packed))
            ci += 1
        for ename, a, b, c in ents:
            struct.pack_into("<5I", head, end + 0x14 * di, ni, u14, a, b, c)
            ni += len(ename.encode("latin-1")) + 1
            di += 1
    out = bytearray(head)
    for entry, data, raw, packed in datas:
        at = up(len(out), align)
        out += bytes(at - len(out)) + raw
        struct.pack_into("<5I", out, entry, at, len(data), up(len(data), 0x1000) + 0x1000, 0x82,
                         len(raw) if packed else 0)
    out += bytes(up(len(out), align) - len(out))
    return bytes(out)


def level(language: bytes, flag: tuple, flag_name: str, extra_texture: bool = False,
          align: int = 0x10000) -> bytes:
    """A made-up level archive: a named STRING section, and the streamed one with a packed bank
    (a language list, maybe another texture, the flag texture) and a stored bank (a plain
    texture, the same in every release)."""
    plain = texture(0x40, 0x300)
    texs = ([("other_c", *texture(0x10, 0x100))] if extra_texture else []) + [("flag_c", *flag)]
    bank0 = apkf([(TXT, 0x10, 0, [("lang", language, None)]), (TEX, 0x10, 0x100, texs)])
    bank1 = apkf([(TEX, 0x10, 0x100, [("plain_c", *plain)])])
    flag_item = len(texs) - 1
    return kapf([
        ("STRING", [(language * 3, False)], [("lvl.stbb", 0x10, 0, 0)]),
        ("", [(bank0, True), (bank1, False)],
         [("lang.stbb", 0x10, 0, 0), (flag_name, 0x20010, 0, 1 << 16 | flag_item),
          ("plain_c.xetex", 0x20010, 1, 0)]),
    ], align)
