"""The project's own font for the setup tool's tabs, main buttons and Call
of Duty 3's title: Barlow Semi Condensed SemiBold (SIL Open Font License
1.1), the font of the game's F2 window. The game embeds it in
recomp/src/ui/fonts/barlow_semibold.inc as Dear ImGui's compressed base85
text (misc/fonts/binary_to_compressed_c); this module turns that back into
the .ttf and lets this program, and only it, use the font:

- Windows: AddFontResourceExW with FR_PRIVATE;
- Linux: fontconfig's FcConfigAppFontAddFile (this process's own fonts).

The .ttf (with its licence) is written once to config/fonts in the project
folder. Nothing is downloaded or installed into the system. If anything
fails, the tool keeps to system fonts (theme.DISPLAY_FONTS).

No Tk is needed to decode, so the tests check the decoder without a window.
"""
from __future__ import annotations

import os
import zlib
from pathlib import Path
from typing import Optional

from .common import IS_WINDOWS

SOURCE = Path("recomp") / "src" / "ui" / "fonts" / "barlow_semibold.inc"
LICENSE = SOURCE.with_name("OFL.txt")
TTF_NAME = "BarlowSemiCondensed-SemiBold.ttf"
# The names a system may list it under: Windows (GDI) keeps the weight in
# the family's name; fontconfig lists the family and the weight apart.
FAMILIES = (("Barlow Semi Condensed SemiBold", "normal"), ("Barlow Semi Condensed", "bold"))


def decode85(text: str) -> bytes:
    """Dear ImGui's base85 (Decode85): 5 characters to 4 bytes, little-endian."""
    data = text.encode("ascii")
    out = bytearray()
    for i in range(0, len(data) - len(data) % 5, 5):
        value = 0
        for ch in reversed(data[i:i + 5]):
            value = value * 85 + (ch - 36 if ch >= 0x5C else ch - 35)   # 0x5C: backslash isn't used
        out += (value & 0xFFFFFFFF).to_bytes(4, "little")
    return bytes(out)


def stb_decompress(src: bytes) -> bytes:
    """stb_compress's format, as Dear ImGui's stb_decompress reads it: a
    16-byte header (signature, length), then literal runs and back
    references, then an Adler-32 of the output. Raises ValueError on
    anything that doesn't fit."""
    def in2(i: int) -> int:
        return (src[i] << 8) | src[i + 1]

    def in3(i: int) -> int:
        return (src[i] << 16) | in2(i + 1)

    def in4(i: int) -> int:
        return (src[i] << 24) | in3(i + 1)

    if len(src) < 18 or in4(0) != 0x57BC0000 or in4(4) != 0:
        raise ValueError("not stb_compress data")
    size = in4(8)
    out = bytearray()

    def match(back: int, length: int) -> None:
        start = len(out) - back
        if start < 0 or len(out) + length > size:
            raise ValueError("bad back reference")
        if back >= length:
            out.extend(out[start:start + length])
        else:                               # overlapping: byte by byte, as the original does
            for k in range(length):
                out.append(out[start + k])

    def literal(at: int, length: int) -> None:
        if len(out) + length > size or at + length > len(src):
            raise ValueError("bad literal run")
        out.extend(src[at:at + length])

    i = 16
    while True:
        if i >= len(src):
            raise ValueError("no end marker")
        c = src[i]
        if c >= 0x80:
            match(src[i + 1] + 1, c - 0x80 + 1)
            i += 2
        elif c >= 0x40:
            match(in2(i) - 0x4000 + 1, src[i + 2] + 1)
            i += 3
        elif c >= 0x20:
            n = c - 0x20 + 1
            literal(i + 1, n)
            i += 1 + n
        elif c >= 0x18:
            match(in3(i) - 0x180000 + 1, src[i + 3] + 1)
            i += 4
        elif c >= 0x10:
            match(in3(i) - 0x100000 + 1, in2(i + 3) + 1)
            i += 5
        elif c >= 0x08:
            n = in2(i) - 0x0800 + 1
            literal(i + 2, n)
            i += 2 + n
        elif c == 0x07:
            n = in2(i + 1) + 1
            literal(i + 3, n)
            i += 3 + n
        elif c == 0x06:
            match(in3(i + 1) + 1, src[i + 4] + 1)
            i += 5
        elif c == 0x04:
            match(in3(i + 1) + 1, in2(i + 4) + 1)
            i += 6
        elif c == 0x05 and src[i + 1] == 0xFA:
            break
        else:
            raise ValueError("unknown token")
    if len(out) != size or zlib.adler32(bytes(out)) != in4(i + 2):
        raise ValueError("checksum doesn't match")
    return bytes(out)


def font_from_inc(text: str) -> bytes:
    """The font file in an ImGui `..._compressed_data_base85` source file."""
    start = text.index("=", text.index("compressed_data_base85"))
    # "??" is written "?\?" there (old compilers read ??x as a trigraph).
    body = "".join(part for part in _c_strings(text[start:])).replace("\\?", "?")
    data = stb_decompress(decode85(body))
    if data[:4] not in (b"\x00\x01\x00\x00", b"OTTO", b"true"):
        raise ValueError("not a font file")
    return data


def _c_strings(text: str):
    """The contents of the C string literals in `text`, in order (the
    base85 alphabet has no double quote, and its only escape is \\?)."""
    pos = 0
    while True:
        a = text.find('"', pos)
        if a < 0:
            return
        b = text.find('"', a + 1)
        if b < 0:
            return
        yield text[a + 1:b]
        pos = b + 1


def ttf_path(config_dir: Path) -> Path:
    return Path(config_dir) / "fonts" / TTF_NAME


def write_font(tool_root: Path, config_dir: Path) -> Optional[Path]:
    """The .ttf in config/fonts, made from the project's copy when it isn't
    there yet. None when the project has no copy."""
    src = Path(tool_root) / SOURCE
    dest = ttf_path(config_dir)
    if dest.is_file() and dest.stat().st_size > 1000:
        return dest
    if not src.is_file():
        return None
    data = font_from_inc(src.read_text(encoding="ascii", errors="replace"))
    dest.parent.mkdir(parents=True, exist_ok=True)
    tmp = dest.with_name(dest.name + ".partial")
    tmp.write_bytes(data)
    os.replace(tmp, dest)
    lic = Path(tool_root) / LICENSE
    if lic.is_file():
        (dest.parent / "OFL.txt").write_bytes(lic.read_bytes())
    return dest


def register(path: Path) -> bool:
    """Makes the font usable by this process only. False when the system
    doesn't allow it."""
    import ctypes
    if IS_WINDOWS:
        fr_private = 0x10
        return ctypes.windll.gdi32.AddFontResourceExW(str(path), fr_private, 0) > 0
    lib = None
    for name in ("libfontconfig.so.1", "libfontconfig.so"):
        try:
            lib = ctypes.CDLL(name)
            break
        except OSError:
            continue
    if lib is None:
        return False
    lib.FcConfigAppFontAddFile.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
    lib.FcConfigAppFontAddFile.restype = ctypes.c_int
    return bool(lib.FcConfigAppFontAddFile(None, os.fsencode(str(path))))


def load_project_font(root, tool_root: Path, config_dir: Path) -> Optional[tuple]:
    """Loads Barlow Semi Condensed SemiBold for this window; returns its
    (family, weight) as Tk knows it, or None (then system fonts are used)."""
    import tkinter.font as tkfont
    try:
        have = {f.lower(): f for f in tkfont.families(root)}
        for family, weight in FAMILIES:          # already installed on this system
            if family.lower() in have:
                return have[family.lower()], weight
        path = write_font(tool_root, config_dir)
        if path is None or not register(path):
            return None
        have = {f.lower(): f for f in tkfont.families(root)}
        for family, weight in FAMILIES:
            if family.lower() in have:
                return have[family.lower()], weight
    except Exception:  # noqa: BLE001 - any failure: system fonts
        return None
    return None
