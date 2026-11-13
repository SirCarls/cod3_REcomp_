"""[acct2-decomp] Call of Duty 3 .cod archives, the part the setup tool needs: read the chunk
tables and directories (KAPF), unpack a chunk (NCH blocks of LZO1X), find the items of an APKF
chunk and where their bytes are, and write an archive again with some chunks replaced (stored
as they are) and every other chunk kept byte for byte (packed or not, as it was).

The full reader and writer, with every rule checked against all 451 retail archives, are in the
project's tools/cod_convert (kapf.py, apkf.py, lzo1x.py; dev notes, acct2-decomp 2026-11-04/05);
this is a small plain-Python copy of what the setup tool uses, so it runs on its own.

KAPF (little-endian): header 0x30 ("KAPF", version 0x4003D70A, 0x30, section count, end of
the chunk tables, end of the directories, end of the source paths, builder name); section
records 0x18 (name char[12], u16 chunk count, u16 entry count, u32 table offset, u32 directory
offset); chunk entries 0x20 (offset, unpacked size, slot size, flags, stored size: 0 = stored
as is, else NCH blocks, one every 512 KB of the chunk); directory entries 0x14 (name offset,
source offset, A, B = chunk, C = APKF type record << 16 | item, for the unnamed section).
Chunks follow the directories in order, each at the previous end rounded up to 64 KB (32 KB
in archives with a single unnamed section), zero filled, as is the file's end. Retail archives
mix stored and packed chunks freely (168 of the 370 campaign ones), with the same flags.

APKF (big-endian), as the game's loader reads it (sub_82129348): type records of (areas + 5)
words (type, version, part count, self-relative item table pointer, item count, per area part
index << 24 | alignment, 0xFF000000 = none) up to a zero word; item entries (self-relative name
record pointer, one size per part); per area a 0x18-byte descriptor at the directory end + 0x14
whose +20 is a self-relative pointer to the area's data, the parts laid out from there in item
order, each at its type's alignment.

Everything read is bounds-checked; a damaged file raises CodError, never reads past its data.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Tuple

MAGIC = b"KAPF"
VERSION = 0x4003D70A
HEADER = 0x30
SECTION = 0x18
ENTRY = 0x20
DIR_ENTRY = 0x14
ALIGN, SMALL_ALIGN = 0x10000, 0x8000
NCH = b"NCH\0"
NCH_STEP = 0x80000
APKF = b"APKF"
NO_PART = 0xFF000000
TEX = 0x00584554           # the APKF type of textures (".xetex"); objects of 0x60 bytes + GPU data


class CodError(ValueError):
    pass


def _up(x: int, a: int) -> int:
    return (x + a - 1) // a * a if a > 1 else x


# ---------------------------------------------------------------------------
# LZO1X (decompression only; written from the format's description, every read checked)
# ---------------------------------------------------------------------------
def lzo1x_decompress(src: bytes, out_len: int) -> Tuple[bytes, int]:
    """(exactly out_len bytes, input position after the end marker)."""
    n = len(src)
    out = bytearray()
    ip = 0

    def byte() -> int:
        nonlocal ip
        if ip >= n:
            raise CodError("LZO: input ends early")
        b = src[ip]
        ip += 1
        return b

    def ext(base: int) -> int:
        nonlocal ip
        t = 0
        while True:
            if ip >= n:
                raise CodError("LZO: input ends early")
            if src[ip] != 0:
                break
            t += 255
            ip += 1
            if t > out_len + 255:
                raise CodError("LZO: length out of range")
        return t + base + byte()

    def literals(count: int) -> None:
        nonlocal ip
        if ip + count > n or len(out) + count > out_len:
            raise CodError("LZO: literal run out of range")
        out.extend(src[ip:ip + count])
        ip += count

    def copy(dist: int, count: int) -> None:
        start = len(out) - dist
        if start < 0 or dist <= 0 or len(out) + count > out_len:
            raise CodError("LZO: match out of range")
        if dist >= count:
            out.extend(out[start:start + count])
        else:
            for k in range(count):
                out.append(out[start + k])

    if n == 0:
        raise CodError("LZO: empty input")
    state, t = "loop", 0
    if src[0] > 17:
        ip, t = 1, src[0] - 17
        if t < 4:
            state = "match_next"
        else:
            literals(t)
            state = "first_literal_run"
    while True:
        if state == "loop":
            t = byte()
            if t >= 16:
                state = "match"
                continue
            if t == 0:
                t = ext(15)
            literals(t + 3)
            state = "first_literal_run"
        elif state == "first_literal_run":
            t = byte()
            if t >= 16:
                state = "match"
                continue
            copy(1 + 0x0800 + (t >> 2) + (byte() << 2), 3)
            state = "match_done"
        elif state == "match":
            if t >= 64:
                copy(1 + ((t >> 2) & 7) + (byte() << 3), (t >> 5) + 1)
            elif t >= 32:
                t &= 31
                if t == 0:
                    t = ext(31)
                lo, hi = byte(), byte()
                copy(1 + ((lo | (hi << 8)) >> 2), t + 2)
            elif t >= 16:
                far = (t & 8) << 11
                t &= 7
                if t == 0:
                    t = ext(7)
                lo, hi = byte(), byte()
                dist = far + ((lo | (hi << 8)) >> 2)
                if dist == 0:
                    break
                copy(dist + 0x4000, t + 2)
            else:
                copy(1 + (t >> 2) + (byte() << 2), 2)
            state = "match_done"
        elif state == "match_done":
            t = src[ip - 2] & 3
            state = "loop" if t == 0 else "match_next"
        else:   # match_next
            literals(t)
            t = byte()
            state = "match"
    if len(out) != out_len:
        raise CodError(f"LZO: {len(out)} bytes, {out_len} expected")
    return bytes(out), ip


# ---------------------------------------------------------------------------
# KAPF
# ---------------------------------------------------------------------------
@dataclass
class Chunk:
    entry: int              # file offset of its 0x20-byte entry
    offset: int
    size: int               # unpacked
    slot: int
    flags: int
    stored: int             # 0: stored as is


@dataclass
class DirEntry:
    name: str
    a: int
    b: int
    c: int


@dataclass
class Section:
    name: str
    chunks: List[Chunk] = field(default_factory=list)
    entries: List[DirEntry] = field(default_factory=list)


@dataclass
class Index:
    sections: List[Section]
    dir_end: int            # header 0x18: where the chunks may start
    size: int
    align: int

    def streamed(self) -> Optional[Section]:
        return next((s for s in self.sections if not s.name), None)


def _u32(b: bytes, o: int) -> int:
    if o < 0 or o + 4 > len(b):
        raise CodError(f"read past the end at {o:#x}")
    return struct.unpack_from("<I", b, o)[0]


def _cstr(b: bytes, o: int, lo: int, hi: int) -> str:
    if not lo <= o < hi:
        raise CodError(f"a name at {o:#x} outside the directory")
    z = b.find(b"\0", o, hi)
    if z < 0:
        raise CodError(f"a name at {o:#x} has no end")
    return b[o:z].decode("latin-1")


def read_index(f: bytes) -> Index:
    """The sections, chunk entries and directory entries of an archive's bytes."""
    if f[:4] != MAGIC or len(f) < HEADER:
        raise CodError("not a Call of Duty 3 archive (KAPF)")
    version, hsize, nsec, end = struct.unpack_from("<4I", f, 4)
    if version != VERSION or hsize != HEADER or not 0 <= nsec <= 64:
        raise CodError(f"odd KAPF header (version {version:#x}, {nsec} sections)")
    u14, u18 = struct.unpack_from("<2I", f, 0x14)
    if not HEADER <= end <= u14 <= u18 <= len(f):
        raise CodError("odd directory bounds")
    secs = []
    for i in range(nsec):
        r = HEADER + i * SECTION
        if r + SECTION > end:
            raise CodError("section records past the tables")
        name = f[r:r + 12].split(b"\0")[0].decode("latin-1")
        count, nent = struct.unpack_from("<2H", f, r + 12)
        table, dstart = struct.unpack_from("<2I", f, r + 16)
        s = Section(name)
        for k in range(count):
            e = table + k * ENTRY
            if not (HEADER <= e and e + ENTRY <= end):
                raise CodError("a chunk entry outside the tables")
            off, size, slot, flags, stored = (_u32(f, e + 4 * j) for j in range(5))
            if off < u18 or off + (stored or size) > len(f):
                raise CodError(f"section {name!r} chunk {k} outside the file")
            s.chunks.append(Chunk(e, off, size, slot, flags, stored))
        if not (end <= dstart and dstart + DIR_ENTRY * nent <= u14):
            raise CodError(f"section {name!r}: directory outside {end:#x}-{u14:#x}")
        for k in range(nent):
            no, _po, a, b, c = struct.unpack_from("<5I", f, dstart + DIR_ENTRY * k)
            s.entries.append(DirEntry(_cstr(f, no, end, u14), a, b, c))
        secs.append(s)
    chunks = sorted((c.offset, c.stored or c.size) for s in secs for c in s.chunks)
    align = ALIGN
    for e, st in zip([u18] + [o + n for o, n in chunks], [o for o, _ in chunks] + [len(f)]):
        if st == _up(e, SMALL_ALIGN) != _up(e, ALIGN):
            align = SMALL_ALIGN
    return Index(secs, u18, len(f), align)


def unpack(f: bytes, c: Chunk) -> bytes:
    """A chunk's bytes, unpacked."""
    if c.stored == 0:
        return bytes(f[c.offset:c.offset + c.size])
    out = bytearray(c.size)
    filled, pos, stop = 0, c.offset, c.offset + c.stored
    while pos < stop:
        h = f[pos:pos + 0x20]
        if len(h) < 0x20 or h[:4] != NCH:
            raise CodError(f"no NCH block at {pos:#x}")
        packed, _c1, size, out_off, _c2, block, one = struct.unpack_from(">7I", h, 4)
        if one != 1 or block != 0x20 + packed or out_off + size > c.size or pos + block > stop:
            raise CodError(f"odd NCH block at {pos:#x}")
        data, used = lzo1x_decompress(f[pos + 0x20:pos + 0x20 + packed], size)
        if used != packed:
            raise CodError(f"NCH block at {pos:#x}: the stream ends early")
        out[out_off:out_off + size] = data
        filled += size
        pos += NCH_STEP
    if filled != c.size:
        raise CodError(f"NCH blocks give {filled} bytes, the chunk says {c.size}")
    return bytes(out)


def replace_chunks(f: bytes, idx: Index, new: Dict[Tuple[int, int], bytes]) -> bytes:
    """The archive with the chunks new[(section, chunk)] replaced by those bytes (stored as
    they are) and every other chunk's bytes kept exactly as they were (packed or not); the
    header, tables and directories are kept, but for the moved chunks' entries."""
    for (si, ci), data in new.items():
        c = idx.sections[si].chunks[ci]
        if len(data) > c.slot:
            raise CodError(f"chunk {si}/{ci}: {len(data):#x} bytes for a slot of {c.slot:#x}")
    head = bytearray(f[:idx.dir_end])
    order = sorted(((c.offset, si, ci) for si, s in enumerate(idx.sections) for ci, c in enumerate(s.chunks)))
    body = []
    at = _up(idx.dir_end, idx.align)
    for _, si, ci in order:
        c = idx.sections[si].chunks[ci]
        if (si, ci) in new:
            data, size, stored = new[(si, ci)], len(new[(si, ci)]), 0
        else:
            data, size, stored = f[c.offset:c.offset + (c.stored or c.size)], c.size, c.stored
        struct.pack_into("<5I", head, c.entry, at, size, c.slot, c.flags, stored)
        body.append((at, data))
        at = _up(at + len(data), idx.align)
    out = bytearray(head)
    for off, data in body:
        out += bytes(off - len(out))
        out += data
    out += bytes(_up(len(out), idx.align) - len(out))
    return bytes(out)


# ---------------------------------------------------------------------------
# APKF
# ---------------------------------------------------------------------------
@dataclass
class Item:
    type: int
    record: int             # type record index (C >> 16)
    index: int              # in its type's table (C & 0xFFFF)
    name: str               # as stored in the name pool (cut to 27 characters)
    parts: List[Tuple[int, int]]   # (start in the chunk, size) per area; (0, 0) where it has none


def apkf_items(ch: bytes) -> List[Item]:
    """Every item of an APKF chunk with where its bytes are, laid out as the game's loader does."""
    if ch[:4] != APKF or len(ch) < 0x20:
        raise CodError("not an APKF chunk")

    def be(o: int) -> int:
        if o < 0 or o + 4 > len(ch):
            raise CodError(f"APKF: read past the end at {o:#x}")
        return struct.unpack_from(">I", ch, o)[0]

    areas, dir_end = be(0x10), be(0x14)
    if areas not in (1, 2) or not 0x1C < dir_end < len(ch):
        raise CodError("APKF: odd header")
    rec, o, first, types = (areas + 5) * 4, 0x1C, None, []
    while first is None or o + rec <= first:
        w = [be(o + 4 * i) for i in range(areas + 5)]
        tbl = (o + 12 + w[3]) & 0xFFFFFFFF
        if not o + rec <= tbl < dir_end:
            raise CodError("APKF: odd type record")
        first = tbl if first is None else min(first, tbl)
        types.append((w, tbl))
        o += rec
    base = []
    for k in range(areas):
        d = dir_end + 0x14 + 0x18 * k
        start = (d + 20 + be(d + 20)) & 0xFFFFFFFF
        size = be(d + 16)
        if not 0 < start <= len(ch) or start + size > len(ch):
            raise CodError("APKF: an area outside the chunk")
        base.append((start, size))
    cursor = [0] * areas
    out = []
    for r, (w, tbl) in enumerate(types):
        nparts, count = w[2], w[4]
        if not 1 <= nparts <= areas:
            raise CodError("APKF: odd part count")
        for i in range(count):
            e = tbl + i * (nparts + 1) * 4
            nm = (e + be(e)) & 0xFFFFFFFF
            if not 0 <= nm < dir_end:
                raise CodError("APKF: a name outside the pool")
            # (the last record can run past the pool into the text at the directory end)
            z = ch.find(b"\0", nm + 4, min(len(ch), nm + 4 + 256))
            name = ch[nm + 4:z if z >= 0 else nm + 4].decode("latin-1")
            sizes = [be(e + 4 + 4 * j) for j in range(nparts)]
            parts = []
            for k in range(areas):
                word = w[5 + k]
                if word == NO_PART:
                    parts.append((0, 0))
                    continue
                p = word >> 24
                if p >= nparts:
                    raise CodError("APKF: odd area word")
                cursor[k] = _up(cursor[k], word & 0xFFFFFF)
                start = base[k][0] + cursor[k]
                cursor[k] += sizes[p]
                if cursor[k] > base[k][1]:
                    raise CodError("APKF: an item past its area")
                parts.append((start, sizes[p]))
            out.append(Item(w[0], r, i, name, parts))
    return out


def texture_shape(obj: bytes) -> Optional[Tuple[int, ...]]:
    """A texture object's fetch constant without its GPU address: size, format, tiling, mips."""
    if len(obj) < 0x60:
        return None
    w = struct.unpack_from(">6I", obj, 0x48)
    return (w[0], w[1] & 0xFFF, w[2], w[3], w[4], w[5] & 0xFFF)
