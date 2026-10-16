"""The PC games' executables as the x86 decomp tools see them: PE32 images
mapped at their preferred base, with sections, imports and exports.

Where the files come from (never from the repository: they are the player's
own game files): the folders in the `IW_PC_INPUT` environment variable, or
`dev/decomp-input/pc/` in the project folder (private). The files keep their
original names (`CoD2MP_s.exe`, `CoDUOMP.exe` ...); they may sit in that
folder or in a subfolder of it (`uo/uo_game_mp_x86.dll`). Each image is
identified by its SHA-1, so names and addresses are only used with the exact
build they were found in.
"""
from __future__ import annotations

import bisect
import hashlib
import os
import struct
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

SCN_CODE = 0x20000000   # IMAGE_SCN_MEM_EXECUTE
SCN_WRITE = 0x80000000  # IMAGE_SCN_MEM_WRITE


@dataclass(frozen=True)
class KnownImage:
    key: str          # the name the tools use (--image)
    file_name: str    # the file as the game installs it
    sha1: str         # the build every name and address belongs to
    title: str        # for `/// Original:` lines: executable and version


# The builds the decomp supports. A different file with the same name is
# refused: its addresses would be wrong.
KNOWN_IMAGES = {
    k.key: k for k in (
        KnownImage("cod2mp", "CoD2MP_s.exe", "d1d264373cfdd117b9d57e441916c43dd51caa62", "CoD2MP_s.exe 1.3"),
        KnownImage("uomp", "CoDUOMP.exe", "31b21e8b55bb168dd2ce11e510a9cd2f47776c57", "CoDUOMP.exe 1.51"),
        KnownImage("codmp", "CoDMP.exe", "712c3e0b7766714e8f7d90f982bd1711ae5ac426", "CoDMP.exe 1.5"),
        KnownImage("uo_game_mp", "uo_game_mp_x86.dll", "00f008703631c3e62cfe554961eae59c306de33c",
                   "uo_game_mp_x86.dll 1.51"),
        KnownImage("cod2_gfx_mp", "gfx_d3d_mp_x86_s.dll", "559a85e55ce496b6fbbdc37e599f24ef8154f121",
                   "gfx_d3d_mp_x86_s.dll 1.3"),
    )
}


def input_dirs() -> list[Path]:
    """`IW_PC_INPUT` (several folders separated like PATH: the games may sit on different
    drives), then `dev/decomp-input/pc/`."""
    dirs = [Path(d) for d in os.environ.get("IW_PC_INPUT", "").split(os.pathsep) if d]
    dirs.append(ROOT / "dev" / "decomp-input" / "pc")
    return dirs


def find_input(key: str) -> Path:
    known = KNOWN_IMAGES.get(key)
    if known is None:
        raise SystemExit(f"Unknown image '{key}'. Known: {', '.join(KNOWN_IMAGES)}")
    wanted = known.file_name.lower()
    for d in input_dirs():
        if not d.is_dir():
            continue
        direct = d / known.file_name
        if direct.is_file():
            return direct
        # Any case, up to two folders down (`uo/`, `Main/`).
        for pattern in ("*", "*/*", "*/*/*"):
            for p in d.glob(pattern):
                if p.name.lower() == wanted and p.is_file():
                    return p
    where = " or ".join(str(d) for d in input_dirs())
    raise SystemExit(f"{known.file_name} not found in {where}.\n"
                     "Copy it from your own game folder (or set IW_PC_INPUT); see tools/decomp/x86/README.md.")


@dataclass
class Section:
    name: str
    start: int
    size: int       # virtual size: .data is mostly zero-fill past its raw bytes
    flags: int

    @property
    def end(self) -> int:
        return self.start + self.size


@dataclass
class PEImage:
    key: str
    title: str
    path: Path
    sha1: str
    base: int
    entry: int
    data: bytes                     # the whole image as mapped: headers, sections, zero fill
    sections: list[Section] = field(default_factory=list)
    imports: dict[int, str] = field(default_factory=dict)   # IAT slot address -> "dll!name"
    exports: dict[int, str] = field(default_factory=dict)   # address -> exported name
    names: dict[int, str] = field(default_factory=dict)     # address -> name (see names.py)
    addresses: dict[str, int] = field(default_factory=dict)  # name -> address
    lengths: dict[int, int] = field(default_factory=dict)   # function start -> length, where known

    # --- reading ----------------------------------------------------------
    def contains(self, address: int) -> bool:
        return self.base <= address < self.base + len(self.data)

    def read(self, address: int, size: int) -> bytes:
        o = address - self.base
        return self.data[o:o + size]

    def u32(self, address: int) -> int:
        return struct.unpack("<I", self.read(address, 4))[0]

    def f32(self, address: int) -> float:
        return struct.unpack("<f", self.read(address, 4))[0]

    def f64(self, address: int) -> float:
        return struct.unpack("<d", self.read(address, 8))[0]

    def cstr(self, address: int, limit: int = 256) -> bytes | None:
        if not self.contains(address):
            return None
        raw = self.read(address, limit)
        end = raw.find(b"\0")
        return raw[:end] if end >= 0 else None

    def section_at(self, address: int) -> Section | None:
        for s in self.sections:
            if s.start <= address < s.end:
                return s
        return None

    def is_code(self, address: int) -> bool:
        s = self.section_at(address)
        return bool(s and s.flags & SCN_CODE)

    def is_writable(self, address: int) -> bool:
        s = self.section_at(address)
        return bool(s and s.flags & SCN_WRITE)

    def code_ranges(self) -> list[tuple[int, int]]:
        return [(s.start, s.end) for s in self.sections if s.flags & SCN_CODE]

    # --- names ------------------------------------------------------------
    def add_name(self, address: int, name: str, length: int = 0) -> None:
        self.names.setdefault(address, name)
        self.addresses.setdefault(name, address)
        if length:
            self.lengths.setdefault(address, length)

    def describe(self, address: int) -> str:
        """`Name+0x10` for an address after a named function start (within its length when known)."""
        if not hasattr(self, "_sorted_names") or len(self._sorted_names) != len(self.names):
            self._sorted_names = sorted(a for a in self.names if self.is_code(a))
        i = bisect.bisect_right(self._sorted_names, address) - 1
        if i >= 0:
            start = self._sorted_names[i]
            length = self.lengths.get(start, 0x1000)
            if start <= address < start + length:
                name = self.names[start]
                return name if address == start else f"{name}+0x{address - start:X}"
        return self.names.get(address, f"0x{address:08X}")

    def resolve(self, what: str) -> int:
        """A name, `sub_XXXXXXXX`, or a hex address."""
        if what in self.addresses:
            return self.addresses[what]
        text = what[4:] if what.startswith("sub_") else what
        try:
            return int(text, 16)
        except ValueError:
            raise SystemExit(f"'{what}' is neither a known name in {self.key} nor an address") from None


def _parse(image: PEImage, raw: bytes) -> None:
    if raw[:2] != b"MZ":
        raise SystemExit(f"{image.path} is not a PE file")
    pe = struct.unpack_from("<I", raw, 0x3C)[0]
    if raw[pe:pe + 4] != b"PE\0\0":
        raise SystemExit(f"{image.path} is not a PE file")
    machine, count = struct.unpack_from("<HH", raw, pe + 4)
    if machine != 0x14C:
        raise SystemExit(f"{image.path} is not a 32-bit x86 image (machine 0x{machine:X})")
    optional_size = struct.unpack_from("<H", raw, pe + 20)[0]
    opt = pe + 24
    if struct.unpack_from("<H", raw, opt)[0] != 0x10B:
        raise SystemExit(f"{image.path} is not PE32")
    image.entry = struct.unpack_from("<I", raw, opt + 16)[0]
    image.base = struct.unpack_from("<I", raw, opt + 28)[0]
    size_of_image, size_of_headers = struct.unpack_from("<II", raw, opt + 56)
    directories = [struct.unpack_from("<II", raw, opt + 96 + 8 * i) for i in range(16)]
    data = bytearray(size_of_image)
    data[:size_of_headers] = raw[:size_of_headers]
    s = opt + optional_size
    for _ in range(count):
        name = raw[s:s + 8].rstrip(b"\0").decode("ascii", "replace")
        vsize, vaddr, raw_size, raw_ptr = struct.unpack_from("<IIII", raw, s + 8)
        flags = struct.unpack_from("<I", raw, s + 36)[0]
        n = min(raw_size, vsize or raw_size)
        data[vaddr:vaddr + n] = raw[raw_ptr:raw_ptr + n]
        image.sections.append(Section(name, image.base + vaddr, vsize or raw_size, flags))
        s += 40
    image.data = bytes(data)
    image.entry += image.base
    _parse_imports(image, directories[1])
    _parse_exports(image, directories[0])


def _cstr_at(data: bytes, offset: int) -> str:
    end = data.find(b"\0", offset)
    return data[offset:end].decode("latin-1")


def _parse_imports(image: PEImage, directory: tuple[int, int]) -> None:
    rva, size = directory
    if not rva:
        return
    d = image.data
    for desc in range(rva, rva + size, 20):
        lookup, _stamp, _chain, name_rva, iat = struct.unpack_from("<IIIII", d, desc)
        if not name_rva:
            break
        dll = _cstr_at(d, name_rva).lower()
        table = lookup or iat
        i = 0
        while True:
            entry = struct.unpack_from("<I", d, table + 4 * i)[0]
            if not entry:
                break
            if entry & 0x80000000:
                what = f"#{entry & 0xFFFF}"
            else:
                what = _cstr_at(d, entry + 2)
            image.imports[image.base + iat + 4 * i] = f"{dll}!{what}"
            i += 1


def _parse_exports(image: PEImage, directory: tuple[int, int]) -> None:
    rva, size = directory
    if not rva:
        return
    d = image.data
    (_flags, _stamp, _ver, _name, ordinal_base, n_functions, n_names, functions, names,
     ordinals) = struct.unpack_from("<IIIIIIIIII", d, rva)
    for i in range(n_names):
        name = _cstr_at(d, struct.unpack_from("<I", d, names + 4 * i)[0])
        index = struct.unpack_from("<H", d, ordinals + 2 * i)[0]
        target = struct.unpack_from("<I", d, functions + 4 * index)[0]
        if not rva <= target < rva + size:  # not a forwarder
            image.exports[image.base + target] = name


_CACHE: dict[str, PEImage] = {}


def load(key: str, with_names: bool = True) -> PEImage:
    """Reads and parses one of the supported images (once per process)."""
    if key in _CACHE:
        return _CACHE[key]
    known = KNOWN_IMAGES.get(key)
    if known is None:
        raise SystemExit(f"Unknown image '{key}'. Known: {', '.join(KNOWN_IMAGES)}")
    path = find_input(key)
    raw = path.read_bytes()
    digest = hashlib.sha1(raw).hexdigest()
    if digest != known.sha1:
        raise SystemExit(f"{path} is not the supported build of {known.file_name} (SHA-1 {digest}, "
                         f"expected {known.sha1}): its addresses would not match the names.")
    image = PEImage(key, known.title, path, digest, 0, 0, b"")
    _parse(image, raw)
    for address, name in image.exports.items():
        image.add_name(address, name)
    if with_names:
        import names
        names.apply(image)
    _CACHE[key] = image
    return image
