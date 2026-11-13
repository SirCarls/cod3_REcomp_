"""[acct2-decomp] APKF chunks (the banks inside a .cod's unnamed section): read, write, change an
item, add items, in plain Python. The container is docs/formats/cod3.md ("Unpacked main chunks:
APKF"), every value big-endian; the loader is sub_82129348 (relocation sub_82128B98, imports
sub_82128C30). Rebuilt with parse() then build(), every APKF chunk of the 451 retail archives
comes out byte for byte (dev notes, acct2-decomp 2026-11-05).

  0x00 "APKF", 0x04010000 (version 0x104, the loader checks it), 0x04000000, 0xFFFFFFFF,
       u32 area count (1 or 2), u32 directory end (from the chunk's start), u32 0
  0x1C type records, (areas + 5) words each: type, version, part count, self-relative pointer to
       the item table, item count, one word per area: part index << 24 | alignment
       (0xFF000000 = the type has no part in that area)
       then one zero word (it ends the loader's walk), then the item tables back to back in record
       order: per item a self-relative pointer to its name record, then one size per part
       then the name pool up to the directory end (name records: u32 hash, then the name, cut to
       27 characters, NUL-terminated, 4-aligned; hash = hash * 33 + byte over the full lower-case
       name, from 0)
  dir end: 0x14 bytes (an extension text or zeros), then one 0x18-byte descriptor per area:
       +0 self-relative pointer to the area's name ("image", "physical"), +4 0, +8 alignment of
       the area's start (image 0x10 or the largest item alignment, physical 0x10000), +12 0 or
       0x10000 (not read by the loader; kept as read, 0 for new chunks), +16 the area's size,
       +20 self-relative pointer to the area's data (the loader places every part from it)
  image: from the descriptors' end rounded up to the image alignment; each item's object at its
       type's alignment from the area's start, zero filled
  physical (2 areas): from the image end rounded up to 64 KB; each GPU part likewise
  pointer table at the last area's end rounded up to 4: one u32 per pointer field,
       area << 26 | dword index of the field in that area's data, then 0xFFFFFFFF; the field
       holds its target the same way, area 63 being the name pool (0xFC000000 | dword index)
  import table: (field as above, u32 type, u32 name offset in the pool) per entry, then
       0xFFFFFFFF; the loader fills the field with the named resource, so the file's value there
       is only a placeholder. The chunk ends there.
  In every retail chunk both tables are grouped by the item holding the field, in item order;
  within an item the order is the builder's and is kept.

Here every pointer is attached to the item holding its field and names its target as (item,
area, offset in that item's part) or a name (pool offset), and every import to its item; build()
lays the parts out again and writes both tables and every pointer field from that. So an item
can change size, and items and names can be added, and the pointers follow. A chunk whose tables
don't fit that model (none among the retail ones) is kept with its tables as read, and then only
same-size changes are allowed.

In the archive's directory an item is B = the chunk's index in the section and C = type record
index << 16 | the item's index in its type's table; items are only ever appended, so existing
items keep their C. Only the items the game looks up by name have an entry (11,766 of the 25,209
items of the stored retail archives); the rest (a model's materials, meshes, skeleton, ...) are
reached through pointers and have none. The entries' order is the builder's (388 of the 448
streamed sections are not in chunk order), so new ones are appended. directory_entry() makes one,
with the A the game's loaders expect (directory_a()).
"""
from __future__ import annotations

import struct
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Sequence, Tuple

MAGIC = b"APKF"
HEAD_WORDS = (0x04010000, 0x04000000, 0xFFFFFFFF)
NO_PART = 0xFF000000
NAME_AREA = 63
END = 0xFFFFFFFF
PHYS_ALIGN = 0x10000
NAME_MAX = 27


class ApkfError(ValueError):
    pass


def _up(x: int, a: int) -> int:
    return (x + a - 1) // a * a if a > 1 else x


def name_hash(name: str) -> int:
    h = 0
    for b in name.lower().encode("latin-1"):
        h = (h * 33 + b) & 0xFFFFFFFF
    return h


def name_record(name: str) -> bytes:
    """A name record as the builder writes it: hash of the full name, the name cut to 27
    characters, NUL, zero fill to 4."""
    rec = struct.pack(">I", name_hash(name)) + name.encode("latin-1")[:NAME_MAX] + b"\0"
    return rec + bytes(_up(len(rec), 4) - len(rec))


@dataclass
class Target:
    """Where a pointer field points: a part of an item (area 0 = its object, 1 = its GPU part)
    at a byte offset, or a name (area 63, offset in the name pool)."""
    item: Optional["Item"]
    area: int
    offset: int

    @staticmethod
    def name(pool_off: int) -> "Target":
        return Target(None, NAME_AREA, pool_off)


@dataclass
class Pointer:
    field: int                      # byte offset of the field in its item's part
    target: Target
    area: int = 0                   # the part holding the field (always 0 in retail chunks)


@dataclass
class Import:
    field: int
    type: int                       # four-character code
    name_off: int                   # name record in the pool
    area: int = 0


@dataclass(eq=False)
class Item:
    name_off: int                   # its name record, from the name pool's start
    sizes: List[int]                # one per part (rewritten by build)
    obj: bytes = b""                # the object (image) bytes
    gpu: Optional[bytes] = None     # the GPU (physical) bytes, for a 2-part item
    pointers: List[Pointer] = field(default_factory=list)
    imports: List[Import] = field(default_factory=list)

    def part(self, area: int) -> bytes:
        return self.obj if area == 0 else (self.gpu or b"")


@dataclass
class TypeRecord:
    words: List[int]                # (areas + 5) words; the table pointer and count are rewritten
    items: List[Item] = field(default_factory=list)

    @property
    def type(self) -> int:
        return self.words[0]

    @property
    def parts(self) -> int:
        return self.words[2]

    def has(self, area: int) -> bool:
        return self.words[5 + area] != NO_PART

    def align(self, area: int) -> int:
        w = self.words[5 + area]
        return 1 if w == NO_PART else max(1, w & 0xFFFFFF)


@dataclass
class Apkf:
    head: bytes                     # 0x00-0x1C as read (the area count and directory end rewritten)
    types: List[TypeRecord]
    pool: bytes                     # the name pool
    dir_tail: bytes                 # the 0x14 bytes at the directory end
    descs: List[List[int]]          # per area 6 words; [0] = the area name's pool offset; [4], [5] rewritten
    tables: Optional[bytes] = None  # the pointer and import tables as read, only when they can't be rebuilt

    @property
    def areas(self) -> int:
        return len(self.descs)

    @property
    def relocatable(self) -> bool:
        return self.tables is None

    def items(self):
        """(type record, index, item) in directory order."""
        for t in self.types:
            for i, it in enumerate(t.items):
                yield t, i, it

    def name(self, item_or_off) -> str:
        off = item_or_off.name_off if isinstance(item_or_off, Item) else item_or_off
        z = self.pool.find(b"\0", off + 4)
        return self.pool[off + 4:z].decode("latin-1")

    def name_hash(self, item: Item) -> int:
        return struct.unpack_from(">I", self.pool, item.name_off)[0]

    def where(self, item: Item) -> Tuple[int, int]:
        """(type record index, index in its table): the item's C in the archive directory is
        the first << 16 | the second."""
        for k, t in enumerate(self.types):
            for i, it in enumerate(t.items):
                if it is item:
                    return k, i
        raise ApkfError("the item is not in this chunk")

    # -- changes ---------------------------------------------------------------------------

    def add_name(self, name: str) -> int:
        """Appends a name record to the pool; returns its offset."""
        off = len(self.pool)
        if off % 4:
            raise ApkfError("the pool's end is not 4-aligned")
        self.pool += name_record(name)
        return off

    def add_type(self, type_code: int, version: int, parts: int, aligns: Sequence[int]) -> int:
        """Appends a type record; returns its index. aligns: one alignment per part."""
        if not 1 <= parts <= self.areas or len(aligns) != parts:
            raise ApkfError("one alignment per part, at most one part per area")
        words = [type_code, version, parts, 0, 0]
        for k in range(self.areas):
            words.append((k << 24 | aligns[k]) if k < parts else NO_PART)
        self.types.append(TypeRecord(words))
        return len(self.types) - 1

    def add_item(self, type_index: int, name: str, obj: bytes, gpu: Optional[bytes] = None,
                 pointers: Sequence[Pointer] = (), imports: Sequence[Import] = ()) -> Item:
        """Appends an item at the end of its type's table (existing items keep their C)."""
        if not self.relocatable:
            raise ApkfError("this chunk's tables are kept as read: no new items")
        t = self.types[type_index]
        if (t.parts > 1) != (gpu is not None):
            raise ApkfError("GPU bytes exactly for a type with two parts")
        it = Item(self.add_name(name), [0] * t.parts, bytes(obj), None if gpu is None else bytes(gpu),
                  list(pointers), list(imports))
        t.items.append(it)
        return it

    def replace(self, item: Item, obj: Optional[bytes] = None, gpu: Optional[bytes] = None,
                pointers: Optional[Sequence[Pointer]] = None,
                imports: Optional[Sequence[Import]] = None) -> None:
        """Swaps an item's bytes, of any size when the chunk is relocatable. Without new
        pointers/imports the item keeps its own, which must still fit; pointers from other
        items into it are checked when the chunk is built."""
        if obj is not None and len(obj) != len(item.obj) or \
                gpu is not None and item.gpu is not None and len(gpu) != len(item.gpu):
            if not self.relocatable:
                raise ApkfError("this chunk's tables are kept as read: only the same size")
        if gpu is not None and item.gpu is None:
            raise ApkfError("GPU bytes for an item without a GPU part")
        new_obj = item.obj if obj is None else bytes(obj)
        new_gpu = item.gpu if gpu is None else bytes(gpu)
        ptrs = item.pointers if pointers is None else list(pointers)
        imps = item.imports if imports is None else list(imports)
        for f in [(p.area, p.field) for p in ptrs] + [(m.area, m.field) for m in imps]:
            if f[1] % 4 or f[1] + 4 > len(new_obj if f[0] == 0 else (new_gpu or b"")):
                raise ApkfError(f"a pointer or import field at {f[1]:#x} is outside the new bytes: give new ones")
        item.obj, item.gpu, item.pointers, item.imports = new_obj, new_gpu, ptrs, imps


def new(areas: int = 2) -> Apkf:
    """An empty chunk to add types and items to."""
    if areas not in (1, 2):
        raise ApkfError("1 or 2 areas")
    a = Apkf(MAGIC + struct.pack(">3I", *HEAD_WORDS) + struct.pack(">3I", areas, 0, 0), [], b"",
             bytes(0x14), [])
    for k, nm in enumerate(("image", "physical")[:areas]):
        a.descs.append([a.add_name(nm), 0, 0x10 if k == 0 else PHYS_ALIGN, 0, 0, 0])
    return a


# -- layout --------------------------------------------------------------------------------

def _layout(a: Apkf, desc_end: int) -> Tuple[List[int], List[int], Dict[int, List[int]]]:
    """(area data starts in the chunk, area sizes, id(item) -> part start per area from the
    area's data start)."""
    flat = [(t, it) for t, _, it in a.items()]
    starts: Dict[int, List[int]] = {id(it): [0] * a.areas for _, it in flat}
    data, sizes = [], []
    pos = _up(desc_end, a.descs[0][2])
    for k in range(a.areas):
        if k:
            pos = _up(pos, PHYS_ALIGN)
        base = pos
        for t, it in flat:
            if k and not t.has(k):
                continue
            q = _up(pos - base, t.align(k)) + base
            starts[id(it)][k] = q - base
            pos = q + len(it.part(k))
        data.append(base)
        sizes.append(pos - base)
    return data, sizes, starts


def _find(parts: List[Tuple[int, int, Item]], off: int, end_ok: bool) -> Optional[Item]:
    """The item whose part (start, size) holds off (or ends there, if end_ok)."""
    lo, hi, k = 0, len(parts) - 1, -1
    while lo <= hi:
        m = (lo + hi) // 2
        if parts[m][0] <= off:
            k, lo = m, m + 1
        else:
            hi = m - 1
    if k < 0:
        return None
    s, n, it = parts[k]
    if off < s + n or (end_ok and off == s + n):
        return it
    return None


def _zero(b: bytes, what: str) -> None:
    if any(b):
        raise ApkfError(f"{what} is not zero filled")


def parse(ch: bytes, keep_tables: bool = False) -> Apkf:
    """Reads an APKF chunk. The pointer and import tables are attached to the items when a
    rebuild gives the same bytes (every retail chunk); otherwise, or with keep_tables, they are
    kept as read."""
    if ch[:4] != MAGIC or len(ch) < 0x20:
        raise ApkfError("not an APKF chunk")

    def be(o: int) -> int:
        if o < 0 or o + 4 > len(ch):
            raise ApkfError(f"read past the end at {o:#x}")
        return struct.unpack_from(">I", ch, o)[0]

    areas, dir_end = be(0x10), be(0x14)
    if areas not in (1, 2) or not 0x1C < dir_end < len(ch):
        raise ApkfError(f"odd header: {areas} areas, directory end {dir_end:#x}")
    rec = (areas + 5) * 4
    o, first, raw = 0x1C, None, []
    while first is None or o + rec <= first:
        w = [be(o + 4 * i) for i in range(areas + 5)]
        tbl = (o + 12 + w[3]) & 0xFFFFFFFF
        if not 0x1C < tbl < dir_end:
            raise ApkfError(f"type record at {o:#x}: item table at {tbl:#x}")
        first = tbl if first is None else min(first, tbl)
        raw.append((w, tbl))
        o += rec
    if first != o + 4:
        raise ApkfError("the item tables don't start one word after the type records")
    _zero(ch[o:first], "the word after the type records")
    types, pos = [], first
    for w, tbl in raw:
        parts, count = w[2], w[4]
        if tbl != pos or not 1 <= parts <= areas:
            raise ApkfError(f"type {w[0]:#x}: table at {tbl:#x}, {parts} parts")
        for k in range(areas):
            if w[5 + k] != NO_PART and (w[5 + k] >> 24 != k or k >= parts):
                raise ApkfError(f"type {w[0]:#x}: area word {w[5 + k]:#x}")
        if parts > 1 and w[6] == NO_PART:
            raise ApkfError(f"type {w[0]:#x}: two parts, no area for the second")
        t = TypeRecord(list(w))
        for k in range(count):
            e = tbl + k * (parts + 1) * 4
            nm = (e + be(e)) & 0xFFFFFFFF
            t.items.append(Item(nm, [be(e + 4 + 4 * j) for j in range(parts)]))
        types.append(t)
        pos = tbl + count * (parts + 1) * 4
    pool_start = pos
    pool = bytes(ch[pool_start:dir_end])
    for t in types:
        for it in t.items:
            if not pool_start <= it.name_off < dir_end:
                raise ApkfError(f"a name record at {it.name_off:#x} outside the pool")
            it.name_off -= pool_start
    dir_tail = bytes(ch[dir_end:dir_end + 0x14])
    descs = []
    for k in range(areas):
        d = dir_end + 0x14 + 0x18 * k
        w = [be(d + 4 * i) for i in range(6)]
        w[0] = ((d + w[0]) & 0xFFFFFFFF) - pool_start
        if not 0 <= w[0] < len(pool):
            raise ApkfError(f"area {k}: name outside the pool")
        descs.append(w)
    desc_end = dir_end + 0x14 + 0x18 * areas
    img = _up(desc_end, descs[0][2])
    _zero(ch[desc_end:img], "the gap before the image")
    flat = [(t, it) for t in types for it in t.items]
    parts_at: List[List[Tuple[int, int, Item]]] = [[] for _ in range(areas)]
    pos = img
    for k in range(areas):
        if k:
            phys = _up(pos, PHYS_ALIGN)
            _zero(ch[pos:phys], "the gap before the physical area")
            pos = phys
        base = pos
        if (dir_end + 0x14 + 0x18 * k + 20 + descs[k][5]) & 0xFFFFFFFF != base:
            raise ApkfError(f"area {k}: its data pointer doesn't point at its data")
        for t, it in flat:
            if k and not t.has(k):
                continue
            q = _up(pos - base, t.align(k)) + base
            _zero(ch[pos:q], f"a gap in area {k}")
            n = it.sizes[k]
            data = bytes(ch[q:q + n])
            if len(data) != n:
                raise ApkfError("an item runs past the chunk's end")
            if k == 0:
                it.obj = data
            else:
                it.gpu = data
            parts_at[k].append((q - base, n, it))
            pos = q + n
        if pos != base + descs[k][4]:
            raise ApkfError(f"the parts don't fill area {k}")
    tbl = _up(pos, 4)
    _zero(ch[pos:tbl], "the gap before the pointer table")
    q = tbl
    ptrs = []
    while be(q) != END:
        ptrs.append(be(q))
        q += 4
    q += 4
    imps = []
    while be(q) != END:
        imps.append((be(q), be(q + 4), be(q + 8)))
        q += 12
    q += 4
    if q != len(ch):
        raise ApkfError(f"the import table ends {q - len(ch):+d} bytes from the chunk's end")
    a = Apkf(bytes(ch[:0x1C]), types, pool, dir_tail, descs, bytes(ch[tbl:]))
    if keep_tables:
        return a
    # attach the tables to the items; keep them as read if they don't fit the model
    try:
        _attach(a, ch, img, parts_at, ptrs, imps)
    except ApkfError:
        _detach(a)
        return a
    tables, align0 = a.tables, a.descs[0][2]
    a.tables = None
    try:
        same = build(a) == bytes(ch)
    except ApkfError:
        same = False
    if not same:
        _detach(a)
        a.tables = tables
        a.descs[0][2] = align0
    return a


def _detach(a: Apkf) -> None:
    for _, _, it in a.items():
        it.pointers, it.imports = [], []


def _attach(a: Apkf, ch: bytes, img: int, parts_at, ptrs, imps) -> None:
    order = {id(it): n for n, (_, _, it) in enumerate(a.items())}
    base = [img] + [None] * (a.areas - 1)
    if a.areas == 2:
        dir_end = struct.unpack_from(">I", ch, 0x14)[0]
        base[1] = (dir_end + 0x14 + 0x18 + 20 + a.descs[1][5]) & 0xFFFFFFFF

    def field_of(e: int):
        area, off = e >> 26, (e & 0x03FFFFFF) * 4
        if area >= a.areas:
            raise ApkfError("a field outside the areas")
        it = _find(parts_at[area], off, False)
        if it is None:
            raise ApkfError("a field outside every item")
        return area, off, it

    starts = {}
    for k in range(a.areas):
        for s, _, it in parts_at[k]:
            starts[(id(it), k)] = s
    last = -1
    for e in ptrs:
        area, off, it = field_of(e)
        n = order[id(it)]
        if n < last:
            raise ApkfError("the pointer table is not grouped by item")
        last = n
        rel = off - starts[(id(it), area)]
        if rel + 4 > len(it.part(area)):
            raise ApkfError("a field across an item's end")
        v = struct.unpack_from(">I", ch, base[area] + off)[0]
        va, vo = v >> 26, (v & 0x03FFFFFF) * 4
        if va == NAME_AREA:
            tgt = Target.name(vo)
        else:
            if va >= a.areas:
                raise ApkfError("a pointer into no area")
            ti = _find(parts_at[va], vo, True)
            if ti is None:
                raise ApkfError("a pointer into a gap")
            tgt = Target(ti, va, vo - starts[(id(ti), va)])
        it.pointers.append(Pointer(rel, tgt, area))
    last = -1
    for f, typ, nm in imps:
        area, off, it = field_of(f)
        n = order[id(it)]
        if n < last:
            raise ApkfError("the import table is not grouped by item")
        last = n
        rel = off - starts[(id(it), area)]
        if rel + 4 > len(it.part(area)):
            raise ApkfError("an import field across an item's end")
        it.imports.append(Import(rel, typ, nm, area))


def build(a: Apkf) -> bytes:
    areas = a.areas
    if areas not in (1, 2):
        raise ApkfError("1 or 2 areas")
    out = bytearray(a.head)
    struct.pack_into(">I", out, 0x10, areas)
    rec = (areas + 5) * 4
    first = 0x1C + rec * len(a.types) + 4
    pos, tables = first, []
    for t in a.types:
        if len(t.words) != areas + 5:
            raise ApkfError("a type record of another area count")
        tables.append(pos)
        pos += len(t.items) * (t.parts + 1) * 4
    pool_start, dir_end = pos, pos + len(a.pool)
    if len(a.pool) % 4:
        raise ApkfError("the name pool is not a multiple of 4 bytes")
    for t, tbl in zip(a.types, tables):
        w = list(t.words)
        o = len(out)
        w[3], w[4] = (tbl - (o + 12)) & 0xFFFFFFFF, len(t.items)
        out += struct.pack(f">{areas + 5}I", *w)
    out += bytes(4)
    for t in a.types:
        for it in t.items:
            if len(it.sizes) != t.parts:
                raise ApkfError("an item with another part count than its type")
            if (t.parts > 1) != (it.gpu is not None):
                raise ApkfError("an item's GPU part doesn't match its type")
            it.sizes = [len(it.part(k)) for k in range(t.parts)]
            e = len(out)
            out += struct.pack(f">{t.parts + 1}I", (pool_start + it.name_off - e) & 0xFFFFFFFF, *it.sizes)
    out += a.pool
    out += a.dir_tail.ljust(0x14, b"\0")[:0x14]
    struct.pack_into(">I", out, 0x14, dir_end)
    desc_at = len(out)
    desc_end = desc_at + 0x18 * areas
    if a.tables is None:
        need = max([t.align(0) for t in a.types] + [0x10])
        if a.descs[0][2] < need:
            a.descs[0][2] = need
    data, sizes, starts = _layout(a, desc_end)
    # the parts, with every pointer field written from its target
    parts: Dict[Tuple[int, int], bytearray] = {}
    if a.tables is None:
        for _, _, it in a.items():
            for p in it.pointers:
                buf = parts.setdefault((id(it), p.area), bytearray(it.part(p.area)))
                struct.pack_into(">I", buf, p.field, _value(a, p.target, starts))
    out += bytes(data[0] - len(out))
    for k in range(areas):
        if k:
            out += bytes(data[k] - len(out))
        for t, _, it in a.items():
            if k and not t.has(k):
                continue
            q = data[k] + starts[id(it)][k]
            out += bytes(q - len(out))
            out += parts.get((id(it), k)) or it.part(k)
    for k, d in enumerate(a.descs):
        w = list(d)
        o = desc_at + 0x18 * k
        w[0] = (pool_start + d[0] - o) & 0xFFFFFFFF
        w[4] = sizes[k]
        w[5] = (data[k] - (o + 20)) & 0xFFFFFFFF
        struct.pack_into(">6I", out, o, *w)
    out += bytes(_up(len(out), 4) - len(out))
    if a.tables is not None:
        out += a.tables
        return bytes(out)
    pt, it_tab = bytearray(), bytearray()
    for _, _, it in a.items():
        for p in it.pointers:
            pt += struct.pack(">I", _field(p.area, starts[id(it)][p.area] + p.field))
        for m in it.imports:
            if m.name_off % 4 or m.name_off + 4 > len(a.pool):
                raise ApkfError("an import's name is not a record in the pool")
            it_tab += struct.pack(">3I", _field(m.area, starts[id(it)][m.area] + m.field), m.type, m.name_off)
    out += pt + struct.pack(">I", END) + it_tab + struct.pack(">I", END)
    return bytes(out)


def _field(area: int, off: int) -> int:
    if off % 4 or off // 4 > 0x03FFFFFF:
        raise ApkfError(f"a field at {off:#x} can't be addressed (4-aligned, below 256 MB)")
    return area << 26 | off // 4


def _value(a: Apkf, t: Target, starts) -> int:
    if t.area == NAME_AREA:
        if t.offset % 4 or t.offset + 4 > len(a.pool):
            raise ApkfError("a name pointer outside the pool")
        return NAME_AREA << 26 | t.offset // 4
    if t.item is None or id(t.item) not in starts:
        raise ApkfError("a pointer to an item that is not in this chunk")
    if not 0 <= t.offset <= len(t.item.part(t.area)):
        raise ApkfError(f"a pointer {t.offset:#x} bytes into an item part of {len(t.item.part(t.area)):#x}")
    return _field(t.area, starts[id(t.item)][t.area] + t.offset)


# -- the archive directory's A for an item -------------------------------------------------

def directory_a(ext: str, item: Item) -> Optional[int]:
    """The A the game's loaders expect for a streamed item (dev notes 2026-11-05 05:45): the sum
    of the item's part sizes for most types; textures (.xetex, 0x60-byte object) the GPU part +
    0x10010; fonts (.xefont) the sum + 0xC0. None where no rule is known (meshes, .bpeb, .xefx,
    .xecell: no loader reads A on that path; keep a read item's A)."""
    ext = ext.lower().lstrip(".")
    total = len(item.obj) + len(item.gpu or b"")
    if ext == "xetex":
        return len(item.gpu or b"") + 0x10010 if len(item.obj) == 0x60 else None
    if ext == "xefont":
        return total + 0xC0
    if ext in ("xemesh", "bpeb", "xefx", "xecell"):
        return None
    return total


def directory_entry(a: Apkf, item: Item, chunk_index: int, name: str, source: str,
                    size_a: Optional[int] = None):
    """The archive directory entry (kapf.Entry) for an item of this chunk, chunk_index being the
    chunk's place in the streamed section: B, C from where the item is, A from directory_a()
    by the name's extension unless size_a is given."""
    import kapf
    k, i = a.where(item)
    ext = name.rsplit(".", 1)[-1] if "." in name else ""
    value = size_a if size_a is not None else directory_a(ext, item)
    if value is None:
        raise ApkfError(f"no rule for the A of a .{ext} item: give it")
    return kapf.Entry(name, source, value, chunk_index, k << 16 | i)
