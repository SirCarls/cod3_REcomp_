"""Upscaling a picture atlas cell by cell: the Xbox 360 button font.

[claude-9b-think] 2026-10-27 UNTESTED (Part D, the Xbox 360 button upscale).

The game's controller buttons in its prompts are characters of a font whose
picture is an atlas, i_button_icons_xe_<language> (global.cod): 256 x 128
(recomp/src/game/addresses.h, namespace prompts). waifu2x run on the whole
atlas would let each button bleed into its neighbours (the network sees
across them), and its colour under the transparent corners is whatever the
artist left there. So each button is upscaled on its own.

[claude-9b-think] 2026-10-27 UNTESTED v3: the buttons are cut on their own
rectangles, not on 32 x 32 cells. claude-9b-test found on the SP atlas that
the 21 buttons sit on a ~33 px pitch (columns x 10-42, 45-73, 78-106, 111-139,
144-172, 175-207, 208-240; rows y 6-34, 39-67, 69-103), so a fixed 32 x 32 cut
crossed every button and upscaled it in 2-4 pieces with seams. The font's
own character rectangles aren't available to the setup tool (the ripper
doesn't read fonts), so the rectangles come from the picture
(find_glyph_boxes):

- Rows, then columns within each row, of pixels with opacity >= TRANSPARENT.
- A run much wider or taller than the typical one (two buttons touching, as
  the SP atlas's x 207 / 208) is split at its emptiest line near the even
  division.
- Each button's tight box is then widened into the gap around it, by at
  most GAP_PAD pixels and never past halfway to a neighbour (pad_boxes), so
  its faint edge comes along and no two boxes overlap.

If that finds nothing sensible (no box, a box over MAX_GLYPH, more than
MAX_GLYPHS), the atlas falls back to the old 32 x 32 cells. The boxes are
written next to the staged pictures (<atlas>.boxes.txt) for checking.

1. Each button (its widened box) is cut out with a border of its own edge
   pixels repeated (PAD on every side), so the network sees no neighbour.
2. Transparent pixels get the colour of the nearest opaque ones (a few
   rounds of averaging opaque neighbours), so the colour edge doesn't pull in
   the colour hidden under the transparency.
3. The colour (RGB) and the opacity (as grey) are upscaled as two pictures,
   because waifu2x works on colour pictures.
4. Each button goes back at exactly `scale` times its place, without the
   border, colour and opacity together, into an otherwise transparent atlas:
   the same layout, so the font's character coordinates (fractions of the
   atlas) still find each button.

The result is an ordinary upscaled copy in textures_upscaled/ under the
atlas's own name; the HD pack (`rip cod3-hd-pack`) compresses it in the
original's format with a full mip chain like any other.

Both programs: the campaign's atlas comes with the rip of sp/ (global.cod:
i_button_icons_xe_en, DXT5 256x128); the multiplayer program's own copy
(mp/global.cod, a different picture under the same name) is added by
add_mp_atlases after that rip. Every language's atlas the archives hold
(i_button_icons_xe_<language>) matches CELL_ATLASES.

PNG reading and writing is plain Python (zlib): 8-bit greyscale, grey + alpha,
RGB, RGBA and palette pictures without interlacing, which is what the ripper
writes.
"""
from __future__ import annotations

import re
import shutil
import struct
import zlib
from pathlib import Path
from typing import Callable, Dict, List, Optional, Tuple

# Atlases upscaled cell by cell, by their texture name (the ripper's `name`;
# the PNG's stem may carry "__<key>" after it), with their cell size.
CELL_ATLASES: List[Tuple[re.Pattern, int]] = [
    (re.compile(r"^i_button_icons_xe(?:_[a-z]+)?(?:__[0-9a-f]{16})?$", re.IGNORECASE), 32),
]
# Border of repeated edge pixels around each cell, in source pixels.
PAD = 4
# Rounds of colour spreading into transparent pixels.
BLEED_ROUNDS = 6
# Below this opacity (0-255) a pixel counts as transparent for the bleed and
# the button boxes.
TRANSPARENT = 8
# [claude-9b-think] v3: a button's box widens into the gap around it by at most
# this many pixels (never past halfway to a neighbour).
GAP_PAD = 5
# A box larger than this either way, or more boxes than MAX_GLYPHS, means the
# boxes weren't found: the atlas is cut in cells instead.
MAX_GLYPH = 64
MAX_GLYPHS = 128
# A run of rows or columns this many times the typical one holds touching
# buttons and is split.
SPLIT_RATIO = 1.5

Image = Tuple[int, int, bytearray]   # width, height, RGBA bytes
Box = Tuple[int, int, int, int]      # x0, y0, x1, y1 (x1 and y1 one past the last pixel)


def cell_size(stem: str) -> Optional[int]:
    """The cell size of the atlas named `stem`, or None if it isn't one."""
    for pattern, size in CELL_ATLASES:
        if pattern.match(stem):
            return size
    return None


# ---------------------------------------------------------------------------
# PNG
# ---------------------------------------------------------------------------
def _paeth(a: int, b: int, c: int) -> int:
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def read_png(path: Path) -> Image:
    """A PNG as RGBA. Raises ValueError for one it can't read."""
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path.name}: not a PNG")
    pos, idat, palette, trns = 8, bytearray(), b"", b""
    width = height = depth = ctype = interlace = 0
    while pos + 8 <= len(data):
        length, tag = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if tag == b"IHDR":
            width, height, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", body)
        elif tag == b"PLTE":
            palette = body
        elif tag == b"tRNS":
            trns = body
        elif tag == b"IDAT":
            idat += body
        elif tag == b"IEND":
            break
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}.get(ctype)
    if depth != 8 or channels is None or interlace:
        raise ValueError(f"{path.name}: {depth}-bit colour type {ctype} (interlace {interlace}) not supported")
    raw = zlib.decompress(bytes(idat))
    stride = width * channels
    out = bytearray(width * height * 4)
    prev = bytearray(stride)
    i = 0
    for y in range(height):
        kind = raw[i]
        line = bytearray(raw[i + 1:i + 1 + stride])
        i += 1 + stride
        for x in range(stride):
            a = line[x - channels] if x >= channels else 0
            b = prev[x]
            c = prev[x - channels] if x >= channels else 0
            if kind == 1:
                line[x] = (line[x] + a) & 0xFF
            elif kind == 2:
                line[x] = (line[x] + b) & 0xFF
            elif kind == 3:
                line[x] = (line[x] + ((a + b) >> 1)) & 0xFF
            elif kind == 4:
                line[x] = (line[x] + _paeth(a, b, c)) & 0xFF
        prev = line
        for x in range(width):
            o = (y * width + x) * 4
            if ctype == 6:
                out[o:o + 4] = line[x * 4:x * 4 + 4]
            elif ctype == 2:
                out[o:o + 3] = line[x * 3:x * 3 + 3]
                out[o + 3] = 255
            elif ctype == 4:
                g, alpha = line[x * 2], line[x * 2 + 1]
                out[o:o + 4] = bytes((g, g, g, alpha))
            elif ctype == 0:
                g = line[x]
                out[o:o + 4] = bytes((g, g, g, 255))
            else:  # palette
                k = line[x]
                out[o:o + 3] = palette[k * 3:k * 3 + 3]
                out[o + 3] = trns[k] if k < len(trns) else 255
    return width, height, out


def write_png(path: Path, image: Image, alpha: bool = True) -> None:
    """`image` as an 8-bit RGBA PNG (RGB with `alpha` False)."""
    width, height, rgba = image
    channels = 4 if alpha else 3
    raw = bytearray()
    for y in range(height):
        raw.append(0)
        row = rgba[y * width * 4:(y + 1) * width * 4]
        if alpha:
            raw += row
        else:
            for x in range(width):
                raw += row[x * 4:x * 4 + 3]

    def chunk(tag: bytes, body: bytes) -> bytes:
        return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF)

    header = struct.pack(">IIBBBBB", width, height, 8, 6 if alpha else 2, 0, 0, 0)
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
                     + chunk(b"IEND", b""))


# ---------------------------------------------------------------------------
# Cells
# ---------------------------------------------------------------------------
def _cut(image: Image, x0: int, y0: int, size: int) -> Image:
    """The cell at x0, y0 with PAD pixels of its own edge repeated around it."""
    width, height, rgba = image
    side = size + 2 * PAD
    out = bytearray(side * side * 4)
    for y in range(side):
        sy = y0 + min(max(y - PAD, 0), size - 1)
        sy = min(sy, height - 1)
        for x in range(side):
            sx = min(x0 + min(max(x - PAD, 0), size - 1), width - 1)
            s = (sy * width + sx) * 4
            o = (y * side + x) * 4
            out[o:o + 4] = rgba[s:s + 4]
    return side, side, out


def _bleed(image: Image) -> Image:
    """Transparent pixels take the average colour of their opaque neighbours,
    BLEED_ROUNDS times outward; their opacity is kept."""
    width, height, rgba = image
    rgba = bytearray(rgba)
    known = [rgba[i * 4 + 3] >= TRANSPARENT for i in range(width * height)]
    for _ in range(BLEED_ROUNDS):
        newly = []
        for y in range(height):
            for x in range(width):
                i = y * width + x
                if known[i]:
                    continue
                total, n = [0, 0, 0], 0
                for dy in (-1, 0, 1):
                    for dx in (-1, 0, 1):
                        nx, ny = x + dx, y + dy
                        if (dx or dy) and 0 <= nx < width and 0 <= ny < height and known[ny * width + nx]:
                            j = (ny * width + nx) * 4
                            total[0] += rgba[j]
                            total[1] += rgba[j + 1]
                            total[2] += rgba[j + 2]
                            n += 1
                if n:
                    newly.append((i, [t // n for t in total]))
        if not newly:
            break
        for i, colour in newly:
            rgba[i * 4:i * 4 + 3] = bytes(colour)
            known[i] = True
    return width, height, rgba


def _alpha_as_grey(image: Image) -> Image:
    width, height, rgba = image
    out = bytearray(len(rgba))
    for i in range(width * height):
        a = rgba[i * 4 + 3]
        out[i * 4:i * 4 + 4] = bytes((a, a, a, 255))
    return width, height, out


def _cut_box(image: Image, box: Box) -> Image:
    """The box with PAD pixels of its own edge repeated around it."""
    width, height, rgba = image
    x0, y0, x1, y1 = box
    w, h = x1 - x0, y1 - y0
    out_w, out_h = w + 2 * PAD, h + 2 * PAD
    out = bytearray(out_w * out_h * 4)
    for y in range(out_h):
        sy = min(y0 + min(max(y - PAD, 0), h - 1), height - 1)
        for x in range(out_w):
            sx = min(x0 + min(max(x - PAD, 0), w - 1), width - 1)
            s = (sy * width + sx) * 4
            o = (y * out_w + x) * 4
            out[o:o + 4] = rgba[s:s + 4]
    return out_w, out_h, out


def _runs(counts: List[int]) -> List[Tuple[int, int]]:
    """The runs of non-zero counts, as (start, end) with end one past the last."""
    runs, start = [], None
    for i, n in enumerate(counts):
        if n and start is None:
            start = i
        elif not n and start is not None:
            runs.append((start, i))
            start = None
    if start is not None:
        runs.append((start, len(counts)))
    return runs


def _median(values: List[int]) -> int:
    ordered = sorted(values)
    return ordered[len(ordered) // 2] if ordered else 0


def _split_wide(run: Tuple[int, int], counts: List[int], typical: int) -> List[Tuple[int, int]]:
    """A run SPLIT_RATIO or more times the typical length (buttons touching) cut
    into round(length / typical) pieces, each cut at the emptiest line within a
    quarter of the typical length of the even division (ties: the nearest)."""
    start, end = run
    length = end - start
    if typical <= 0 or length < SPLIT_RATIO * typical:
        return [run]
    pieces = max(2, round(length / typical))
    window = max(2, typical // 4)
    cuts = []
    for j in range(1, pieces):
        target = start + length * j // pieces
        lo, hi = max(start + 1, target - window), min(end - 1, target + window)
        if lo > hi:
            continue
        cut = min(range(lo, hi + 1), key=lambda c: (counts[c], abs(c - target)))
        if not cuts or cut > cuts[-1]:
            cuts.append(cut)
    bounds = [start] + cuts + [end]
    return [(a, b) for a, b in zip(bounds, bounds[1:]) if b > a]


def find_glyph_boxes(image: Image) -> List[Box]:
    """[claude-9b-think] v3: the buttons' tight boxes: rows of pixels with
    opacity >= TRANSPARENT, then columns within each row, touching ones split
    (_split_wide), each piece shrunk to the pixels it holds. Row by row, left
    to right."""
    width, height, rgba = image
    opaque = [rgba[i * 4 + 3] >= TRANSPARENT for i in range(width * height)]
    row_counts = [sum(opaque[y * width:(y + 1) * width]) for y in range(height)]
    bands = _runs(row_counts)
    typical_h = _median([b - a for a, b in bands])
    bands = [piece for band in bands for piece in _split_wide(band, row_counts, typical_h)]
    columns = []   # (band, column run, column counts)
    for y0, y1 in bands:
        counts = [sum(1 for y in range(y0, y1) if opaque[y * width + x]) for x in range(width)]
        columns.append(((y0, y1), _runs(counts), counts))
    typical_w = _median([b - a for _, runs, _ in columns for a, b in runs])
    boxes = []
    for (y0, y1), runs, counts in columns:
        for run in runs:
            for x0, x1 in _split_wide(run, counts, typical_w):
                ys = [y for y in range(y0, y1) if any(opaque[y * width + x] for x in range(x0, x1))]
                xs = [x for x in range(x0, x1) if any(opaque[y * width + x] for y in range(y0, y1))]
                if ys and xs:
                    boxes.append((xs[0], ys[0], xs[-1] + 1, ys[-1] + 1))
    return boxes


def pad_boxes(boxes: List[Box], width: int, height: int) -> List[Box]:
    """[claude-9b-think] v3: each box widened into the gap around it, by at
    most GAP_PAD on a side and at most half the gap to any box beside, above or
    below it (within reach on the other axis), so no two widened boxes
    overlap."""
    reach = 2 * GAP_PAD
    out = []
    for i, (x0, y0, x1, y1) in enumerate(boxes):
        left = right = up = down = GAP_PAD
        for j, (a0, b0, a1, b1) in enumerate(boxes):
            if i == j:
                continue
            rows_meet = b0 < y1 + reach and b1 > y0 - reach
            cols_meet = a0 < x1 + reach and a1 > x0 - reach
            if rows_meet:
                if a1 <= x0:
                    left = min(left, (x0 - a1) // 2)
                elif a0 >= x1:
                    right = min(right, (a0 - x1) // 2)
            if cols_meet:
                if b1 <= y0:
                    up = min(up, (y0 - b1) // 2)
                elif b0 >= y1:
                    down = min(down, (b0 - y1) // 2)
        out.append((max(0, x0 - left), max(0, y0 - up), min(width, x1 + right), min(height, y1 + down)))
    return out


def glyph_boxes(image: Image) -> Optional[List[Box]]:
    """The widened button boxes, or None when they don't look like buttons
    (the caller then cuts cells)."""
    width, height, _ = image
    boxes = find_glyph_boxes(image)
    if not boxes or len(boxes) > MAX_GLYPHS:
        return None
    if any(x1 - x0 > MAX_GLYPH or y1 - y0 > MAX_GLYPH for x0, y0, x1, y1 in boxes):
        return None
    return pad_boxes(boxes, width, height)


def cut_glyphs(atlas: Path, staging: Path) -> Optional[Dict[str, Box]]:
    """[claude-9b-think] v3: each button of `atlas` written twice into
    `staging`, as cut_cells does: <stem>@g<nn>.png (colour, transparency
    bled) and <stem>@g<nn>@a.png (opacity as grey), each its widened box plus
    PAD. Also <stem>.boxes.txt (tight and widened boxes) for checking.
    Returns {picture stem: widened box}, or None when no boxes were found."""
    image = read_png(atlas)
    width, height, _ = image
    tight = find_glyph_boxes(image)
    boxes = glyph_boxes(image)
    if boxes is None:
        return None
    staging.mkdir(parents=True, exist_ok=True)
    lines = [f"# {atlas.name}: {width}x{height}, {len(boxes)} buttons; x0 y0 x1 y1 (tight, then widened)"]
    glyphs = {}
    for n, (box, tight_box) in enumerate(zip(boxes, tight)):
        stem = f"{atlas.stem}@g{n:02d}"
        piece = _cut_box(image, box)
        write_png(staging / f"{stem}.png", _bleed(piece), alpha=False)
        write_png(staging / f"{stem}@a.png", _alpha_as_grey(piece), alpha=False)
        glyphs[stem] = box
        lines.append(f"{stem}\t{' '.join(map(str, tight_box))}\t{' '.join(map(str, box))}")
    (staging / f"{atlas.stem}.boxes.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
    return glyphs


def join_glyphs(atlas: Path, glyphs: Dict[str, Box], upscaled: Path, scale: int, out: Path) -> None:
    """[claude-9b-think] v3: the atlas at `scale`: transparent, with each
    upscaled button (colour and opacity) cropped of its border and put at
    exactly `scale` times its widened box."""
    src_w, src_h, _ = read_png(atlas)
    width, height = src_w * scale, src_h * scale
    rgba = bytearray(width * height * 4)
    pad = PAD * scale
    for stem, (x0, y0, x1, y1) in glyphs.items():
        cw, ch, colour = read_png(upscaled / f"{stem}.png")
        aw, ah, opacity = read_png(upscaled / f"{stem}@a.png")
        ew, eh = (x1 - x0 + 2 * PAD) * scale, (y1 - y0 + 2 * PAD) * scale
        if (cw, ch) != (ew, eh) or (aw, ah) != (ew, eh):
            raise ValueError(f"{stem}: upscaled to {cw}x{ch} / {aw}x{ah}, expected {ew}x{eh}")
        for y in range((y1 - y0) * scale):
            for x in range((x1 - x0) * scale):
                s = ((y + pad) * cw + x + pad) * 4
                o = ((y0 * scale + y) * width + x0 * scale + x) * 4
                rgba[o:o + 3] = colour[s:s + 3]
                rgba[o + 3] = opacity[s]   # the grey's red channel
    write_png(out, (width, height, rgba))


def cut_cells(atlas: Path, size: int, staging: Path) -> Dict[str, Tuple[int, int]]:
    """Writes each cell of `atlas` twice into `staging`: <stem>@<cx>_<cy>.png
    (colour, transparency bled) and <stem>@<cx>_<cy>@a.png (opacity as grey).
    Returns {cell picture stem: (x0, y0)} for the colour ones."""
    image = read_png(atlas)
    width, height, _ = image
    if width % size or height % size:
        raise ValueError(f"{atlas.name} is {width}x{height}, not whole {size}x{size} cells")
    staging.mkdir(parents=True, exist_ok=True)
    cells = {}
    for y0 in range(0, height, size):
        for x0 in range(0, width, size):
            cell = _cut(image, x0, y0, size)
            stem = f"{atlas.stem}@{x0 // size}_{y0 // size}"
            write_png(staging / f"{stem}.png", _bleed(cell), alpha=False)
            write_png(staging / f"{stem}@a.png", _alpha_as_grey(cell), alpha=False)
            cells[stem] = (x0, y0)
    return cells


def join_cells(atlas: Path, size: int, cells: Dict[str, Tuple[int, int]], upscaled: Path, scale: int,
               out: Path) -> None:
    """The atlas at `scale` from the upscaled cells in `upscaled` (colour and
    opacity), each cropped of its border, written to `out` as RGBA."""
    src_w, src_h, _ = read_png(atlas)
    width, height = src_w * scale, src_h * scale
    rgba = bytearray(width * height * 4)
    side = size * scale
    pad = PAD * scale
    for stem, (x0, y0) in cells.items():
        cw, ch, colour = read_png(upscaled / f"{stem}.png")
        aw, ah, opacity = read_png(upscaled / f"{stem}@a.png")
        expected = (size + 2 * PAD) * scale
        if (cw, ch) != (expected, expected) or (aw, ah) != (expected, expected):
            raise ValueError(f"{stem}: upscaled to {cw}x{ch} / {aw}x{ah}, expected {expected}x{expected}")
        for y in range(side):
            for x in range(side):
                s = ((y + pad) * cw + x + pad) * 4
                o = ((y0 * scale + y) * width + x0 * scale + x) * 4
                rgba[o:o + 3] = colour[s:s + 3]
                rgba[o + 3] = opacity[s]   # the grey's red channel
    write_png(out, (width, height, rgba))


KEYS_FILE = "hd_keys.tsv"   # the ripper's list (c3w/upscale.py, engine/tools/rip/src/hd.rs)
KEYS_HEADER = "# key\twidth\theight\txenos_format\tname\tfile\tkind"


def add_mp_atlases(rip: Path, mp_global: Path, textures: Path, scratch: Path, run: Callable[[List[str]], None],
                   remove_tree: Callable[[Path], None]) -> int:
    """[claude-9b-think] 2026-10-27 UNTESTED. The multiplayer program's own copies of the atlases: its mp/global.cod
    has i_button_icons_xe_<language> under the same name with another picture (another key: the campaign's
    d532a336349af630, cod3mp's 7059a5c4c5009136 for English), and the campaign rip (`rip cod3-all` on sp/) never
    sees it. `rip cod3-tex mp_global scratch` (run(cmd)), then each atlas whose key `textures`/hd_keys.tsv doesn't
    have yet is copied in as "<name>__<key>.png" (the ripper's own name for a second picture under one name) with
    its row, so the upscale and the HD pack treat it like the campaign's; the game replaces it by its key in
    whichever program loads it. `scratch` is emptied with remove_tree. Returns how many were added (0 without an
    mp/global.cod)."""
    if not mp_global.is_file():
        return 0
    keys_path = textures / KEYS_FILE
    text = keys_path.read_text(encoding="utf-8", errors="replace")
    rows = [line for line in text.splitlines() if line and not line.startswith("#")]
    have = {line.split("\t")[0] for line in rows}
    if scratch.exists():
        remove_tree(scratch)
    try:
        run([str(rip), "cod3-tex", str(mp_global), str(scratch)])
        ripped = scratch / "textures"
        added = []
        for line in (ripped / KEYS_FILE).read_text(encoding="utf-8", errors="replace").splitlines():
            cols = line.split("\t")
            if not line or line.startswith("#") or len(cols) < 7 or cols[5] == "-":
                continue
            if cell_size(cols[4]) is None or cols[0] in have:
                continue
            source = ripped / (cols[5] + ".png")
            if not source.is_file():
                continue
            file = f"{cols[4]}__{cols[0]}"
            shutil.copy2(source, textures / (file + ".png"))
            added.append("\t".join(cols[:5] + [file, cols[6]]))
            have.add(cols[0])
    finally:
        if scratch.exists():
            remove_tree(scratch)
    if added:
        merged = sorted(rows + added, key=lambda r: (r.split("\t")[4].lower(), r.split("\t")[0]))
        keys_path.write_text("\n".join([KEYS_HEADER] + merged) + "\n", encoding="utf-8")
    return len(added)


def upscale_atlases(atlases: List[Path], dst: Path, staging: Path, scale: int,
                    run_waifu2x: Callable[[Path, Path], None]) -> List[Path]:
    """Button-by-button copies of `atlases` in `dst` at `scale` ([claude-9b-think]
    v3: each button on its own box, find_glyph_boxes; cells when no boxes are
    found). `run_waifu2x(src, out)` upscales every PNG in folder `src` into
    folder `out` (the caller's model, noise and scale). Returns the atlases
    done; one that fails is left out (the original stays in use)."""
    cells_dir = staging / "cells"
    up_dir = staging / "cells_up"
    plans = {}
    for atlas in atlases:
        size = cell_size(atlas.stem)
        if size is None:
            continue
        try:
            glyphs = cut_glyphs(atlas, cells_dir)
            plans[atlas] = ("glyphs", glyphs) if glyphs is not None else ("cells", (size, cut_cells(atlas, size, cells_dir)))
        except (OSError, ValueError):
            continue
    if not plans:
        return []
    up_dir.mkdir(parents=True, exist_ok=True)
    run_waifu2x(cells_dir, up_dir)
    done = []
    for atlas, (kind, plan) in plans.items():
        try:
            if kind == "glyphs":
                join_glyphs(atlas, plan, up_dir, scale, dst / atlas.name)
            else:
                size, cells = plan
                join_cells(atlas, size, cells, up_dir, scale, dst / atlas.name)
            done.append(atlas)
        except (OSError, ValueError):
            pass
    return done
