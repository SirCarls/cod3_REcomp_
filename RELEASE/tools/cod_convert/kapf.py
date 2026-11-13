"""[acct2-decomp] Call of Duty 3 `.cod` archives (KAPF, version 2.06): read and write, in plain Python.

The layout is docs/formats/cod3.md (all header integers little-endian):
  header 0x30: "KAPF", version 0x4003D70A, header size 0x30, section count, end of the chunk
               tables (= start of the directories), end of the directories and names (= start
               of the source paths), end of the source paths, builder name char[20]
  section records 0x18: name char[12], u16 chunk count, u16 directory entry count, u32 table
               offset, u32 directory offset
  chunk entries 0x20: u32 file offset (aligned: see Archive.align), u32 unpacked size, u32 slot size,
               u32 flags, u32 stored size (0 = stored as is, else NCH/LZO1X blocks), 3 x u32 0
A packed chunk is NCH blocks, one every 512 KB inside the chunk, each a 0x20-byte big-endian
header ("NCH\\0", packed size, checksum, unpacked size, output offset, checksum, block size,
1) and an LZO1X stream.

The directories (header 0x10 to 0x18; the game reads them, sub_82462790): for each section in
record order, 4-aligned, its entries (0x14 bytes: u32 name offset, u32 source path offset, u32 A,
u32 B, u32 C, offsets from the file's start), then those entries' names (NUL-terminated, each
4-aligned, zero fill); header 0x14 is the 4-aligned end of that. Then one source path per entry in
the same order (NUL-terminated, each 4-aligned, never shared), up to header 0x18 (4-aligned).
Every retail archive's directories are rebuilt byte for byte from their entries this way.
  B: the item's chunk in its section. C: in the unnamed (streamed) section, the APKF type record
  << 16 | the item's index; in a named section, the item's byte offset in chunk B.
  A: the size the item's loader is given. Named sections: the item's length; streamed items:
  the sum of the item's APKF part sizes for most types, textures (.xetex) the GPU part + 0x10010
  (0x60-byte header part), fonts the sum + 0xC0; a few types (meshes, .bpeb, .xefx, .xecell) have
  no loader on that path and keep whatever A they had (dev notes, acct2-decomp 2026-11-05).
Named (raw) sections hold one chunk (slot = its size, flags 0) whose items sit at NAMED_ALIGN
steps from 0 in directory order; new_named_section() builds one.

The writer stores every chunk as is (stored size 0): the game reads such chunks, and no
compressor is needed. Everything read is bounds-checked.
"""
from __future__ import annotations

import os
import struct
from dataclasses import dataclass, field
from typing import List, Optional, Sequence, Tuple

from lzo1x import LzoError, decompress

MAGIC = b"KAPF"
VERSION = 0x4003D70A
HEADER = 0x30
SECTION = 0x18
ENTRY = 0x20
DIR_ENTRY = 0x14
ALIGN = 0x10000                 # chunks start, and the file ends, on this boundary (zero filled)
SMALL_ALIGN = 0x8000            # ... or this one: see Archive.align
NCH = b"NCH\0"
NCH_STEP = 0x80000
# Named sections: where each item starts in the section's one chunk (None: one item, the chunk).
NAMED_ALIGN = {"ANIM": 0x8000, "DTEX": 0x10000, "AUDIO": 4, "FLI": None, "STRING": None}


class KapfError(ValueError):
    pass


@dataclass
class Chunk:
    data: bytes                     # unpacked
    slot: int = 0x300000
    flags: int = 0x82
    # as read (not written):
    offset: int = 0
    stored: int = 0
    size: int = 0                   # unpacked size from the entry (also when not unpacked)


@dataclass
class Entry:
    """One item of a section's directory (see the module text for A, B and C)."""
    name: str                       # as stored (latin-1, no NUL)
    source: str                     # the builder's source path, as stored
    a: int
    b: int
    c: int


@dataclass
class Section:
    name: str
    unknown14: int = 0              # as read: the directory's entry count (written from entries)
    unknown20: int = 0              # as read: the directory's start, relative to the tables' end
    chunks: List[Chunk] = field(default_factory=list)
    entries: Optional[List[Entry]] = None


@dataclass
class Archive:
    sections: List[Section]
    builder: str = "CODAUTO32"
    # The bytes from the tables' end to header 0x18, as read. write() builds them from the
    # sections' entries instead when every section has its entries (read() fills them when
    # rebuilding gives the same bytes, which holds for every retail archive).
    directory: bytes = b""
    dir_end: int = 0               # header 0x14, relative to the directory's start
    size: int = 0
    # Where chunks start and where the file ends: the previous end rounded up to this, zero
    # filled. Retail MP archives use 64 KB, except those with one unnamed section (anim.cod,
    # the mp_merv zones, casdyn.cod: 12 of 81), which use 32 KB. read() finds which.
    align: int = ALIGN


def _u32(b: bytes, o: int) -> int:
    if o < 0 or o + 4 > len(b):
        raise KapfError(f"read past the end at {o:#x}")
    return struct.unpack_from("<I", b, o)[0]


def _up(x: int, a: int) -> int:
    return (x + a - 1) // a * a


def _cstr(b: bytes, o: int, lo: int, hi: int) -> str:
    if not lo <= o < hi:
        raise KapfError(f"a directory string at {o:#x} outside {lo:#x}-{hi:#x}")
    z = b.find(b"\0", o, hi)
    if z < 0:
        raise KapfError(f"a directory string at {o:#x} has no end")
    return b[o:z].decode("latin-1")


def encode_directory(sections: Sequence[Section], base: int) -> Tuple[bytes, List[int], int]:
    """The directory bytes for sections' entries, placed at file offset base (the tables' end).
    Returns (bytes up to header 0x18, each section's directory start relative to base,
    header 0x14 relative to base)."""
    starts: List[int] = []
    names: List[List[int]] = []
    pos = base
    for s in sections:
        if s.entries is None:
            raise KapfError(f"section {s.name!r} has no entries")
        if len(s.entries) > 0xFFFF:
            raise KapfError(f"section {s.name!r}: too many entries")
        pos = _up(pos, 4)
        starts.append(pos - base)
        pos += DIR_ENTRY * len(s.entries)
        row = []
        for e in s.entries:
            pos = _up(pos, 4)
            row.append(pos)
            pos += len(e.name.encode("latin-1")) + 1
        names.append(row)
    u14 = _up(pos, 4)
    pos = u14
    paths: List[List[int]] = []
    for s in sections:
        row = []
        for e in s.entries:
            pos = _up(pos, 4)
            row.append(pos)
            pos += len(e.source.encode("latin-1")) + 1
        paths.append(row)
    buf = bytearray(_up(pos, 4) - base)
    for s, st, nr, pr in zip(sections, starts, names, paths):
        for k, e in enumerate(s.entries):
            for v in (e.a, e.b, e.c):
                if not 0 <= v <= 0xFFFFFFFF:
                    raise KapfError(f"section {s.name!r} entry {k}: a field out of range")
            nm, src = e.name.encode("latin-1"), e.source.encode("latin-1")
            if b"\0" in nm or b"\0" in src:
                raise KapfError(f"section {s.name!r} entry {k}: NUL in a name")
            struct.pack_into("<5I", buf, st + DIR_ENTRY * k, nr[k], pr[k], e.a, e.b, e.c)
            buf[nr[k] - base:nr[k] - base + len(nm)] = nm
            buf[pr[k] - base:pr[k] - base + len(src)] = src
    return bytes(buf), starts, u14 - base


def new_named_section(name: str, items: Sequence[Tuple[str, str, bytes]]) -> Section:
    """A named (raw) section from (item name, source path, bytes) in order: one chunk, each
    item at the section's NAMED_ALIGN step, A = its length, B = 0, C = its offset."""
    if name not in NAMED_ALIGN:
        raise KapfError(f"unknown named section {name!r} (known: {', '.join(NAMED_ALIGN)})")
    step = NAMED_ALIGN[name]
    if step is None and len(items) != 1:
        raise KapfError(f"section {name!r} holds one item")
    data = bytearray()
    entries = []
    for nm, src, b in items:
        at = _up(len(data), step) if step else 0
        data += bytes(at - len(data)) + b
        entries.append(Entry(nm, src, len(b), 0, at))
    d = bytes(data)
    return Section(name, len(entries), 0, [Chunk(d, len(d), 0, size=len(d))], entries)


def unpack_chunk(f: bytes, off: int, size: int, stored: int) -> bytes:
    if stored == 0:
        if off + size > len(f):
            raise KapfError("a stored chunk runs past the file")
        return f[off:off + size]
    if off + stored > len(f):
        raise KapfError("a packed chunk runs past the file")
    out = bytearray(size)
    filled = 0
    pos = off
    while pos < off + stored:
        h = f[pos:pos + 0x20]
        if len(h) < 0x20 or h[:4] != NCH:
            raise KapfError(f"no NCH block at {pos:#x}")
        packed, _ck1, unpacked, out_off, _ck2, block, one = struct.unpack_from(">7I", h, 4)
        if one != 1 or block != 0x20 + packed or out_off + unpacked > size or pos + block > off + stored:
            raise KapfError(f"odd NCH block at {pos:#x}")
        try:
            data, end = decompress(f[pos + 0x20:pos + 0x20 + packed], unpacked, with_end=True)
        except LzoError as e:
            raise KapfError(f"NCH block at {pos:#x}: {e}") from e
        if end != packed:
            raise KapfError(f"NCH block at {pos:#x}: the stream ends at {end}, the block says {packed}")
        out[out_off:out_off + unpacked] = data
        filled += unpacked
        pos += NCH_STEP
    if filled != size:
        raise KapfError(f"NCH blocks give {filled} bytes, the chunk says {size}")
    return bytes(out)


def read(path: str, unpack: bool = True) -> Archive:
    """unpack=False reads only the header, tables and directories (chunk data stays b"")."""
    fsize = os.path.getsize(path)
    with open(path, "rb") as fh:
        f = fh.read() if unpack else fh.read(HEADER)
        if not unpack and f[:4] == MAGIC and len(f) == HEADER:
            f += fh.read(max(0, min(fsize, struct.unpack_from("<I", f, 0x18)[0]) - HEADER))
    if f[:4] != MAGIC:
        raise KapfError("not a KAPF archive")
    version, hsize, nsec, _end = struct.unpack_from("<4I", f, 4)
    if version != VERSION or hsize != HEADER:
        raise KapfError(f"not a Call of Duty 3 archive (version {version:#x}, header {hsize:#x})")
    if not 0 <= nsec <= 64:          # 0: an empty archive (3 retail SP ones: a header, 32 KB of zeros)
        raise KapfError(f"odd section count {nsec}")
    u14, u18 = struct.unpack_from("<2I", f, 0x14)
    builder = f[0x1C:0x30].split(b"\0")[0].decode("latin-1")
    secs = []
    for i in range(nsec):
        r = HEADER + i * SECTION
        if r + SECTION > len(f):
            raise KapfError("section records run past the file")
        name = f[r:r + 12].split(b"\0")[0].decode("latin-1")
        count, s14 = struct.unpack_from("<2H", f, r + 12)
        table, s20 = struct.unpack_from("<2I", f, r + 16)
        sec = Section(name, s14, s20)
        for k in range(count):
            e = table + k * ENTRY
            off, size, slot, flags, stored = (_u32(f, e + 4 * j) for j in range(5))
            if off + (stored or size) > fsize:
                raise KapfError(f"section {name!r} chunk {k} runs past the file")
            data = unpack_chunk(f, off, size, stored) if unpack else b""
            sec.chunks.append(Chunk(data, slot, flags, off, stored, size))
        secs.append(sec)
    end = _end
    if not end <= u14 <= u18 <= fsize:
        raise KapfError(f"odd directory bounds {end:#x} {u14:#x} {u18:#x}")
    for sec in secs:
        if not end <= sec.unknown20 <= u14:
            raise KapfError(f"section {sec.name!r}: directory at {sec.unknown20:#x} outside {end:#x}-{u14:#x}")
    first = min((c.offset for s in secs for c in s.chunks), default=fsize)
    if u18 > first:
        raise KapfError("the directory runs into the first chunk")
    directory = bytes(f[end:u18])
    try:                                # the entries, when laid out the usual way (see above)
        for sec in secs:
            if sec.unknown20 + DIR_ENTRY * sec.unknown14 > u14:
                raise KapfError(f"section {sec.name!r}: directory runs past {u14:#x}")
            sec.entries = []
            for k in range(sec.unknown14):
                no, po, a, b, c = struct.unpack_from("<5I", f, sec.unknown20 + DIR_ENTRY * k)
                sec.entries.append(Entry(_cstr(f, no, end, u14), _cstr(f, po, u14, u18), a, b, c))
        if secs:
            rebuilt, starts, r14 = encode_directory(secs, end)
            if rebuilt != directory or r14 != u14 - end or starts != [s.unknown20 - end for s in secs]:
                raise KapfError("directory not in the usual layout")
    except KapfError:                   # write() then keeps the directory bytes as read
        for sec in secs:
            sec.entries = None
    for sec in secs:
        sec.unknown20 -= end           # kept relative to the directory's start
    return Archive(secs, builder, directory, u14 - end, fsize, _alignment(secs, u18, fsize))


def _alignment(secs: List[Section], head_end: int, size: int) -> int:
    """SMALL_ALIGN when some chunk start (or the file's end) is only 32 KB rounded, else ALIGN."""
    ch = sorted((c.offset, c.stored or c.size) for s in secs for c in s.chunks)
    ends = [head_end] + [o + n for o, n in ch]
    starts = [o for o, _ in ch] + [size]
    for e, s in zip(ends, starts):
        if s == _up(e, SMALL_ALIGN) != _up(e, ALIGN):
            return SMALL_ALIGN
    return ALIGN


def write(arc: Archive, path: str) -> int:
    """Writes arc with every chunk stored as is; returns the file size. The directories come
    from the sections' entries when all have them, else from arc.directory as read."""
    nsec = len(arc.sections)
    if not 0 <= nsec <= 64:
        raise KapfError("0 to 64 sections")
    tables = HEADER + nsec * SECTION
    pos = tables
    table_of = []
    for s in arc.sections:
        if len(s.chunks) > 0xFFFF:
            raise KapfError("too many chunks")
        table_of.append(pos)
        pos += ENTRY * len(s.chunks)
    end_tables = pos
    if nsec and all(s.entries is not None for s in arc.sections):
        directory, starts, dir_end = encode_directory(arc.sections, end_tables)
        counts = [len(s.entries) for s in arc.sections]
    else:
        directory, dir_end = arc.directory, arc.dir_end
        starts = [s.unknown20 for s in arc.sections]
        counts = [s.unknown14 for s in arc.sections]
    head = bytearray()
    head += MAGIC + struct.pack("<4I", VERSION, HEADER, nsec, end_tables)
    head += struct.pack("<2I", end_tables + dir_end, end_tables + len(directory))
    head += arc.builder.encode("latin-1")[:19].ljust(20, b"\0")
    recs = bytearray()
    ents = bytearray()
    body = []
    if arc.align not in (ALIGN, SMALL_ALIGN):
        raise KapfError(f"alignment {arc.align:#x}: 64 KB or 32 KB")
    at = _up(end_tables + len(directory), arc.align)
    for s, t, st, n in zip(arc.sections, table_of, starts, counts):
        nm = s.name.encode("latin-1")
        if len(nm) > 11:
            raise KapfError(f"section name too long: {s.name}")
        recs += nm.ljust(12, b"\0") + struct.pack("<2H2I", len(s.chunks), n, t, end_tables + st)
        for c in s.chunks:
            if len(c.data) > c.slot:
                raise KapfError("a chunk is bigger than its slot")
            ents += struct.pack("<8I", at, len(c.data), c.slot, c.flags, 0, 0, 0, 0)
            body.append((at, c.data))
            at = _up(at + len(c.data), arc.align)
    with open(path, "wb") as w:
        w.write(head + recs + ents + directory)
        for off, data in body:
            w.seek(off)
            w.write(data)
        end = body[-1][0] + len(body[-1][1]) if body else end_tables + len(directory)
        end = _up(end, arc.align)
        w.truncate(end)
    return end
