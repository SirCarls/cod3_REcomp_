#!/usr/bin/env python3
"""bro_glyphs.py - find Big Red One's fonts in a .cod archive: every text font
file (.fdf, "; font file") and every DDS texture, written out so the button
font can be matched with its picture.

    python3 -I bro_glyphs.py <archive.cod> <out_dir> [--png-size WxH ...]

Writes into <out_dir>/bro_fonts/:
  fonts.tsv          one row per font file found: offset, cellheight, ascent,
                     firstglyph, numglyphs, bitmap extent (max x+w, max y+h)
  font_<offset>.fdf  each font file's text as stored
  textures.tsv       one row per DDS: offset, width, height, format, mips
  tex_<offset>.png   each DDS whose size matches a font's extent (or --png-size)
Prints a short summary only. Reads the archive as data; nothing is run.
"""
import argparse
import os
import re
import struct
import sys
import zlib

FDF_START = b"; font file"


def parse_fdf(text):
    head = {}
    glyphs = []
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith(";"):
            continue
        toks = line.split()
        if toks[0].isdigit() and len(toks) >= 3:
            g = {"char": int(toks[0])}
            for k, v in zip(toks[1::2], toks[2::2]):
                try:
                    g[k] = int(v, 0)
                except ValueError:
                    pass
            glyphs.append(g)
        else:
            for k, v in zip(toks[0::2], toks[1::2]):
                try:
                    head[k] = int(v, 0)
                except ValueError:
                    head[k] = v
    return head, glyphs


def find_fdfs(d):
    out = []
    pos = 0
    while True:
        i = d.find(FDF_START, pos)
        if i < 0:
            return out
        j = i
        while j < len(d) and (32 <= d[j] < 127 or d[j] in (9, 10, 13)):
            j += 1
        out.append((i, d[i:j].decode("latin-1")))
        pos = j


# --- DDS ------------------------------------------------------------------
def c565(v):
    r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
    return ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))


def bc1(blk, opaque):
    c0, c1 = struct.unpack_from("<HH", blk, 0)
    a, b = c565(c0), c565(c1)
    if c0 > c1 or opaque:
        cols = [a + (255,), b + (255,), tuple((2 * a[i] + b[i]) // 3 for i in range(3)) + (255,),
                tuple((a[i] + 2 * b[i]) // 3 for i in range(3)) + (255,)]
    else:
        cols = [a + (255,), b + (255,), tuple((a[i] + b[i]) // 2 for i in range(3)) + (255,), (0, 0, 0, 0)]
    idx = struct.unpack_from("<I", blk, 4)[0]
    return [cols[(idx >> (2 * i)) & 3] for i in range(16)]


def bc3a(blk):
    a0, a1 = blk[0], blk[1]
    bits = int.from_bytes(blk[2:8], "little")
    tab = [a0, a1] + ([((6 - i) * a0 + (1 + i) * a1) // 7 for i in range(6)] if a0 > a1 else
                      [((4 - i) * a0 + (1 + i) * a1) // 5 for i in range(4)] + [0, 255])
    return [tab[(bits >> (3 * i)) & 7] for i in range(16)]


def dds_info(d, o):
    if d[o:o + 4] != b"DDS " or struct.unpack_from("<I", d, o + 4)[0] != 124:
        return None
    h, w = struct.unpack_from("<II", d, o + 12)
    mips = struct.unpack_from("<I", d, o + 28)[0]
    pf_flags, fourcc, bits = struct.unpack_from("<I4sI", d, o + 80)
    masks = struct.unpack_from("<IIII", d, o + 92)
    fmt = fourcc.decode("latin-1") if pf_flags & 4 else f"rgb{bits}"
    return {"w": w, "h": h, "mips": mips, "fmt": fmt, "masks": masks, "bits": bits}


def dds_rgba(d, o, info):
    w, h, fmt = info["w"], info["h"], info["fmt"]
    p = o + 128
    out = bytearray(w * h * 4)
    if fmt in ("DXT1", "DXT2", "DXT3", "DXT4", "DXT5"):
        bpb = 8 if fmt == "DXT1" else 16
        wb, hb = (w + 3) // 4, (h + 3) // 4
        for by in range(hb):
            for bx in range(wb):
                blk = d[p:p + bpb]
                p += bpb
                if fmt == "DXT1":
                    px = bc1(blk, False)
                elif fmt in ("DXT2", "DXT3"):
                    cols = bc1(blk[8:], True)
                    px = [c[:3] + (((blk[i // 2] >> (4 * (i & 1))) & 15) * 17,) for i, c in enumerate(cols)]
                else:
                    cols, al = bc1(blk[8:], True), bc3a(blk[:8])
                    px = [c[:3] + (al[i],) for i, c in enumerate(cols)]
                for i in range(16):
                    x, y = bx * 4 + (i & 3), by * 4 + (i >> 2)
                    if x < w and y < h:
                        out[(y * w + x) * 4:(y * w + x) * 4 + 4] = bytes(px[i])
        return bytes(out)
    if fmt == "rgb32":
        rm, gm, bm, am = info["masks"]

        def ch(v, m):
            if not m:
                return 255
            s = (m & -m).bit_length() - 1
            return ((v & m) >> s) * 255 // (m >> s)
        for i in range(w * h):
            v = struct.unpack_from("<I", d, p + 4 * i)[0]
            out[i * 4:i * 4 + 4] = bytes((ch(v, rm), ch(v, gm), ch(v, bm), ch(v, am)))
        return bytes(out)
    raise ValueError(f"DDS format {fmt} not decoded")


def write_png(path, w, h, rgba):
    raw = b"".join(b"\0" + rgba[y * w * 4:(y + 1) * w * 4] for y in range(h))

    def chunk(t, x):
        return struct.pack(">I", len(x)) + t + x + struct.pack(">I", zlib.crc32(t + x) & 0xFFFFFFFF)
    with open(path, "wb") as fh:
        fh.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
                 + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("archive")
    ap.add_argument("out_dir")
    ap.add_argument("--png-size", action="append", default=[], help="also write DDS of this size, e.g. 256x128")
    a = ap.parse_args(argv)
    d = open(a.archive, "rb").read()
    out = os.path.join(a.out_dir, "bro_fonts")
    os.makedirs(out, exist_ok=True)
    fonts = []
    with open(os.path.join(out, "fonts.tsv"), "w", encoding="utf-8") as fh:
        fh.write("offset\tcellheight\tascent\tfirstglyph\tnumglyphs\tglyph_rows\textent_w\textent_h\n")
        for off, text in find_fdfs(d):
            head, gl = parse_fdf(text)
            ew = max((g.get("x", 0) + g.get("w", 0) for g in gl), default=0)
            eh = max((g.get("y", 0) + g.get("gh", 0) for g in gl), default=0)
            fonts.append((off, head, gl, ew, eh))
            fh.write(f"{off:X}\t{head.get('cellheight')}\t{head.get('ascent')}\t{head.get('firstglyph')}\t"
                     f"{head.get('numglyphs')}\t{len(gl)}\t{ew}\t{eh}\n")
            with open(os.path.join(out, f"font_{off:X}.fdf"), "w", encoding="latin-1") as f2:
                f2.write(text)
    want = {tuple(int(v) for v in s.lower().split("x")) for s in a.png_size}
    texs = 0
    pngs = 0
    with open(os.path.join(out, "textures.tsv"), "w", encoding="utf-8") as fh:
        fh.write("offset\twidth\theight\tformat\tmips\tpng\n")
        for m in re.finditer(rb"DDS \x7c\x00\x00\x00", d):
            info = dds_info(d, m.start())
            if not info:
                continue
            texs += 1
            fits = any(info["w"] >= ew and info["h"] >= eh and info["w"] <= 2 * max(ew, 1) and info["h"] <= 2 * max(eh, 1)
                       for _, _, _, ew, eh in fonts) or (info["w"], info["h"]) in want
            png = ""
            if fits and info["w"] * info["h"] <= 1024 * 1024:
                try:
                    png = f"tex_{m.start():X}.png"
                    write_png(os.path.join(out, png), info["w"], info["h"], dds_rgba(d, m.start(), info))
                    pngs += 1
                except ValueError as e:
                    png = f"({e})"
            fh.write(f"{m.start():X}\t{info['w']}\t{info['h']}\t{info['fmt']}\t{info['mips']}\t{png}\n")
    print(f"bro_glyphs: {len(fonts)} font files, {texs} DDS textures, {pngs} written as PNG -> {out}")
    for off, head, gl, ew, eh in fonts:
        print(f"  font at {off:X}: cellheight {head.get('cellheight')}, glyphs {head.get('firstglyph')}+"
              f"{head.get('numglyphs')} ({len(gl)} rows), bitmap {ew}x{eh}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
