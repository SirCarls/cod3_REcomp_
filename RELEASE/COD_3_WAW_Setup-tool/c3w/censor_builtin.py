"""[acct2-decomp] The built-in censor mode: the insignia painted over, no disc needed.

The owner (2026-11-05 06:15): "a default censor mode that can replace the flag/image with various
LGBTQ+ and just blank colors if the user wants. The setup tool should cover this." His choices:
the flags and all the insignia; pride flags and plain colours; the campaign and multiplayer; one
choice for the flags and another for the insignia.

What it paints (lists/censor_regions.tsv: rectangles, measured once from where the German release
changed the textures, shipped as coordinates only; see the file's header):
  flag      the Nazi flags and banners: the whole picture becomes the chosen design, keeping the
            cloth's folds and dirt; "blend in" only takes the symbol off, as the German release did
  insignia  uniforms, helmets, caps, a gorget, a knife, crates, barrels, aircraft and tanks: each
            insignia's rectangle gets the chosen design; "blend in" fills the symbol from the
            texels around it
  data      the normal and specular maps of those: the symbol is always filled from around it
            (a flag painted into a normal map would only make bumps)

How it reaches the game: the user texture pack (c3w/user_textures.py, `rip cod3-user-pack`), joined
with the player's own pictures (owner 2026-11-05 07:24): every build of that pack (the Textures
page's button, or the build's own step after the HD pack) first calls refresh(), which makes or
takes back the pictures for the saved settings (data/cod3/censor_builtin.json, written by the
setup tool as they are chosen). The pictures are made from the ripped originals
(data/cod3/textures; the HD copies in textures_upscaled when there are, so the HD pack's quality
stays, and user_textures.replaced_names leaves them to the upscaler) and saved in the player's
own picture folder, data/cod3/user_textures_png, under the names the pack expects, listed in
.censor_builtin.tsv there with their MD5s. A picture of the player's own with the same name is
never touched: it wins. Turning the mode off removes only the pictures this made (by their MD5s).

Multiplayer: a texture that multiplayer shares with the campaign is the same texture to the game
(the pack goes by the texture's data), so the campaign's pictures cover it in a match too (with
the game's "all" texture policy, its default). The few insignia only the multiplayer files have
(a carried flag in two sizes, three heads, a helmet) aren't in the campaign's rip: their originals
are read from the game's own multiplayer archive (assets/mp/..., stored as is), checked against
their key, and added to the rip (data/cod3/textures, as <name>__<key>.png with an hd_keys.tsv
line, listed in .censor_builtin_mp.tsv there) so the pack takes their pictures like the others;
turning the mode off takes them out again. A new rip drops them; the pack build that follows it
adds them back.

Everything is local work; nothing is downloaded.

    python3 -m c3w.censor_builtin <output folder> --status
    python3 -m c3w.censor_builtin <output folder> --flags rainbow --insignia blend [--no-pack]
          (saves the settings and builds the texture pack, which makes the pictures)
    python3 -m c3w.censor_builtin <output folder> --flags custom --flags-color "#3366cc"
    python3 -m c3w.censor_builtin <output folder> --off
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import struct
import sys
import threading
import zlib
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Dict, List, Optional, Sequence, Tuple

if __package__ in (None, ""):   # run as a file: python3 c3w/censor_builtin.py
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
    __package__ = "c3w"

from . import user_textures   # noqa: E402
from .common import Cancelled, Log, Runner, SetupError, console_sink, exe_name   # noqa: E402

try:                            # optional: much faster PNG reading and writing
    from PIL import Image as _PIL   # type: ignore
except Exception:               # noqa: BLE001 - not installed, or broken
    _PIL = None

REGIONS_NAME = "censor_regions.tsv"     # in the tool's lists/
SETTINGS_NAME = "censor_builtin.json"   # in <output>/data/cod3/
MANIFEST_NAME = ".censor_builtin.tsv"   # in the player's picture folder
UPSCALED_NAME = "textures_upscaled"     # c3w/upscale.py's OUTPUT_NAME

FLAG, INSIGNIA, DATA = "flag", "insignia", "data"
RGB = Tuple[int, int, int]

# ---------------------------------------------------------------------------
# Designs
# ---------------------------------------------------------------------------
# Stripes from the top, (colour, relative height). The usual published colours.
STRIPES: Dict[str, List[Tuple[RGB, int]]] = {
    "rainbow": [((0xE4, 0x03, 0x03), 1), ((0xFF, 0x8C, 0x00), 1), ((0xFF, 0xED, 0x00), 1),
                ((0x00, 0x80, 0x26), 1), ((0x00, 0x4D, 0xFF), 1), ((0x75, 0x07, 0x87), 1)],
    "trans": [((0x5B, 0xCE, 0xFA), 1), ((0xF5, 0xA9, 0xB8), 1), ((0xFF, 0xFF, 0xFF), 1),
              ((0xF5, 0xA9, 0xB8), 1), ((0x5B, 0xCE, 0xFA), 1)],
    "bi": [((0xD6, 0x02, 0x70), 2), ((0x9B, 0x4F, 0x96), 1), ((0x00, 0x38, 0xA8), 2)],
    "pan": [((0xFF, 0x21, 0x8C), 1), ((0xFF, 0xD8, 0x00), 1), ((0x21, 0xB1, 0xFF), 1)],
    "lesbian": [((0xD5, 0x2D, 0x00), 1), ((0xFF, 0x9A, 0x56), 1), ((0xFF, 0xFF, 0xFF), 1),
                ((0xD3, 0x62, 0xA4), 1), ((0xA3, 0x02, 0x62), 1)],
    "nonbinary": [((0xFC, 0xF4, 0x34), 1), ((0xFF, 0xFF, 0xFF), 1), ((0x9C, 0x59, 0xD1), 1),
                  ((0x2C, 0x2C, 0x2C), 1)],
    "ace": [((0x00, 0x00, 0x00), 1), ((0xA3, 0xA3, 0xA3), 1), ((0xFF, 0xFF, 0xFF), 1),
            ((0x80, 0x00, 0x80), 1)],
}
# The Progress Pride flag's chevron, from the hoist: white, pink, light blue, brown, black.
CHEVRON: List[RGB] = [(0xFF, 0xFF, 0xFF), (0xF5, 0xA9, 0xB8), (0x5B, 0xCE, 0xFA), (0x61, 0x39, 0x15),
                      (0x00, 0x00, 0x00)]
PLAIN: Dict[str, RGB] = {"black": (0x14, 0x14, 0x14), "grey": (0x80, 0x80, 0x80), "white": (0xEB, 0xEB, 0xEB)}

# (key, label) in the order the setup tool shows them.
DESIGNS: List[Tuple[str, str]] = [
    ("off", "Leave as the game has it"),
    ("blend", "Blend in (take the symbol off)"),
    ("rainbow", "Rainbow pride flag"),
    ("progress", "Progress Pride flag"),
    ("trans", "Transgender flag"),
    ("bi", "Bisexual flag"),
    ("pan", "Pansexual flag"),
    ("lesbian", "Lesbian flag"),
    ("nonbinary", "Non-binary flag"),
    ("ace", "Asexual flag"),
    ("black", "Black"),
    ("grey", "Grey"),
    ("white", "White"),
    ("custom", "A colour of your choice"),
]
DESIGN_KEYS = [k for k, _ in DESIGNS]
LABELS = dict(DESIGNS)


def is_flag_design(design: str) -> bool:
    return design in STRIPES or design == "progress"


def parse_color(text: str) -> RGB:
    """'#rrggbb' (or 'rrggbb') to a colour; SetupError for anything else."""
    s = (text or "").strip().lstrip("#")
    if len(s) != 6 or any(c not in "0123456789abcdefABCDEF" for c in s):
        raise SetupError(f"Not a colour: {text!r} (use #rrggbb, e.g. #3366cc).")
    return int(s[0:2], 16), int(s[2:4], 16), int(s[4:6], 16)


def design_color(design: str, u: float, v: float, custom: RGB = (128, 128, 128)) -> RGB:
    """The design's colour at (u, v) in [0, 1) of the area it fills (u across, v down)."""
    if design in PLAIN:
        return PLAIN[design]
    if design == "custom":
        return custom
    if design == "progress":
        # Chevron bands pointing to the fly; each band 1/12 of the height wide.
        d = (u * 1.5 + abs(v - 0.5)) * 12.0       # the flag is 3:2; contours are ">" shapes
        k = int(d)
        if k < len(CHEVRON):
            return CHEVRON[k]
        design = "rainbow"
    stripes = STRIPES[design]
    total = sum(w for _, w in stripes)
    t = min(max(v, 0.0), 0.999999) * total
    for color, w in stripes:
        if t < w:
            return color
        t -= w
    return stripes[-1][0]


# ---------------------------------------------------------------------------
# Settings
# ---------------------------------------------------------------------------
@dataclass
class Settings:
    flags: str = "off"
    insignia: str = "off"
    flags_color: str = "#808080"
    insignia_color: str = "#808080"

    @property
    def on(self) -> bool:
        return self.flags != "off" or self.insignia != "off"

    def check(self) -> "Settings":
        for name in ("flags", "insignia"):
            if getattr(self, name) not in DESIGN_KEYS:
                raise SetupError(f"Unknown design for the {name}: {getattr(self, name)!r} "
                                 f"(one of {', '.join(DESIGN_KEYS)}).")
        parse_color(self.flags_color)
        parse_color(self.insignia_color)
        return self


def settings_path(data_dir: Path) -> Path:
    return Path(data_dir) / SETTINGS_NAME


def load_settings(data_dir: Path) -> Settings:
    try:
        raw = json.loads(settings_path(data_dir).read_text(encoding="utf-8"))
        s = Settings(**{k: str(raw[k]) for k in ("flags", "insignia", "flags_color", "insignia_color") if k in raw})
        return s.check()
    except (OSError, ValueError, TypeError, SetupError):
        return Settings()


def save_settings(data_dir: Path, s: Settings) -> None:
    s.check()
    p = settings_path(data_dir)
    p.parent.mkdir(parents=True, exist_ok=True)
    tmp = p.with_name(p.name + ".part")
    tmp.write_text(json.dumps(s.__dict__, indent=2) + "\n", encoding="utf-8")
    tmp.replace(p)


# ---------------------------------------------------------------------------
# Regions
# ---------------------------------------------------------------------------
@dataclass
class Region:
    key: str
    name: str
    width: int
    height: int
    role: str
    rects: List[Tuple[int, int, int, int]]
    where: str = "campaign"     # "campaign", or the multiplayer archive a multiplayer-only texture is in

    def mask_at(self, w: int, h: int) -> bytearray:
        """What "blend in" fills at size w x h (the original's or a whole multiple): each rectangle
        with its corners rounded. 1 byte per texel."""
        out = bytearray(w * h)
        for x0, y0, x1, y1 in self.rects_at(w, h):
            cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
            rx, ry = (x1 - x0) / 2 * 1.3, (y1 - y0) / 2 * 1.3
            for y in range(y0, y1):
                dy = ((y + 0.5 - cy) / ry) ** 2
                if dy > 1.0:
                    continue
                half = rx * (1.0 - dy) ** 0.5
                a, b = max(x0, int(cx - half + 0.5)), min(x1, int(cx + half + 0.5))
                if a < b:
                    out[y * w + a:y * w + b] = b"\x01" * (b - a)
        return out

    def rects_at(self, w: int, h: int) -> List[Tuple[int, int, int, int]]:
        return [(x0 * w // self.width, y0 * h // self.height, -(-x1 * w // self.width), -(-y1 * h // self.height))
                for x0, y0, x1, y1 in self.rects]


def regions_path(tool_root: Path) -> Path:
    return Path(tool_root) / "lists" / REGIONS_NAME


def parse_regions(text: str) -> List[Region]:
    """The rows of censor_regions.tsv (tab-separated, # comments); malformed rows are skipped."""
    out = []
    for line in text.splitlines():
        if not line.strip() or line.startswith("#"):
            continue
        c = line.rstrip("\r").split("\t")
        if len(c) < 7 or len(c[0]) != 16 or c[4] not in (FLAG, INSIGNIA, DATA):
            continue
        try:
            int(c[0], 16)
            w, h = int(c[2]), int(c[3])
            rects = [tuple(int(v) for v in r.split(",")) for r in c[5].split(";") if r]
        except ValueError:
            continue
        if not (0 < w <= 8192 and 0 < h <= 8192):
            continue
        if any(len(r) != 4 or not (0 <= r[0] < r[2] <= w and 0 <= r[1] < r[3] <= h) for r in rects):
            continue
        if c[4] != FLAG and not rects:
            continue
        where = c[6].strip()
        if where != "campaign" and not (where.startswith("mp/") and where.endswith(".cod") and ".." not in where
                                        and "\\" not in where):
            continue
        out.append(Region(c[0].lower(), c[1], w, h, c[4], [tuple(r) for r in rects], where))
    return out


def read_regions(tool_root: Path) -> List[Region]:
    try:
        return parse_regions(regions_path(tool_root).read_text(encoding="utf-8"))
    except OSError:
        return []


# ---------------------------------------------------------------------------
# PNG (8-bit; Pillow when it's there, plain Python otherwise)
# ---------------------------------------------------------------------------
_MASKS: Dict[int, Tuple[int, int, int]] = {}


def _masks(n: int) -> Tuple[int, int, int]:
    m = _MASKS.get(n)
    if m is None:
        m = (int.from_bytes(b"\x7f" * n, "little"), int.from_bytes(b"\x80" * n, "little"), (1 << (8 * n)) - 1)
        _MASKS[n] = m
    return m


def _add8(x: int, y: int, lo: int, hi: int) -> int:
    """Byte-wise (x + y) mod 256 of two little-endian byte strings held as ints."""
    return ((x & lo) + (y & lo)) ^ ((x ^ y) & hi)


def _unfilter(kind: int, line: bytes, prev: bytes, bpp: int) -> bytes:
    n = len(line)
    if kind == 0:
        return line
    lo, hi, full = _masks(n)
    if kind == 2:   # Up
        return _add8(int.from_bytes(line, "little"), int.from_bytes(prev, "little"), lo, hi).to_bytes(n, "little")
    if kind == 1:   # Sub: a running sum per channel, by doubling
        x = int.from_bytes(line, "little")
        step = 8 * bpp
        while step < 8 * n:
            x = _add8(x, (x << step) & full, lo, hi)
            step <<= 1
        return x.to_bytes(n, "little")
    out = bytearray(line)
    if kind == 3:   # Average
        for i in range(n):
            a = out[i - bpp] if i >= bpp else 0
            out[i] = (out[i] + ((a + prev[i]) >> 1)) & 0xFF
        return bytes(out)
    if kind == 4:   # Paeth
        for i in range(n):
            a = out[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            p = a + b - c
            pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
            out[i] = (out[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 0xFF
        return bytes(out)
    raise ValueError(f"PNG filter {kind}")


def read_png(path: Path) -> Tuple[int, int, bytearray]:
    """(width, height, RGBA bytes). ValueError for a PNG this can't read."""
    if _PIL is not None:
        try:
            with _PIL.open(path) as im:
                im = im.convert("RGBA")
                return im.width, im.height, bytearray(im.tobytes())
        except OSError as e:
            raise ValueError(f"{Path(path).name}: {e}") from e
    data = Path(path).read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{Path(path).name}: not a PNG")
    pos, idat, plte, trns = 8, [], b"", b""
    w = h = depth = ctype = interlace = 0
    while pos + 8 <= len(data):
        length, tag = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if tag == b"IHDR":
            w, h, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", body)
        elif tag == b"PLTE":
            plte = body
        elif tag == b"tRNS":
            trns = body
        elif tag == b"IDAT":
            idat.append(body)
        elif tag == b"IEND":
            break
    ch = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}.get(ctype)
    if depth != 8 or ch is None or interlace or not (0 < w <= 16384 and 0 < h <= 16384):
        raise ValueError(f"{Path(path).name}: {depth}-bit colour type {ctype} (interlace {interlace}) not supported")
    raw = zlib.decompress(b"".join(idat))
    stride = w * ch
    if len(raw) < h * (stride + 1):
        raise ValueError(f"{Path(path).name}: the picture data is cut short")
    rows, prev, i = [], bytes(stride), 0
    for _ in range(h):
        cur = _unfilter(raw[i], raw[i + 1:i + 1 + stride], prev, ch)
        rows.append(cur)
        prev = cur
        i += 1 + stride
    px = b"".join(rows)
    n = w * h
    out = bytearray(n * 4)
    if ctype == 6:
        out[:] = px
    elif ctype == 2:
        out[0::4], out[1::4], out[2::4], out[3::4] = px[0::3], px[1::3], px[2::3], b"\xff" * n
    elif ctype == 0:
        out[0::4] = out[1::4] = out[2::4] = px
        out[3::4] = b"\xff" * n
    elif ctype == 4:
        out[0::4] = out[1::4] = out[2::4] = px[0::2]
        out[3::4] = px[1::2]
    else:
        pal = plte + bytes(768 - len(plte))
        alpha = trns + b"\xff" * (256 - len(trns))
        out[0::4] = px.translate(bytes(pal[k * 3] for k in range(256)))
        out[1::4] = px.translate(bytes(pal[k * 3 + 1] for k in range(256)))
        out[2::4] = px.translate(bytes(pal[k * 3 + 2] for k in range(256)))
        out[3::4] = px.translate(alpha[:256])
    return w, h, out


def write_png(path: Path, w: int, h: int, rgba: bytes) -> None:
    """An RGBA PNG, written to a .part file first and then put in place."""
    path = Path(path)
    tmp = path.with_name(path.name + ".part")
    if _PIL is not None:
        _PIL.frombytes("RGBA", (w, h), bytes(rgba)).save(tmp, format="PNG", compress_level=6)
    else:
        stride = w * 4
        raw = b"".join(b"\x00" + bytes(rgba[y * stride:(y + 1) * stride]) for y in range(h))

        def chunk(tag: bytes, body: bytes) -> bytes:
            return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF)
        tmp.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
                        + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))
    os.replace(tmp, path)


# ---------------------------------------------------------------------------
# Painting
# ---------------------------------------------------------------------------
def inpaint(px: bytearray, w: int, h: int, mask: bytearray, smooth: int = 24) -> int:
    """Fills the texels where mask is 1 from the texels around them (layer by layer from the
    edge, then smoothed); alpha is kept. Returns how many texels it filled."""
    todo = [i for i, m in enumerate(mask) if m]
    if not todo:
        return 0
    known = bytearray(1 if not m else 0 for m in mask)
    if not any(known):
        return 0

    def neighbours(i: int):
        x, y = i % w, i // w
        for dy in (-1, 0, 1):
            yy = y + dy
            if 0 <= yy < h:
                for dx in (-1, 0, 1):
                    xx = x + dx
                    if (dx or dy) and 0 <= xx < w:
                        yield yy * w + xx

    frontier = [i for i in todo if any(known[j] for j in neighbours(i))]
    while frontier:
        vals = []
        for i in frontier:
            r = g = b = n = 0
            for j in neighbours(i):
                if known[j]:
                    o = j * 4
                    r += px[o]; g += px[o + 1]; b += px[o + 2]; n += 1
            vals.append((i, r // n, g // n, b // n))
        nxt = set()
        for i, r, g, b in vals:
            o = i * 4
            px[o], px[o + 1], px[o + 2] = r, g, b
            known[i] = 1
        for i, *_ in vals:
            for j in neighbours(i):
                if not known[j]:
                    nxt.add(j)
        frontier = sorted(nxt)
    for _ in range(smooth):             # Gauss-Seidel: each filled texel becomes its 4 neighbours' mean
        for i in todo:
            x, y = i % w, i // w
            r = g = b = n = 0
            for j in ((i - 1) if x else -1, (i + 1) if x + 1 < w else -1, (i - w) if y else -1,
                      (i + w) if y + 1 < h else -1):
                if j >= 0:
                    o = j * 4
                    r += px[o]; g += px[o + 1]; b += px[o + 2]; n += 1
            o = i * 4
            px[o], px[o + 1], px[o + 2] = r // n, g // n, b // n
    return len(todo)


def _luma(px: bytes, i: int) -> float:
    o = i * 4
    return 0.299 * px[o] + 0.587 * px[o + 1] + 0.114 * px[o + 2]


def _box_blur(vals: List[float], w: int, h: int, r: int) -> List[float]:
    """A box blur of radius r (each way), done along rows then columns with running sums."""
    tmp = [0.0] * (w * h)
    for y in range(h):
        o = y * w
        acc = 0.0
        row = vals[o:o + w]
        pre = [0.0]
        for v in row:
            acc += v
            pre.append(acc)
        for x in range(w):
            a, b = max(0, x - r), min(w, x + r + 1)
            tmp[o + x] = (pre[b] - pre[a]) / (b - a)
    out = [0.0] * (w * h)
    for x in range(w):
        acc = 0.0
        pre = [0.0]
        for y in range(h):
            acc += tmp[y * w + x]
            pre.append(acc)
        for y in range(h):
            a, b = max(0, y - r), min(h, y + r + 1)
            out[y * w + x] = (pre[b] - pre[a]) / (b - a)
    return out


def paint_flag(px: bytearray, w: int, h: int, design: str, custom: RGB, mask: bytearray,
               rects: Sequence[Tuple[int, int, int, int]]) -> None:
    """The whole picture becomes `design`, shaded with the cloth's folds and dirt (the symbol is
    taken off first, and the disc it sat on is left out of the shading)."""
    base = bytearray(px)
    inpaint(base, w, h, mask, smooth=12)
    lum = [_luma(base, i) for i in range(w * h)]
    r = max(2, min(w, h) // 48)
    fine = _box_blur(lum, w, h, r)
    broad = _box_blur(lum, w, h, max(r * 6, 8))
    mean = sum(broad) / len(broad) or 1.0
    # The disc: an ellipse inside each symbol rectangle, grown a little; flat inside, eased out.
    discs = [((x0 + x1) / 2, (y0 + y1) / 2, (x1 - x0) / 2 * 1.12 + 1, (y1 - y0) / 2 * 1.12 + 1)
             for x0, y0, x1, y1 in rects]
    ease = max(2.0, r * 2.0)
    for y in range(h):
        v = (y + 0.5) / h
        for x in range(w):
            i = y * w + x
            weight = 1.0
            for cx, cy, rx, ry in discs:
                d = ((x + 0.5 - cx) / rx) ** 2 + ((y + 0.5 - cy) / ry) ** 2
                if d < 1.0:
                    weight = 0.0
                    break
                edge = (d ** 0.5 - 1.0) * min(rx, ry) / ease
                weight = min(weight, max(0.0, min(1.0, edge)))
            detail = lum[i] / fine[i] if fine[i] > 1.0 else 1.0
            low = broad[i] / mean
            shade = 1.0 + weight * (0.85 * (min(max(detail, 0.55), 1.45) - 1.0)
                                    + 0.6 * (min(max(low, 0.5), 1.25) - 1.0))
            c = design_color(design, (x + 0.5) / w, v, custom)
            o = i * 4
            px[o] = max(0, min(255, int(c[0] * shade)))
            px[o + 1] = max(0, min(255, int(c[1] * shade)))
            px[o + 2] = max(0, min(255, int(c[2] * shade)))


def paint_rects(px: bytearray, w: int, h: int, design: str, custom: RGB,
                rects: Sequence[Tuple[int, int, int, int]]) -> None:
    """Each rectangle filled with `design`; a flag design is dimmed to the light around it."""
    for x0, y0, x1, y1 in rects:
        shade = 1.0
        if is_flag_design(design):
            ring = []
            for x in range(max(0, x0 - 2), min(w, x1 + 2)):
                for y in (y0 - 2, y1 + 1):
                    if 0 <= y < h:
                        ring.append(_luma(px, y * w + x))
            for y in range(max(0, y0 - 2), min(h, y1 + 2)):
                for x in (x0 - 2, x1 + 1):
                    if 0 <= x < w:
                        ring.append(_luma(px, y * w + x))
            if ring:
                shade = min(1.0, max(0.55, (sum(ring) / len(ring)) / 150.0))
        rw, rh = x1 - x0, y1 - y0
        for y in range(y0, y1):
            v = (y - y0 + 0.5) / rh
            for x in range(x0, x1):
                c = design_color(design, (x - x0 + 0.5) / rw, v, custom)
                o = (y * w + x) * 4
                px[o] = int(c[0] * shade)
                px[o + 1] = int(c[1] * shade)
                px[o + 2] = int(c[2] * shade)


def paint(region: Region, px: bytearray, w: int, h: int, design: str, custom: RGB) -> None:
    """Paints one picture (at the original's size or a whole multiple of it) in place."""
    mask = region.mask_at(w, h)
    rects = region.rects_at(w, h)
    if region.role == DATA or design == "blend":
        inpaint(px, w, h, mask)
    elif region.role == FLAG:
        paint_flag(px, w, h, design, custom, mask, rects)
    else:
        paint_rects(px, w, h, design, custom, rects)


def design_for(region: Region, s: Settings) -> Tuple[str, RGB]:
    """(design, custom colour) for a region with these settings; "off" leaves it alone."""
    if region.role == FLAG:
        design, color = s.flags, s.flags_color
    else:
        design, color = s.insignia, s.insignia_color
    if region.role == DATA and design != "off":
        design = "blend"
    if design == "blend" and not region.rects:
        design = "off"            # nothing to take off (the war flag already has no swastika)
    return design, parse_color(color)


# ---------------------------------------------------------------------------
# Multiplayer-only textures: the originals, from the game's multiplayer archives
# ---------------------------------------------------------------------------
MP_MANIFEST_NAME = ".censor_builtin_mp.tsv"     # in data/cod3/textures
_TEX_TYPE = 5784916                             # APKF item type of a texture ("TEX")
_BLOCK_BYTES = {18: 8, 19: 16, 20: 16}          # DXT1, DXT3, DXT5
_M64 = (1 << 64) - 1
_P1, _P2, _P3, _P4, _P5 = (0x9E3779B185EBCA87, 0xC2B2AE3D27D4EB4F, 0x165667B19E3779F9,
                           0x85EBCA77C2B2AE63, 0x27D4EB2F165667C5)


def _rotl(x: int, r: int) -> int:
    return ((x << r) | (x >> (64 - r))) & _M64


def _xround(acc: int, lane: int) -> int:
    return (_rotl((acc + lane * _P2) & _M64, 31) * _P1) & _M64


def xxh64(data: bytes, seed: int = 0) -> int:
    """XXH64, the hash hd_keys.tsv's keys are (of a texture's untiled base level)."""
    n, p = len(data), 0
    if n >= 32:
        v = [(seed + _P1 + _P2) & _M64, (seed + _P2) & _M64, seed & _M64, (seed - _P1) & _M64]
        lanes = struct.unpack_from(f"<{(n // 32) * 4}Q", data)
        for i in range(0, len(lanes), 4):
            v[0] = _xround(v[0], lanes[i])
            v[1] = _xround(v[1], lanes[i + 1])
            v[2] = _xround(v[2], lanes[i + 2])
            v[3] = _xround(v[3], lanes[i + 3])
        h = (_rotl(v[0], 1) + _rotl(v[1], 7) + _rotl(v[2], 12) + _rotl(v[3], 18)) & _M64
        for x in v:
            h = (((h ^ _xround(0, x)) * _P1) + _P4) & _M64
        p = (n // 32) * 32
    else:
        h = (seed + _P5) & _M64
    h = (h + n) & _M64
    while p + 8 <= n:
        h = ((_rotl(h ^ _xround(0, struct.unpack_from("<Q", data, p)[0]), 27) * _P1) + _P4) & _M64
        p += 8
    if p + 4 <= n:
        h = ((_rotl(h ^ ((struct.unpack_from("<I", data, p)[0] * _P1) & _M64), 23) * _P2) + _P3) & _M64
        p += 4
    while p < n:
        h = (_rotl(h ^ ((data[p] * _P5) & _M64), 11) * _P1) & _M64
        p += 1
    h ^= h >> 33
    h = (h * _P2) & _M64
    h ^= h >> 29
    h = (h * _P3) & _M64
    return h ^ (h >> 32)


def _tiled(x: int, y: int, wb: int, bpb: int) -> int:
    """Block index of block (x, y) in a tiled surface wb blocks wide (XGAddress2DTiledOffset)."""
    aligned = (wb + 31) & ~31
    lb = (bpb >> 2) + ((bpb >> 1) >> (bpb >> 2))
    macro = ((x >> 5) + (y >> 5) * (aligned >> 5)) << (lb + 7)
    micro = ((x & 7) + ((y & 6) << 2)) << lb
    off = macro + ((micro & ~15) << 1) + (micro & 15) + ((y & 8) << (3 + lb)) + ((y & 1) << 4)
    bank = ((((y & 8) >> 2) + (x >> 3)) & 3) << 6
    return (((off & ~511) << 3) + ((off & 448) << 2) + (off & 63) + ((y & 16) << 7) + bank) >> lb


def base_level(obj: bytes, gpu: bytes) -> Optional[Tuple[int, int, int, bytes]]:
    """(width, height, format, the base level untiled and byte-swapped) of a DXT1/3/5 texture, or
    None for one this doesn't read (the fetch constant is the object's last 6 words)."""
    if len(obj) < 0x60:
        return None
    w = struct.unpack_from(">6I", obj, 0x48)
    fmt, endian, dim = w[1] & 0x3F, (w[1] >> 6) & 3, (w[5] >> 9) & 3
    width, height = (w[2] & 0x1FFF) + 1, ((w[2] >> 13) & 0x1FFF) + 1
    packed = (w[5] >> 11) & 1
    if fmt not in _BLOCK_BYTES or endian != 1 or dim != 1 or (packed and min(width, height) <= 16):
        return None
    bpb = _BLOCK_BYTES[fmt]
    wb, hb = -(-width // 4), -(-height // 4)
    pb = max(((w[0] >> 22) & 0x1FF) * 32 // 4, wb)
    tiled = w[0] >> 31
    out = bytearray(wb * hb * bpb)
    o = 0
    for y in range(hb):
        for x in range(wb):
            src = (_tiled(x, y, pb, bpb) if tiled else y * pb + x) * bpb
            if src + bpb > len(gpu):
                return None
            out[o:o + bpb] = gpu[src:src + bpb]
            o += bpb
    out[0::2], out[1::2] = out[1::2], out[0::2]         # 8-in-16 swap
    return width, height, fmt, bytes(out)


def _rgb565(c: int) -> RGB:
    r, g, b = (c >> 11) & 31, (c >> 5) & 63, c & 31
    return (r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)


def decode_dxt(lin: bytes, width: int, height: int, fmt: int) -> bytearray:
    """RGBA of a linear DXT1/3/5 base level."""
    bpb = _BLOCK_BYTES[fmt]
    wb = -(-width // 4)
    out = bytearray(width * height * 4)
    for bi in range(len(lin) // bpb):
        o = bi * bpb
        bx, by = (bi % wb) * 4, (bi // wb) * 4
        alpha = [255] * 16
        if fmt == 19:
            a = int.from_bytes(lin[o:o + 8], "little")
            alpha = [((a >> (4 * i)) & 15) * 17 for i in range(16)]
            o += 8
        elif fmt == 20:
            a0, a1 = lin[o], lin[o + 1]
            if a0 > a1:
                pal = [a0, a1] + [((7 - i) * a0 + i * a1) // 7 for i in range(1, 7)]
            else:
                pal = [a0, a1] + [((5 - i) * a0 + i * a1) // 5 for i in range(1, 5)] + [0, 255]
            bits = int.from_bytes(lin[o + 2:o + 8], "little")
            alpha = [pal[(bits >> (3 * i)) & 7] for i in range(16)]
            o += 8
        c0, c1, idx = struct.unpack_from("<HHI", lin, o)
        p0, p1 = _rgb565(c0), _rgb565(c1)
        if c0 > c1 or fmt != 18:
            p2 = tuple((2 * a + b) // 3 for a, b in zip(p0, p1))
            p3 = tuple((a + 2 * b) // 3 for a, b in zip(p0, p1))
            cut = False
        else:
            p2 = tuple((a + b) // 2 for a, b in zip(p0, p1))
            p3, cut = (0, 0, 0), True
        pal = (p0, p1, p2, p3)
        for i in range(16):
            x, y = bx + (i & 3), by + (i >> 2)
            if x < width and y < height:
                k = (idx >> (2 * i)) & 3
                d = (y * width + x) * 4
                c = pal[k]
                out[d], out[d + 1], out[d + 2] = c
                out[d + 3] = 0 if (cut and k == 3) else alpha[i]
    return out


def _up(x: int, a: int) -> int:
    return (x + a - 1) // a * a if a > 1 else x


def apkf_textures(ch: bytes):
    """(name, object, GPU data) of each texture in an APKF chunk (docs/formats/cod3.md)."""
    if ch[:4] != b"APKF" or len(ch) < 0x20:
        return
    be = lambda o: struct.unpack_from(">I", ch, o)[0]   # noqa: E731
    areas, dir_end = be(0x10), be(0x14)
    if areas != 2 or not 0x1C < dir_end < len(ch):
        return
    rec = (areas + 5) * 4
    o, first, recs = 0x1C, None, []
    while first is None or o + rec <= first:
        w = [be(o + 4 * i) for i in range(areas + 5)]
        tbl = (o + 12 + w[3]) & 0xFFFFFFFF
        if not 0x1C < tbl < dir_end:
            return
        first = tbl if first is None else min(first, tbl)
        recs.append((w, tbl))
        o += rec
    types = []
    for w, tbl in recs:
        parts, items = w[2], []
        if not 1 <= parts <= 2:
            return
        for k in range(w[4]):
            e = tbl + k * (parts + 1) * 4
            nm = (e + be(e)) & 0xFFFFFFFF
            z = ch.find(b"\0", nm + 4)
            if z < 0:
                return
            items.append([ch[nm + 4:z].decode("latin-1"), [be(e + 4 + 4 * j) for j in range(parts)], 0, 0])
        types.append((w, items))
    pos = _up(dir_end + 0x14 + 0x18 * areas, be(dir_end + 0x14 + 8))
    for w, items in types:
        for it in items:
            it[2] = _up(pos, max(1, w[5]))
            pos = it[2] + it[1][0]
    pos = _up(pos, 0x10000)
    for w, items in types:
        if w[2] < 2:
            continue
        for it in items:
            it[3] = _up(pos, 1 if w[6] == 0xFF000000 else max(1, w[6] & 0xFFFFFF))
            pos = it[3] + it[1][1]
    if pos > len(ch):
        return
    for w, items in types:
        if w[0] == _TEX_TYPE and w[2] == 2:
            for name, sizes, ob, gp in items:
                yield name, ch[ob:ob + sizes[0]], ch[gp:gp + sizes[1]]


def kapf_main_chunks(path: Path):
    """The unnamed section's chunks of a .cod archive, when stored as is (the multiplayer
    archives are); packed ones are skipped."""
    with open(path, "rb") as f:
        head = f.read(0x30)
        if head[:4] != b"KAPF":
            return
        nsec = struct.unpack_from("<I", head, 12)[0]
        recs = f.read(0x18 * min(nsec, 64))
        for i in range(min(nsec, 64)):
            r = recs[0x18 * i:0x18 * (i + 1)]
            if len(r) < 0x18 or r[:12].split(b"\0")[0]:
                continue
            count, _, table, _ = struct.unpack_from("<HHII", r, 12)
            f.seek(table)
            ents = f.read(0x20 * count)
            for k in range(count):
                off, size, _slot, _flags, stored = struct.unpack_from("<5I", ents, 0x20 * k)
                if stored:
                    continue
                f.seek(off)
                yield f.read(size)


def mp_original(assets: Path, region: Region) -> Optional[Tuple[int, int, int, bytearray]]:
    """(width, height, format, RGBA) of a multiplayer-only texture, from its archive, if the key
    matches; None when the archive or the texture isn't there."""
    path = Path(assets) / region.where
    if not path.is_file():
        return None
    want = int(region.key, 16)
    for ch in kapf_main_chunks(path):
        for name, obj, gpu in apkf_textures(ch):
            b = base_level(obj, gpu)
            if b is None or (b[0], b[1]) != (region.width, region.height):
                continue
            if xxh64(b[3]) == want:
                return b[0], b[1], b[2], decode_dxt(b[3], b[0], b[1], b[2])
    return None


def _read_mp_manifest(textures: Path) -> Dict[str, str]:
    """file -> hd_keys.tsv line of the multiplayer originals this added."""
    out: Dict[str, str] = {}
    try:
        for line in (Path(textures) / MP_MANIFEST_NAME).read_text(encoding="utf-8").splitlines():
            if line and not line.startswith("#") and "\t" in line:
                file, rest = line.split("\t", 1)
                out[file] = rest
    except OSError:
        pass
    return out


def _write_text(path: Path, text: str) -> None:
    tmp = path.with_name(path.name + ".part")
    tmp.write_text(text, encoding="utf-8")
    tmp.replace(path)


def sync_mp_originals(data_dir: Path, want: Dict[str, Tuple[Region, Optional[Tuple[int, int, int, bytearray]]]]
                      ) -> None:
    """Puts the multiplayer originals in `want` (file -> (region, picture, or None for one already
    there)) into the rip with their hd_keys.tsv lines, and takes out the ones this added before
    that aren't wanted now."""
    textures = Path(data_dir) / "textures"
    keys_path = textures / "hd_keys.tsv"
    added = _read_mp_manifest(textures)
    text = keys_path.read_text(encoding="utf-8", errors="replace")
    drop = {f for f in added if f not in want}
    lines = [l for l in text.splitlines() if not (l.count("\t") >= 6 and l.split("\t")[5] in drop)]
    have = {l.split("\t")[5] for l in lines if l.count("\t") >= 6}
    for file, (reg, pic) in want.items():
        png = textures / f"{file}.png"
        if pic is not None and not png.is_file():
            write_png(png, pic[0], pic[1], pic[3])
        if pic is not None:
            fmt = str(pic[2])
        else:
            old = added.get(file, "").split("\t")
            fmt = old[3] if len(old) > 3 else "18"
        line = f"{reg.key}\t{reg.width}\t{reg.height}\t{fmt}\t{reg.name}\t{file}\tdiffuse"
        if file not in have:
            lines.append(line)
        added[file] = line
    for file in [f for f in added if f not in want]:
        try:
            (textures / f"{file}.png").unlink()
        except OSError:
            pass
        del added[file]
    new_text = "\n".join(lines) + "\n"
    if new_text != text:
        _write_text(keys_path, new_text)
    mp = textures / MP_MANIFEST_NAME
    if added:
        _write_text(mp, "# [acct2-decomp] Multiplayer originals the built-in censor mode added to this rip "
                        "(c3w/censor_builtin.py): file, then its hd_keys.tsv line.\n"
                    + "".join(f"{f}\t{l}\n" for f, l in sorted(added.items())))
    elif mp.exists():
        mp.unlink()


# ---------------------------------------------------------------------------
# Applying
# ---------------------------------------------------------------------------
def key_files(textures_dir: Path) -> Dict[str, str]:
    """hd_keys.tsv: key -> the ripped picture's name (without .png); textures the pack leaves
    alone ("-") are left out."""
    out: Dict[str, str] = {}
    try:
        text = (Path(textures_dir) / "hd_keys.tsv").read_text(encoding="utf-8", errors="replace")
    except OSError:
        return out
    for line in text.splitlines():
        if not line or line.startswith("#"):
            continue
        c = line.split("\t")
        if len(c) >= 6 and len(c[0]) == 16 and c[5] and c[5] != "-":
            out.setdefault(c[0].lower(), c[5])
    return out


def _md5(path: Path) -> str:
    h = hashlib.md5()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def read_manifest(folder: Path) -> Dict[str, Tuple[str, str]]:
    """file (picture name) -> (md5, what it is) of the pictures this made."""
    out: Dict[str, Tuple[str, str]] = {}
    try:
        text = (Path(folder) / MANIFEST_NAME).read_text(encoding="utf-8")
    except OSError:
        return out
    for line in text.splitlines():
        c = line.split("\t")
        if len(c) >= 3 and not line.startswith("#") and len(c[1]) == 32:
            out[c[0]] = (c[1], c[2])
    return out


def write_manifest(folder: Path, entries: Dict[str, Tuple[str, str]]) -> None:
    p = Path(folder) / MANIFEST_NAME
    if not entries:
        if p.exists():
            p.unlink()
        return
    text = ("# [acct2-decomp] Pictures the built-in censor mode made (c3w/censor_builtin.py): name, MD5, design.\n"
            "# A picture whose MD5 no longer matches was changed by you: it is yours then and left alone.\n"
            + "".join(f"{k}\t{m}\t{d}\n" for k, (m, d) in sorted(entries.items())))
    tmp = p.with_name(p.name + ".part")
    tmp.write_text(text, encoding="utf-8")
    tmp.replace(p)


def _safe_name(file: str) -> bool:
    return bool(file) and "/" not in file and "\\" not in file and ":" not in file and file not in (".", "..") \
        and not file.startswith(".") and all(ord(c) >= 0x20 for c in file)


def _source(data_dir: Path, file: str, region: Region) -> Path:
    """The picture to paint: the HD copy when there is a whole multiple of the original, else the
    ripped original."""
    hd = Path(data_dir) / UPSCALED_NAME / f"{file}.png"
    if hd.is_file():
        try:
            with open(hd, "rb") as f:
                head = f.read(24)
            w, h = struct.unpack(">II", head[16:24])
            if head[:8] == b"\x89PNG\r\n\x1a\n" and w % region.width == 0 and h % region.height == 0 \
                    and w // region.width == h // region.height >= 1:
                return hd
        except (OSError, struct.error):
            pass
    return Path(data_dir) / "textures" / f"{file}.png"


@dataclass
class Result:
    made: List[str] = field(default_factory=list)       # pictures written (or rewritten)
    kept: List[str] = field(default_factory=list)       # the player's own picture has this name
    removed: List[str] = field(default_factory=list)    # pictures this had made, taken away
    missing: List[str] = field(default_factory=list)    # not in this rip, nor in the multiplayer files
    from_mp: List[str] = field(default_factory=list)    # originals read from the multiplayer files
    failed: List[str] = field(default_factory=list)     # "name: why"

    def summary(self) -> str:
        parts = [f"{len(self.made)} picture(s) made"]
        if self.removed:
            parts.append(f"{len(self.removed)} removed")
        if self.kept:
            parts.append(f"{len(self.kept)} of your own kept")
        if self.from_mp:
            parts.append(f"{len(self.from_mp)} from the multiplayer files")
        if self.missing:
            parts.append(f"{len(self.missing)} not found")
        if self.failed:
            parts.append(f"{len(self.failed)} failed")
        return ", ".join(parts)


def apply(data_dir: Path, tool_root: Path, s: Settings, log: Log,
          cancel: Optional[threading.Event] = None) -> Result:
    """Makes (or removes) the pictures for settings `s` in the player's picture folder and saves
    the settings. The pack build calls it through refresh() before it packs."""
    cancel = cancel or threading.Event()
    s.check()
    data_dir = Path(data_dir)
    textures = data_dir / "textures"
    if s.on and not (textures / "hd_keys.tsv").is_file():
        raise SetupError(f"No ripped textures with hd_keys.tsv in {textures}: build (rip the game) first.")
    regions = read_regions(tool_root)
    if s.on and not regions:
        raise SetupError(f"The list of what to paint ({regions_path(tool_root)}) is missing: reinstall the setup tool.")
    folder = user_textures.prepare_folder(data_dir, tool_root)
    if folder.is_symlink():
        raise SetupError(f"{folder} is a link: the pictures are only written into the game's own folder.")
    files = key_files(textures)
    manifest = read_manifest(folder)
    res = Result()
    wanted: Dict[str, Tuple[str, str]] = {}
    todo = []
    mp_want: Dict[str, Tuple[Region, Optional[Tuple[int, int, int, bytearray]]]] = {}
    mp_added = _read_mp_manifest(textures)
    assets = Path(data_dir).parent.parent / "assets"
    for reg in regions:
        design, custom = design_for(reg, s)
        if design == "off":
            continue
        file = files.get(reg.key)
        if reg.where != "campaign" and (file is None or file in mp_added):
            file = f"{reg.name}__{reg.key}"
            if not _safe_name(file):
                res.failed.append(f"{file}: an odd name")
                continue
            got = None
            if file not in mp_added or not (textures / f"{file}.png").is_file():
                if cancel.is_set():
                    raise Cancelled()
                log.progress(0.0, f"built-in censor: reading {reg.where}")
                got = mp_original(assets, reg)
                if got is None:
                    res.missing.append(reg.name)
                    continue
            mp_want[file] = (reg, got)
            res.from_mp.append(file)
        if file is None:
            res.missing.append(reg.name)
            continue
        if not _safe_name(file):
            res.failed.append(f"{file}: an odd picture name in hd_keys.tsv")
            continue
        todo.append((reg, file, design, custom))
    if (textures / "hd_keys.tsv").is_file():
        sync_mp_originals(data_dir, mp_want)
    total = len(todo) or 1
    for n, (reg, file, design, custom) in enumerate(todo):
        if cancel.is_set():
            raise Cancelled()
        log.progress(n / total, f"built-in censor: {file}")
        out = folder / f"{file}.png"
        what = f"{design}" + (f" {s.flags_color if reg.role == FLAG else s.insignia_color}" if design == "custom" else "")
        if out.is_symlink():
            res.kept.append(file)
            continue
        src = _source(data_dir, file, reg)
        tag = f"{what}@{reg.key}@{_png_size(src)}"    # the source's size: another HD scale makes it again
        if out.exists():
            mine = manifest.get(file)
            if mine is None or _md5(out) != mine[0]:
                res.kept.append(file)      # the player's own picture (or one changed by hand): it wins
                continue
            if mine[1] == tag:
                wanted[file] = mine        # already made with these settings, from a source this size
                continue
        try:
            w, h, px = read_png(src)
            if w % reg.width or h % reg.height or w // reg.width != h // reg.height:
                raise ValueError(f"{src.name} is {w}x{h}, not the {reg.width}x{reg.height} texture listed "
                                 "(rip the game again?)")
            paint(reg, px, w, h, design, custom)
            write_png(out, w, h, px)
            wanted[file] = (_md5(out), tag)
            res.made.append(file)
        except (OSError, ValueError, zlib.error) as e:
            res.failed.append(f"{file}: {e}")
    # Pictures made before that these settings no longer want: removed if still as made.
    for file, (md5, _) in manifest.items():
        if file in wanted:
            continue
        p = folder / f"{file}.png"
        try:
            if p.is_file() and not p.is_symlink() and _md5(p) == md5:
                p.unlink()
                res.removed.append(file)
        except OSError as e:
            res.failed.append(f"{file}: {e}")
    write_manifest(folder, wanted)
    save_settings(data_dir, s)
    log.progress(1.0, "built-in censor: pictures ready")
    log.info(f"Built-in censor: {res.summary()} in {folder}.")
    for file in res.kept:
        log.info(f"Built-in censor: kept your own picture {file}.png (it wins over the built-in one).")
    if res.from_mp:
        log.info(f"Built-in censor: {len(res.from_mp)} multiplayer-only original(s) read from assets/mp and added "
                 "to the rip.")
    if res.missing:
        log.info(f"Built-in censor: not found (not in your rip, nor in assets/mp): {', '.join(sorted(set(res.missing)))}.")
    for why in res.failed:
        log.warn(f"Built-in censor: {why}")
    return res


def _png_size(path: Path) -> str:
    """"WxH" from a PNG's header, "" when it can't be read."""
    try:
        with open(path, "rb") as f:
            head = f.read(24)
        if head[:8] == b"\x89PNG\r\n\x1a\n":
            w, h = struct.unpack(">II", head[16:24])
            return f"{w}x{h}"
    except (OSError, struct.error):
        pass
    return ""


def made_names(data_dir: Path) -> set:
    """The pictures in the player's folder that this made and nobody changed since (their MD5s
    match): the HD upscaler still makes those textures' copies (they are painted from them)."""
    folder = user_textures.png_dir(Path(data_dir))
    out = set()
    for file, (md5, _) in read_manifest(folder).items():
        p = folder / f"{file}.png"
        try:
            if p.is_file() and not p.is_symlink() and _md5(p) == md5:
                out.add(file)
        except OSError:
            pass
    return out


def refresh(data_dir: Path, tool_root: Path, log: Log, cancel: Optional[threading.Event] = None
            ) -> Optional[Result]:
    """Called by every build of the user texture pack (user_textures.build) before it packs: the
    pictures for the saved settings are made, changed or taken back (and the multiplayer-only
    originals put back into a new rip). Nothing to do (None) when the mode is off and nothing of
    it is left."""
    data_dir = Path(data_dir)
    s = load_settings(data_dir)
    if not s.on and not read_manifest(user_textures.png_dir(data_dir)) \
            and not _read_mp_manifest(data_dir / "textures"):
        return None
    return apply(data_dir, tool_root, s, log, cancel)


def build_pack(output: Path, data_dir: Path, tool_root: Path, jobs: int, runner: Runner, log: Log) -> int:
    """Builds the user texture pack (c3w/user_textures.py), which applies the saved settings first."""
    rip = user_textures.installed_rip(Path(output), exe_name("rip"))
    if rip is None:
        raise SetupError(f"No ripper in {Path(output) / 'tools'}: build first (Set up).")
    return user_textures.build(rip, Path(data_dir), Path(tool_root), jobs, runner, log)


def status(data_dir: Path) -> str:
    s = load_settings(data_dir)
    mine = read_manifest(user_textures.png_dir(Path(data_dir)))
    if not s.on:
        return "Built-in censor: off" + (f" ({len(mine)} picture(s) still to remove: build the texture pack)"
                                         if mine else "") + "."
    return (f"Built-in censor: flags {LABELS[s.flags]}{' ' + s.flags_color if s.flags == 'custom' else ''}; "
            f"insignia {LABELS[s.insignia]}{' ' + s.insignia_color if s.insignia == 'custom' else ''}; "
            f"{len(mine)} picture(s) made.")


# ---------------------------------------------------------------------------
def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(prog="python3 -m c3w.censor_builtin", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("output", help="the output folder (with data/cod3)")
    ap.add_argument("--flags", choices=DESIGN_KEYS, help="the design for the flags")
    ap.add_argument("--insignia", choices=DESIGN_KEYS, help="the design for the other insignia")
    ap.add_argument("--flags-color", metavar="#RRGGBB", help="the colour for --flags custom")
    ap.add_argument("--insignia-color", metavar="#RRGGBB", help="the colour for --insignia custom")
    ap.add_argument("--off", action="store_true", help="remove the built-in pictures")
    ap.add_argument("--no-pack", action="store_true", help="make the pictures but don't build the pack")
    ap.add_argument("--status", action="store_true", help="show the settings and what was made")
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) - 1))
    args = ap.parse_args(argv)
    out = Path(args.output).expanduser()
    data = out / "data" / "cod3"
    tool_root = Path(__file__).resolve().parent.parent
    log = Log(sinks=[console_sink])
    try:
        if args.status or not (args.off or args.flags or args.insignia):
            print(status(data))
            return 0
        s = load_settings(data)
        if args.off:
            s.flags = s.insignia = "off"
        if args.flags:
            s.flags = args.flags
        if args.insignia:
            s.insignia = args.insignia
        if args.flags_color:
            s.flags_color = args.flags_color
        if args.insignia_color:
            s.insignia_color = args.insignia_color
        save_settings(data, s)
        if args.no_pack:
            apply(data, tool_root, s, log)
        else:
            build_pack(out, data, tool_root, args.jobs, Runner(log, threading.Event()), log)
        print(status(data))
        return 0
    except (SetupError, Cancelled) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
