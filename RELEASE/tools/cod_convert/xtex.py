"""[acct2-decomp] Textures in a `.cod`: the TEX items (type 0x00584554) of the unnamed section's APKF
chunks, read as pictures and written from pictures, in plain Python (no numpy, no PIL).

A TEX item has two parts. The object, 0x60 bytes (docs/formats/cod3.md): +0x00 width, height (BE u32),
+0x2C the D3D header, +0x48 the fetch constant, six BE words, fields as Xenia names them:
  dword0 bit 31 tiled; 22:30 pitch = the tiled row in texels / 32 (blocks per row rounded up to 32,
         times the block width: true for every 2D texture of stbert and 12 MP archives, 1,271)
  dword1 0:5 format (18 DXT1, 19 DXT3, 20 DXT5, 6 8888, ...); 6:7 endian (1 = 8-in-16 swapped words,
         every DXT texture seen)
  dword2 0:12 width - 1; 13:25 height - 1
  dword4 2:5 lowest, 6:9 highest mip level
  dword5 9:10 dimension (1 = 2D); 11 packed mips; 12:31 mip address >> 12, an offset in the GPU part
         (the base level's size)
The GPU part, the texels (measured on the retail textures and checked in the game, acct2-decomp
NOTES 2026-11-05 16:16; layout()): every level a tiled surface (Xenos 2D tiling as Xenia and crunch
write it: 32 x 32 block macro tiles; blocks per row and rows rounded up to 32), back to back from 0:
the base level, then levels 1.. (from the mip address), until the first level whose shorter side is
16 texels or less (with packed mips); that level and every one after it share one surface the size
of that first one, each at the place Xenia's GetPackedMipOffset gives. A texture whose base level
is that small has every level in that one surface and mip address 0. The part is rounded up to 4 KB,
and a texture whose GPU part is not exactly that size is not read or written. (stbert.cod and the 81
MP archives: all 7,169 textures this reads have exactly that size; of 37,613 levels of 128 texels or
less, 99.8 % are within 24 per channel of the level above shrunk 2 x 2.) In the archive directory a texture's
entry (NAME.xetex) has A = GPU part + 0x10010 (apkf.directory_a).

  xtex.py list IN.cod
        every texture: its place (chunk:type record:item), size, format, mip levels, GPU part size,
        name (the directory entry's; '-' for one reached only through pointers)
  xtex.py export IN.cod NAME OUT.png [--level N] [--chunk N]
        one level (default 0) as an RGBA PNG; DXT1, DXT3, DXT5, and 8888 read as A, R, G, B bytes (that
        order not checked against the game)
  xtex.py dds IN.cod OUTDIR [NAME ...] [--chunk N]
        textures (all of them without NAME) as standard DDS files, every level, untiled, the blocks as
        a PC reads them (DXT1, DXT3, DXT5; others skipped and counted): OUTDIR/NAME.dds (the place
        CHUNK_RECORD_ITEM.dds for a texture without a name). Nothing is converted, so nothing is lost.
  xtex.py import IN.cod OUT.cod NAME PICTURE.png [NAME PICTURE.png ...] [--no-mips]
                [--alpha auto|cutout|plain] [--chunk N]
        each picture written over texture NAME in its format when that is DXT1, DXT3 or DXT5 (others
        are refused): any power-of-two size from 4 to 4096 a side. A new size changes the object's
        size and fetch fields, the GPU part and the directory's A; mip levels are made with a 2 x 2
        box filter down to 1 x 1 (--no-mips: the base level only). A cut-out picture (an alpha
        mask: --alpha cutout, or with auto at least 1 texel in 64 under half alpha and 70 % of the
        alpha near 0 or 255) keeps its coverage in every level: each level's alpha is scaled so the
        share of texels at or over half alpha is the top level's (box filtering alone thins a mask
        level by level until it is gone; acct2-ocr NETTING.md). A chunk that would outgrow its slot
        is refused, and then nothing is written.
NAME: the texture's name as `list` prints it, with or without .xetex, or its place CHUNK:RECORD:ITEM;
with two textures of one name, --chunk picks one. IN is only read; only OUT is written. Pictures: 8-bit or 16-bit PNG,
grey, grey + alpha, RGB, RGBA or palette, not interlaced.
"""
from __future__ import annotations

import array
import os
import struct
import sys
import zlib
from typing import Dict, List, Optional, Sequence, Tuple

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import apkf  # noqa: E402
import kapf  # noqa: E402

TEX = 0x00584554
DXT1, DXT3, DXT5, ARGB8 = 18, 19, 20, 6
# format -> (block width, block height, bytes per block, name)
FORMATS = {2: (1, 1, 1, "8"), 3: (1, 1, 2, "1555"), 4: (1, 1, 2, "565"), 5: (1, 1, 2, "655"),
           6: (1, 1, 4, "8888"), 7: (1, 1, 4, "2_10_10_10"), 8: (1, 1, 1, "8_A"), 9: (1, 1, 1, "8_B"),
           10: (1, 1, 2, "88"), 14: (1, 1, 4, "8888_A"), 15: (1, 1, 2, "4444"), 16: (1, 1, 4, "10_11_11"),
           17: (1, 1, 4, "11_11_10"), 18: (4, 4, 8, "DXT1"), 19: (4, 4, 16, "DXT3"),
           20: (4, 4, 16, "DXT5"), 24: (1, 1, 2, "16"), 25: (1, 1, 4, "16_16"),
           26: (1, 1, 8, "16x4"), 30: (1, 1, 2, "16F"), 31: (1, 1, 4, "16_16F"),
           32: (1, 1, 8, "16x4F"), 33: (1, 1, 4, "32"), 36: (1, 1, 4, "32F"), 49: (4, 4, 16, "DXN"),
           58: (4, 4, 8, "DXT3A"), 59: (4, 4, 8, "DXT5A"), 60: (4, 4, 8, "CTX1")}
READABLE = (DXT1, DXT3, DXT5, ARGB8)
WRITABLE = (DXT1, DXT3, DXT5)


class XtexError(ValueError):
    pass


def up(x: int, a: int) -> int:
    return (x + a - 1) // a * a


def log2(x: int) -> int:
    return x.bit_length() - 1


# -- fields ---------------------------------------------------------------------------------

class Fields:
    def __init__(self, obj: bytes):
        if len(obj) != 0x60:
            raise XtexError("a texture object is 0x60 bytes")
        self.obj = bytes(obj)
        self.w, self.h = struct.unpack_from(">2I", obj, 0)
        self.f = list(struct.unpack_from(">6I", obj, 0x48))
        f = self.f
        self.tiled = f[0] >> 31
        self.pitch = (f[0] >> 22) & 0x1FF
        self.fmt = f[1] & 0x3F
        self.endian = (f[1] >> 6) & 3
        self.fw, self.fh = (f[2] & 0x1FFF) + 1, ((f[2] >> 13) & 0x1FFF) + 1
        self.mip_lo, self.mip_hi = (f[4] >> 2) & 15, (f[4] >> 6) & 15
        self.dim = (f[5] >> 9) & 3
        self.packed = (f[5] >> 11) & 1
        self.mip_addr = (f[5] >> 12) << 12

    @property
    def fmt_name(self) -> str:
        return FORMATS[self.fmt][3] if self.fmt in FORMATS else f"format {self.fmt}"

    def plain(self) -> Optional[str]:
        """None when this module can lay the texture out, else why not."""
        if self.dim != 1:
            return "not 2D"
        if not self.tiled:
            return "not tiled"
        if self.fmt not in FORMATS:
            return f"format {self.fmt} unknown"
        if (self.fw, self.fh) != (self.w, self.h):
            return "object size is not the fetch size"
        if self.fw & (self.fw - 1) or self.fh & (self.fh - 1):
            return "not a power of two"
        if self.mip_lo != 0:
            return "lowest mip level is not 0"
        if self.mip_hi and not self.packed:
            return "mip levels not packed"
        bw, bh, bpb, _ = FORMATS[self.fmt]
        if self.pitch * 32 != up(-(-self.fw // bw), 32) * bw:
            return "pitch not as expected"
        if self.mip_hi:
            want = layout(self.fw, self.fh, self.mip_hi, self.fmt, self.packed)[0][1][3]
            if self.mip_addr != want:
                return f"mip address {self.mip_addr:#x}, the layout's {want:#x}"
        return None

    def gpu_size(self) -> int:
        return up(layout(self.fw, self.fh, self.mip_hi, self.fmt, self.packed)[2], 4096)

    def with_size(self, w: int, h: int, top: int) -> bytes:
        """The object for a w x h texture of the same format with levels 0..top."""
        bw, bh, bpb, _ = FORMATS[self.fmt]
        f = list(self.f)
        f[0] = (f[0] & ~(0x1FF << 22)) | ((up(-(-w // bw), 32) * bw // 32) << 22)
        f[2] = (f[2] & ~0x3FFFFFF) | (w - 1) | ((h - 1) << 13)
        f[4] = (f[4] & ~(0xFF << 2)) | (top << 6)
        f[5] &= ~((1 << 11) | 0xFFFFF000)
        if top:
            f[5] |= (1 << 11) | layout(w, h, top, self.fmt, True)[0][1][3]
        new = bytearray(self.obj)
        struct.pack_into(">2I", new, 0, w, h)
        struct.pack_into(">6I", new, 0x48, *f)
        return bytes(new)


# -- Xenos 2D tiling --------------------------------------------------------------------------

def _outer(y: int, pitch: int, lb: int) -> int:
    macro = ((y >> 5) * (pitch >> 5)) << (lb + 7)
    micro = ((y & 6) << 2) << lb
    return macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 8) << (3 + lb)) + ((y & 1) << 4)


def _inner(x: int, y: int, base: int, lb: int) -> int:
    macro = (x >> 5) << (lb + 7)
    micro = (x & 7) << lb
    off = base + macro + ((micro & ~0xF) << 1) + (micro & 0xF)
    return (((off & ~0x1FF) << 3) + ((off & 0x1C0) << 2) + (off & 0x3F) + ((y & 16) << 7)
            + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6))


def untile(data: bytes, at: int, wb: int, hb: int, bpb: int) -> bytes:
    """wb x hb blocks of a tiled surface at data[at:], row by row."""
    lb = log2(bpb)
    pitch = up(wb, 32)
    out = bytearray(wb * hb * bpb)
    for y in range(hb):
        base = _outer(y, pitch, lb)
        d = y * wb * bpb
        for x in range(wb):
            s = at + (_inner(x, y, base, lb) >> lb) * bpb
            out[d:d + bpb] = data[s:s + bpb]
            d += bpb
    return bytes(out)


def tile(lin: bytes, wb: int, hb: int, bpb: int) -> bytes:
    """Row-by-row blocks -> the tiled surface (blocks per row and rows rounded up to 32)."""
    lb = log2(bpb)
    pitch = up(wb, 32)
    out = bytearray(pitch * up(hb, 32) * bpb)
    for y in range(hb):
        base = _outer(y, pitch, lb)
        s = y * wb * bpb
        for x in range(wb):
            d = (_inner(x, y, base, lb) >> lb) * bpb
            out[d:d + bpb] = lin[s:s + bpb]
            s += bpb
    return bytes(out)


def blocks(w: int, h: int, fmt: int) -> Tuple[int, int]:
    bw, bh, _, _ = FORMATS[fmt]
    return -(-w // bw), -(-h // bh)


def surface_size(w: int, h: int, fmt: int) -> int:
    wb, hb = blocks(w, h, fmt)
    return up(wb, 32) * up(hb, 32) * FORMATS[fmt][2]


def layout(w: int, h: int, top: int, fmt: int, packed: bool):
    """Where levels 0..top sit in the GPU part: ([(level, width, height, at, block x, block y,
    shared)], shared surface, size). A level not shared is a tiled surface of its own at `at`; the
    shared levels sit in one tiled surface, (at, blocks per row, rows) of the first of them (rounded
    up to 32 when tiled), each at (block x, block y) in it. size: the bytes the levels take."""
    bw, bh, bpb, _ = FORMATS[fmt]
    lw2, lh2 = log2(w), log2(h)
    base_pm = max(0, min(lw2, lh2) - 4)
    out, off, tail = [], 0, None
    for lv in range(top + 1):
        lw, lh = max(1, w >> lv), max(1, h >> lv)
        wb, hb = blocks(lw, lh, fmt)
        if packed and lv >= base_pm:
            if tail is None:
                tail = (off, wb, hb)
                off += up(wb, 32) * up(hb, 32) * bpb
            pm = lv - base_pm
            if pm < 3:
                x, y = (0, 16 >> pm) if lw2 > lh2 else (16 >> pm, 0)
            elif lw2 > lh2:
                x, y = (1 << (lw2 - base_pm)) >> (pm - 2), 0
            else:
                x, y = 0, (1 << (lh2 - base_pm)) >> (pm - 2)
            out.append((lv, lw, lh, tail[0], x // bw, y // bh, True))
        else:
            out.append((lv, lw, lh, off, 0, 0, False))
            off += up(wb, 32) * up(hb, 32) * bpb
    return out, tail, off


def read_level_blocks(fl: Fields, gpu: bytes, level: int) -> Tuple[int, int, bytes]:
    """(width, height, the level's blocks row by row, as stored: 8-in-16 swapped for endian 1)"""
    why = fl.plain()
    if why:
        raise XtexError(f"this texture can't be laid out: {why}")
    if not 0 <= level <= fl.mip_hi:
        raise XtexError(f"level {level}: the texture has levels 0-{fl.mip_hi}")
    fmt = fl.fmt
    bpb = FORMATS[fmt][2]
    lay, tail, size = layout(fl.fw, fl.fh, fl.mip_hi, fmt, fl.packed)
    if up(size, 4096) != len(gpu):
        raise XtexError(f"the GPU part is {len(gpu):#x} bytes, the layout's {up(size, 4096):#x}")
    lv, lw, lh, at, bx, by, shared = lay[level]
    wb, hb = blocks(lw, lh, fmt)
    if not shared:
        return lw, lh, untile(gpu, at, wb, hb, bpb)
    pitch, rows = up(tail[1], 32), up(tail[2], 32)
    grid = untile(gpu, at, pitch, rows, bpb)
    return lw, lh, b"".join(grid[((by + j) * pitch + bx) * bpb:((by + j) * pitch + bx + wb) * bpb]
                            for j in range(hb))


def build_gpu(fmt: int, levels: Sequence[Tuple[int, int, bytes]], packed: bool = True) -> bytes:
    """levels[0..top]: (width, height, blocks row by row, as stored) -> the GPU part"""
    bpb = FORMATS[fmt][2]
    w, h, _ = levels[0]
    lay, tail, size = layout(w, h, len(levels) - 1, fmt, packed)
    out = bytearray(up(size, 4096))
    grid = None
    for (lv, lw, lh, at, bx, by, shared), (vw, vh, data) in zip(lay, levels):
        if (vw, vh) != (lw, lh):
            raise XtexError(f"level {lv} is {vw}x{vh}, not {lw}x{lh}")
        wb, hb = blocks(lw, lh, fmt)
        if len(data) != wb * hb * bpb:
            raise XtexError(f"level {lv}: {len(data)} bytes of blocks, not {wb * hb * bpb}")
        if shared:
            if grid is None:
                pitch, rows = up(tail[1], 32), up(tail[2], 32)
                grid = bytearray(pitch * rows * bpb)
            for j in range(hb):
                d = ((by + j) * pitch + bx) * bpb
                if bx + wb > pitch or by + j >= rows or any(grid[d:d + wb * bpb]):
                    raise XtexError(f"level {lv} leaves its place in the shared surface")
                grid[d:d + wb * bpb] = data[j * wb * bpb:(j + 1) * wb * bpb]
        else:
            s = tile(data, wb, hb, bpb)
            out[at:at + len(s)] = s
    if grid is not None:
        s = tile(bytes(grid), pitch, rows, bpb)
        out[tail[0]:tail[0] + len(s)] = s
    return bytes(out)


def swap16(b: bytes) -> bytes:
    a = array.array("H", b)
    a.byteswap()
    return a.tobytes()


# -- block codecs (standard little-endian blocks, as in a DDS file) ---------------------------

def _c565(c: int) -> Tuple[int, int, int]:
    r, g, b = (c >> 11) & 31, (c >> 5) & 63, c & 31
    return (r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)


def _to565(r: float, g: float, b: float) -> int:
    def q(v, m):
        return max(0, min(m, int(v * m / 255 + 0.5)))
    return q(r, 31) << 11 | q(g, 63) << 5 | q(b, 31)


def _color_decode(b: bytes, o: int, dxt1: bool) -> List[Tuple[int, int, int, int]]:
    c0, c1, idx = struct.unpack_from("<HHI", b, o)
    p0, p1 = _c565(c0), _c565(c1)
    if c0 > c1 or not dxt1:
        pal = [p0 + (255,), p1 + (255,),
               tuple((2 * a + c) // 3 for a, c in zip(p0, p1)) + (255,),
               tuple((a + 2 * c) // 3 for a, c in zip(p0, p1)) + (255,)]
    else:
        pal = [p0 + (255,), p1 + (255,), tuple((a + c) // 2 for a, c in zip(p0, p1)) + (255,), (0, 0, 0, 0)]
    return [pal[(idx >> (2 * i)) & 3] for i in range(16)]


def _alpha5_decode(b: bytes, o: int) -> List[int]:
    a0, a1 = b[o], b[o + 1]
    bits = int.from_bytes(b[o + 2:o + 8], "little")
    if a0 > a1:
        pal = [a0, a1] + [((7 - k) * a0 + k * a1) // 7 for k in range(1, 7)]
    else:
        pal = [a0, a1] + [((5 - k) * a0 + k * a1) // 5 for k in range(1, 5)] + [0, 255]
    return [pal[(bits >> (3 * i)) & 7] for i in range(16)]


def decode(fmt: int, w: int, h: int, data: bytes) -> bytes:
    """blocks as stored (8-in-16 swapped) -> RGBA texels, row by row"""
    out = bytearray(w * h * 4)
    if fmt == ARGB8:
        for i in range(w * h):
            a, r, g, b = data[i * 4:i * 4 + 4]
            out[i * 4:i * 4 + 4] = bytes((r, g, b, a))
        return bytes(out)
    if fmt not in (DXT1, DXT3, DXT5):
        raise XtexError(f"no decoder for {FORMATS.get(fmt, (0, 0, 0, fmt))[3]}")
    std = swap16(data)
    bpb = FORMATS[fmt][2]
    wb, hb = blocks(w, h, fmt)
    for by in range(hb):
        for bx in range(wb):
            o = (by * wb + bx) * bpb
            if fmt == DXT1:
                px = _color_decode(std, o, True)
            else:
                px = _color_decode(std, o + 8, False)
                if fmt == DXT3:
                    bits = int.from_bytes(std[o:o + 8], "little")
                    al = [((bits >> (4 * i)) & 15) * 17 for i in range(16)]
                else:
                    al = _alpha5_decode(std, o)
                px = [p[:3] + (a,) for p, a in zip(px, al)]
            for i, p in enumerate(px):
                x, y = bx * 4 + (i & 3), by * 4 + (i >> 2)
                if x < w and y < h:
                    out[(y * w + x) * 4:(y * w + x) * 4 + 4] = bytes(p)
    return bytes(out)


def _endpoints(px: Sequence[Tuple[int, int, int]]):
    """The two colours at the ends of the texels' main axis (a range fit)."""
    n = len(px)
    m = [sum(p[k] for p in px) / n for k in range(3)]
    d = [[p[k] - m[k] for k in range(3)] for p in px]
    cov = [[sum(v[i] * v[j] for v in d) for j in range(3)] for i in range(3)]
    v = [1.0, 1.0, 1.0]
    for _ in range(6):
        nv = [sum(cov[i][j] * v[j] for j in range(3)) for i in range(3)]
        s = max(abs(x) for x in nv)
        if s == 0:
            break
        v = [x / s for x in nv]
    proj = [sum(q[k] * v[k] for k in range(3)) for q in d]
    return px[max(range(n), key=proj.__getitem__)], px[min(range(n), key=proj.__getitem__)]


def _near(p, pal):
    return min(range(len(pal)), key=lambda j: (p[0] - pal[j][0]) ** 2 + (p[1] - pal[j][1]) ** 2
               + (p[2] - pal[j][2]) ** 2)


def _color_encode(px: Sequence[Tuple[int, int, int, int]], dxt1: bool) -> bytes:
    holes = dxt1 and any(p[3] < 128 for p in px)
    solid = [p[:3] for p in px if not holes or p[3] >= 128]
    if not solid:
        return struct.pack("<HHI", 0, 0, 0xFFFFFFFF)
    hi, lo = _endpoints(solid)
    c0, c1 = _to565(*hi), _to565(*lo)
    if holes:
        c0, c1 = min(c0, c1), max(c0, c1)
        p0, p1 = _c565(c0), _c565(c1)
        pal = [p0, p1, tuple((a + c) // 2 for a, c in zip(p0, p1))]
        idx = 0
        for i, p in enumerate(px):
            idx |= (3 if p[3] < 128 else _near(p, pal)) << (2 * i)
        return struct.pack("<HHI", c0, c1, idx)
    if c0 < c1:
        c0, c1 = c1, c0
    if c0 == c1:
        return struct.pack("<HHI", c0, c1, 0)
    p0, p1 = _c565(c0), _c565(c1)
    pal = [p0, p1, tuple((2 * a + c) // 3 for a, c in zip(p0, p1)), tuple((a + 2 * c) // 3 for a, c in zip(p0, p1))]
    idx = 0
    for i, p in enumerate(px):
        idx |= _near(p, pal) << (2 * i)
    return struct.pack("<HHI", c0, c1, idx)


def _alpha5_encode(al: Sequence[int]) -> bytes:
    a0, a1 = max(al), min(al)
    if a0 == a1:
        return bytes((a0, a1)) + bytes(6)
    pal = [a0, a1] + [((7 - k) * a0 + k * a1) // 7 for k in range(1, 7)]
    bits = 0
    for i, a in enumerate(al):
        bits |= min(range(8), key=lambda j: abs(a - pal[j])) << (3 * i)
    return bytes((a0, a1)) + bits.to_bytes(6, "little")


def encode(fmt: int, w: int, h: int, rgba: bytes) -> bytes:
    """RGBA texels, row by row -> blocks as stored (8-in-16 swapped)"""
    if fmt not in WRITABLE:
        raise XtexError(f"no encoder for {FORMATS.get(fmt, (0, 0, 0, fmt))[3]}")
    wb, hb = blocks(w, h, fmt)
    out = bytearray()
    for by in range(hb):
        for bx in range(wb):
            px = []
            for i in range(16):
                x, y = min(w - 1, bx * 4 + (i & 3)), min(h - 1, by * 4 + (i >> 2))
                o = (y * w + x) * 4
                px.append(tuple(rgba[o:o + 4]))
            if fmt == DXT1:
                out += _color_encode(px, True)
                continue
            if fmt == DXT3:
                bits = 0
                for i, p in enumerate(px):
                    bits |= ((p[3] * 15 + 127) // 255) << (4 * i)
                out += bits.to_bytes(8, "little")
            else:
                out += _alpha5_encode([p[3] for p in px])
            out += _color_encode(px, False)
    return swap16(bytes(out))


def shrink(w: int, h: int, rgba: bytes) -> Tuple[int, int, bytes]:
    """The next mip level: 2 x 2 box filter (1 x 2 or 2 x 1 at a side of 1)."""
    nw, nh = max(1, w // 2), max(1, h // 2)
    sx, sy = (2 if w > 1 else 1), (2 if h > 1 else 1)
    out = bytearray(nw * nh * 4)
    k = sx * sy
    for y in range(nh):
        for x in range(nw):
            for c in range(4):
                s = 0
                for j in range(sy):
                    for i in range(sx):
                        s += rgba[((y * sy + j) * w + x * sx + i) * 4 + c]
                out[(y * nw + x) * 4 + c] = (s + k // 2) // k
    return nw, nh, bytes(out)


# -- PNG ------------------------------------------------------------------------------------

def png_write(path: str, w: int, h: int, rgba: bytes) -> None:
    raw = b"".join(b"\x00" + rgba[y * w * 4:(y + 1) * w * 4] for y in range(h))

    def chunk(t: bytes, d: bytes) -> bytes:
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    with open(path, "wb") as fh:
        fh.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">2I5B", w, h, 8, 6, 0, 0, 0))
                 + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def png_read(path: str) -> Tuple[int, int, bytes]:
    """-> (width, height, RGBA texels row by row)"""
    with open(path, "rb") as fh:
        b = fh.read()
    if b[:8] != b"\x89PNG\r\n\x1a\n":
        raise XtexError(f"{path}: not a PNG file")
    o, idat, plte, trns, hdr = 8, [], b"", b"", None
    while o + 8 <= len(b):
        n, t = struct.unpack_from(">I4s", b, o)
        d = b[o + 8:o + 8 + n]
        if t == b"IHDR":
            hdr = struct.unpack(">2I5B", d)
        elif t == b"PLTE":
            plte = d
        elif t == b"tRNS":
            trns = d
        elif t == b"IDAT":
            idat.append(d)
        elif t == b"IEND":
            break
        o += 12 + n
    if hdr is None:
        raise XtexError(f"{path}: no IHDR")
    w, h, depth, ctype, _, _, inter = hdr
    chans = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}.get(ctype)
    if chans is None or depth not in (8, 16) or inter or (ctype == 3 and depth != 8):
        raise XtexError(f"{path}: only 8/16-bit grey, grey+alpha, RGB, RGBA or 8-bit palette, not interlaced")
    bpp = chans * depth // 8
    raw = zlib.decompress(b"".join(idat))
    stride = w * bpp
    prev = bytearray(stride)
    pix = bytearray()
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        if f == 1:
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 255
        elif f == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 255
        elif f == 3:
            for i in range(stride):
                left = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((left + prev[i]) >> 1)) & 255
        elif f == 4:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                c = prev[i - bpp] if i >= bpp else 0
                bb = prev[i]
                p = a + bb - c
                pa, pb, pc = abs(p - a), abs(p - bb), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else bb if pb <= pc else c)) & 255
        elif f != 0:
            raise XtexError(f"{path}: bad filter {f}")
        pix += line
        prev = line
    if depth == 16:
        pix = pix[0::2]
    out = bytearray(w * h * 4)
    for i in range(w * h):
        if ctype == 0:
            v = pix[i]
            px = (v, v, v, 255)
        elif ctype == 4:
            v = pix[i * 2]
            px = (v, v, v, pix[i * 2 + 1])
        elif ctype == 2:
            px = (pix[i * 3], pix[i * 3 + 1], pix[i * 3 + 2], 255)
        elif ctype == 6:
            px = tuple(pix[i * 4:i * 4 + 4])
        else:
            k = pix[i]
            if k * 3 + 3 > len(plte):
                raise XtexError(f"{path}: palette index {k} past the palette")
            px = tuple(plte[k * 3:k * 3 + 3]) + (trns[k] if k < len(trns) else 255,)
        out[i * 4:i * 4 + 4] = bytes(px)
    return w, h, bytes(out)


# -- the archive ----------------------------------------------------------------------------

def textures(arc: kapf.Archive):
    """(section, chunk index, chunk, parsed APKF, type record index, item index, item, name): the name
    is the item's directory entry's (NAME.xetex), else its name in the chunk, else ''."""
    for sec in arc.sections:
        if sec.name:
            continue
        names = {(e.b, e.c): e.name for e in sec.entries or []}
        for bi, ch in enumerate(sec.chunks):
            if ch.data[:4] != b"APKF":
                continue
            a = apkf.parse(ch.data)
            for k, t in enumerate(a.types):
                if t.type != TEX:
                    continue
                for i, it in enumerate(t.items):
                    if it.gpu is not None:
                        yield sec, bi, ch, a, k, i, it, names.get((bi, k << 16 | i)) or a.name(it)


def _bare(name: str) -> str:
    return (name[:-6] if name.lower().endswith(".xetex") else name).lower()


def find(rows, name: str, chunk: Optional[int]):
    """by name (with or without .xetex), or by place: CHUNK:RECORD:ITEM as `list` prints it.
    rows: list(textures(arc)), so that the items of one chunk share one parsed chunk."""
    parts = name.split(":")
    if len(parts) == 3 and all(p.isdigit() for p in parts):
        b, k, i = map(int, parts)
        hits = [r for r in rows if (r[1], r[4], r[5]) == (b, k, i)]
    else:
        hits = [r for r in rows if _bare(r[7]) == _bare(name) and (chunk is None or r[1] == chunk)]
    if not hits:
        raise XtexError(f"no texture named {name!r}" + ("" if chunk is None else f" in chunk {chunk}"))
    if len(hits) > 1:
        raise XtexError(f"{len(hits)} textures named {name!r} (chunks {sorted({r[1] for r in hits})}): give --chunk")
    return hits[0]


def cmd_list(path: str) -> int:
    arc = kapf.read(path)
    n = 0
    for sec, bi, ch, a, k, i, it, name in textures(arc):
        fl = Fields(it.obj) if len(it.obj) == 0x60 else None
        if fl is None:
            print(f"{bi}:{k}:{i:<10s} object of {len(it.obj):#x} bytes  {name or '-'}")
        else:
            why = fl.plain()
            if not why and fl.gpu_size() != len(it.gpu):
                why = f"GPU part {len(it.gpu):#x}, the layout's {fl.gpu_size():#x}"
            place = f"{bi}:{k}:{i}"
            print(f"{place:10s} {fl.fw:4d}x{fl.fh:<4d} {fl.fmt_name:6s} levels 0-{fl.mip_hi:<2d} "
                  f"gpu {len(it.gpu):#9x}  {name or '-'}{'  (' + why + ')' if why else ''}")
        n += 1
    print(f"xtex: {n} textures")
    return 0


def cmd_export(path: str, name: str, png: str, level: int, chunk: Optional[int]) -> int:
    arc = kapf.read(path)
    sec, bi, ch, a, k, i, it, nm = find(list(textures(arc)), name, chunk)
    fl = Fields(it.obj)
    if fl.fmt not in READABLE:
        raise XtexError(f"{fl.fmt_name}: no decoder")
    w, h, data = read_level_blocks(fl, it.gpu, level)
    png_write(png, w, h, decode(fl.fmt, w, h, data))
    print(f"xtex: {nm} level {level}: {w}x{h} {fl.fmt_name} -> {png}")
    return 0


def dds_bytes(fl: Fields, gpu: bytes) -> bytes:
    """A DDS file of the texture: every level untiled, the blocks byte-swapped back to little-endian."""
    if fl.fmt not in (DXT1, DXT3, DXT5):
        raise XtexError(f"{fl.fmt_name}: DDS only for DXT1, DXT3, DXT5")
    data = b"".join(swap16(read_level_blocks(fl, gpu, lv)[2]) for lv in range(fl.mip_hi + 1))
    bpb = FORMATS[fl.fmt][2]
    wb, hb = blocks(fl.fw, fl.fh, fl.fmt)
    caps, flags = 0x1000, 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000          # texture; caps height width pixfmt linearsize
    if fl.mip_hi:
        caps |= 0x8 | 0x400000                                          # complex, mipmap
        flags |= 0x20000                                                # mipmapcount
    fourcc = {DXT1: b"DXT1", DXT3: b"DXT3", DXT5: b"DXT5"}[fl.fmt]
    head = struct.pack("<4s7I44x", b"DDS ", 124, flags, fl.fh, fl.fw, wb * hb * bpb, 0, fl.mip_hi + 1)
    head += struct.pack("<2I4s5I", 32, 0x4, fourcc, 0, 0, 0, 0, 0)   # pixel format: fourcc
    head += struct.pack("<4I4x", caps, 0, 0, 0)
    return head + data


def cmd_dds(path: str, outdir: str, names: Sequence[str], chunk: Optional[int]) -> int:
    arc = kapf.read(path)
    rows = list(textures(arc))
    pick = [find(rows, nm, chunk) for nm in names] if names else rows
    os.makedirs(outdir, exist_ok=True)
    wrote, skipped, seen = 0, {}, set()
    for sec, bi, ch, a, k, i, it, nm in pick:
        fl = Fields(it.obj) if len(it.obj) == 0x60 else None
        why = "object not 0x60" if fl is None else fl.plain()
        if not why and fl.fmt not in (DXT1, DXT3, DXT5):
            why = fl.fmt_name
        if not why and fl.gpu_size() != len(it.gpu):
            why = "GPU part not the layout's size"
        if why:
            skipped[why] = skipped.get(why, 0) + 1
            continue
        base = os.path.basename(_bare(nm) if nm else f"{bi}_{k}_{i}").replace("$", "_")
        if base in seen:
            base = f"{base}.{bi}_{k}_{i}"
        seen.add(base)
        with open(os.path.join(outdir, base + ".dds"), "wb") as fh:
            fh.write(dds_bytes(fl, it.gpu))
        wrote += 1
    for why in sorted(skipped):
        print(f"xtex: skipped ({why}): {skipped[why]}")
    print(f"xtex: wrote {wrote} DDS files to {outdir}")
    return 0


def is_cutout(rgba: bytes) -> bool:
    """An alpha mask: at least 1 texel in 64 under half alpha, and 70 % of the alpha near 0 or 255."""
    a = rgba[3::4]
    under = sum(1 for v in a if v < 128)
    extreme = sum(1 for v in a if v < 16 or v > 239)
    return under * 64 >= len(a) and extreme * 10 >= len(a) * 7


def coverage(rgba: bytes) -> float:
    a = rgba[3::4]
    return sum(1 for v in a if v >= 128) / max(1, len(a))


def keep_coverage(rgba: bytes, target: float) -> bytes:
    """rgba with its alpha scaled so that the share of texels at or over 128 comes as near target as
    the texels' values allow (texels of one value stay together: the value that moves to 128 is chosen
    among the alpha values present, or none at all)."""
    a = rgba[3::4]
    n = len(a)
    counts = [0] * 256
    for v in a:
        counts[v] += 1
    best_v, best_d, above = None, target, 0      # None: no texel reaches 128
    for v in range(255, 0, -1):
        if not counts[v]:
            continue
        above += counts[v]
        d = abs(above / n - target)
        if d < best_d:
            best_v, best_d = v, d
    out = bytearray(rgba)
    if best_v is None:
        for i in range(3, len(out), 4):
            out[i] = min(out[i], 127)
        return bytes(out)
    scale = 128 / best_v
    for i in range(3, len(out), 4):
        v = out[i]
        out[i] = 255 if v * scale >= 255 else (128 if v >= best_v else min(127, int(v * scale)))
    return bytes(out)


def make_levels(fmt: int, w: int, h: int, rgba: bytes, mips: bool,
                cutout: bool = False) -> List[Tuple[int, int, bytes]]:
    """The levels' blocks; with cutout each level from 1 on keeps the top level's coverage (the box
    filter runs on the unscaled levels, the scaling is applied to each level's own copy)."""
    levels = [(w, h, encode(fmt, w, h, rgba))]
    target = coverage(rgba) if cutout else None
    while mips and (w > 1 or h > 1):
        w, h, rgba = shrink(w, h, rgba)
        pic = keep_coverage(rgba, target) if cutout else rgba
        levels.append((w, h, encode(fmt, w, h, pic)))
    return levels


def cmd_import(path: str, out: str, pairs: Sequence[Tuple[str, str]], mips: bool,
               chunk: Optional[int], alpha: str = "auto") -> int:
    if alpha not in ("auto", "cutout", "plain"):
        raise XtexError("--alpha takes auto, cutout or plain")
    if os.path.realpath(path) == os.path.realpath(out):
        raise XtexError("OUT is IN")
    arc = kapf.read(path)
    rows = list(textures(arc))
    pictures: Dict[str, Tuple[int, int, bytes]] = {}
    made: Dict[Tuple[str, int], Tuple[int, int, int, bytes]] = {}
    chunks, done = {}, []
    for name, png in pairs:
        sec, bi, ch, a, k, i, it, nm = find(rows, name, chunk)
        fl = Fields(it.obj)
        why = fl.plain()
        if why:
            raise XtexError(f"{nm}: {why}")
        if fl.fmt not in WRITABLE:
            raise XtexError(f"{nm}: {fl.fmt_name}; this writes DXT1, DXT3 and DXT5 only")
        if png not in pictures:
            w, h, rgba = png_read(png)
            if w & (w - 1) or h & (h - 1) or not (4 <= w <= 4096 and 4 <= h <= 4096):
                raise XtexError(f"{png}: {w}x{h}; each side a power of two from 4 to 4096")
            pictures[png] = (w, h, rgba)
        if (png, fl.fmt) not in made:
            w, h, rgba = pictures[png]
            cut = alpha == "cutout" or alpha == "auto" and is_cutout(rgba)
            if cut and mips:
                print(f"xtex: {os.path.basename(png)}: a cut-out, every level keeps {100 * coverage(rgba):.1f} % coverage")
            levels = make_levels(fl.fmt, w, h, rgba, mips, cut)
            made[(png, fl.fmt)] = (w, h, len(levels) - 1, build_gpu(fl.fmt, levels, len(levels) > 1))
        w, h, top, gpu = made[(png, fl.fmt)]
        done.append((sec, bi, k, i, it, len(it.gpu) + 0x10010, nm, fl))
        a.replace(it, obj=fl.with_size(w, h, top), gpu=gpu)
        chunks[(id(sec), bi)] = (bi, ch, a)
    for bi, ch, a in chunks.values():
        data = apkf.build(a)
        if len(data) > ch.slot:
            raise XtexError(f"chunk {bi} would be {len(data):#x} bytes, over its slot of {ch.slot:#x}; nothing written")
        apkf.parse(data)
        ch.data = data
    fixed = kept = 0
    for sec, bi, k, i, it, old_a, nm, fl in done:
        for e in sec.entries or []:
            if e.b == bi and e.c == (k << 16 | i):
                if e.a == old_a:
                    e.a = apkf.directory_a("xetex", it)
                    fixed += 1
                else:
                    kept += 1
    kapf.write(arc, out)
    back = list(textures(kapf.read(out)))
    for sec, bi, k, i, it, old_a, nm, fl in done:
        fl2 = Fields(find(back, f"{bi}:{k}:{i}", None)[6].obj)
        print(f"xtex: {nm}: {fl.fw}x{fl.fh} levels 0-{fl.mip_hi} -> {fl2.fw}x{fl2.fh} levels 0-{fl2.mip_hi} {fl2.fmt_name}")
    print(f"xtex: {len(done)} textures in {len(chunks)} chunks; directory A updated {fixed}, kept {kept}; wrote {out}")
    return 0


def main(argv: Sequence[str]) -> int:
    args = list(argv)
    opts: Dict[str, Optional[str]] = {}
    for flag, has_value in (("--level", True), ("--chunk", True), ("--no-mips", False), ("--alpha", True)):
        if flag in args:
            j = args.index(flag)
            opts[flag] = args[j + 1] if has_value else ""
            del args[j:j + (2 if has_value else 1)]
    chunk = int(opts["--chunk"]) if opts.get("--chunk") is not None else None
    try:
        if len(args) == 2 and args[0] == "list":
            return cmd_list(args[1])
        if len(args) == 4 and args[0] == "export":
            return cmd_export(args[1], args[2], args[3], int(opts.get("--level") or 0), chunk)
        if len(args) >= 3 and args[0] == "dds":
            return cmd_dds(args[1], args[2], args[3:], chunk)
        if len(args) >= 5 and len(args) % 2 == 1 and args[0] == "import":
            pairs = list(zip(args[3::2], args[4::2]))
            return cmd_import(args[1], args[2], pairs, "--no-mips" not in opts, chunk, opts.get("--alpha") or "auto")
    except (XtexError, apkf.ApkfError, kapf.KapfError) as e:
        print(f"xtex: {e}")
        return 1
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
